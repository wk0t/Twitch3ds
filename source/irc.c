// irc.c: the Twitch chat. reader thread + circular queue, auto-reconnect, anonymous fallback if login fails.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>

#include "irc.h"

#define IRC_HOST      "irc.chat.twitch.tv"
#define IRC_PORT      "6667"
#define IRC_QUEUE_LEN 128
#define IRC_LINE_MAX  4096
#define IRC_STACK     (32 * 1024)
#define IRC_MAX_RETRY 5
#define IRC_PRIO      0x2C

/* why reader_loop returned */
enum {
    ACT_NONE = 0,
    ACT_DISCONNECT,   /* dead socket / network error */
    ACT_AUTHFAIL,     /* login refused */
    ACT_RECONNECT,    /* Twitch asks us to reconnect */
};

static struct {
    Thread th;
    bool   running;

    volatile bool quit;
    volatile bool connected;    /* we got the 001 */
    volatile bool authed;       /* logged in with the account (not justinfan) */
    bool use_auth;              /* tenter PASS/NICK au prochain connect */
    bool dc_announced;          /* avoids re-posting "Disconnected" in a loop */

    int  sock;                  /* -1 if closed, guarded by slock */
    char channel[64];           /* without the '#', lowercase */
    char username[64];
    char oauth[160];            /* "oauth:xxxx" */

    LightLock qlock;            /* the queue */
    LightLock slock;            /* sock + envois */
    ChatMsg queue[IRC_QUEUE_LEN];
    int qhead, qcount;
} s = { .sock = -1 };

/* only the reader thread touches it, so a static global is ok */
static char s_acc[IRC_LINE_MAX];
static char s_rbuf[2048];

/* ---------------------------------------------------------------- utils -- */

/* home-made strlcpy: on overflow, back up so we do not split a UTF-8 char in two */
static void scopy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void push_msg(const char *nick, const char *color, const char *text, bool sys)
{
    LightLock_Lock(&s.qlock);
    ChatMsg *m = &s.queue[(s.qhead + s.qcount) % IRC_QUEUE_LEN];
    scopy(m->nick,  sizeof(m->nick),  nick  ? nick  : "");
    scopy(m->color, sizeof(m->color), color ? color : "");
    scopy(m->text,  sizeof(m->text),  text  ? text  : "");
    m->is_system = sys;
    if (s.qcount == IRC_QUEUE_LEN)
        s.qhead = (s.qhead + 1) % IRC_QUEUE_LEN;   /* queue full: drop the oldest */
    else
        s.qcount++;
    LightLock_Unlock(&s.qlock);
}

static void push_system(const char *text)
{
    push_msg("", "", text, true);
}

// full send under slock. non-blocking socket so we loop on EAGAIN,
// with a budget (~2s) so we do not get stuck here if it goes sideways.
static bool send_raw(const char *data)
{
    size_t len = strlen(data), off = 0;
    int budget = 400;

    LightLock_Lock(&s.slock);
    bool ok = (s.sock >= 0);
    while (ok && off < len && !s.quit) {
        ssize_t n = send(s.sock, data + off, len - off, 0);
        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (--budget <= 0) { ok = false; break; }
            svcSleepThread(5 * 1000 * 1000LL);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            ok = false;
        }
    }
    LightLock_Unlock(&s.slock);
    return ok && off == len;
}

/* ------------------------------------------------------------ IRC parse -- */

// fetches an IRCv3 tag (display-name, color...) unescaping \s \: \\ \r \n
static void tag_get(const char *tags, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    out[0] = '\0';

    const char *p = tags;
    while (p && *p) {
        const char *end = strchr(p, ';');
        size_t seg = end ? (size_t)(end - p) : strlen(p);

        if (seg > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t vlen = seg - klen - 1, o = 0;
            /* unescape into a local buffer then scopy to cut cleanly:
             * a bogus nick must not leave truncated UTF-8 in out */
            char tmp[512];
            for (size_t i = 0; i < vlen && o + 1 < sizeof(tmp); i++) {
                char c = v[i];
                if (c == '\\') {
                    if (i + 1 >= vlen) break;   /* lone backslash at the end */
                    switch (v[++i]) {
                        case 's':  c = ' ';   break;
                        case ':':  c = ';';   break;
                        case 'r':  c = '\r';  break;
                        case 'n':  c = '\n';  break;
                        case '\\': c = '\\';  break;
                        default:   c = v[i];  break;
                    }
                }
                tmp[o++] = c;
            }
            tmp[o] = '\0';
            scopy(out, cap, tmp);
            return;
        }
        p = end ? end + 1 : NULL;
    }
}

static void handle_privmsg(const char *tags, const char *prefix, char *params)
{
    /* params = "#chan :message", the text starts after the ':' */
    char *text = strchr(params, ':');
    if (!text)
        return;
    text++;

    /* a /me arrives wrapped in \x01ACTION ... \x01, unwrap it */
    size_t tlen = strlen(text);
    if (tlen >= 9 && strncmp(text, "\001ACTION ", 8) == 0) {
        text += 8;
        tlen -= 8;
        if (tlen > 0 && text[tlen - 1] == '\001')
            text[tlen - 1] = '\0';
    }

    char nick[IRC_NICK_MAX] = "";
    char color[8] = "";
    if (tags) {
        tag_get(tags, "display-name", nick, sizeof(nick));
        tag_get(tags, "color", color, sizeof(color));
    }
    if (!nick[0] && prefix) {
        /* no display-name: take the login from "login!login@..." */
        const char *bang = strchr(prefix, '!');
        size_t n = bang ? (size_t)(bang - prefix) : strlen(prefix);
        if (n >= sizeof(nick)) n = sizeof(nick) - 1;
        memcpy(nick, prefix, n);
        nick[n] = '\0';
    }

    push_msg(nick, color, text, false);
}

/* handles one complete line (without \r\n). returns an ACT_*. */
static int handle_line(char *line)
{
    /* the PONG must go out immediately, before parsing anything */
    if (strncmp(line, "PING", 4) == 0) {
        char pong[256];
        snprintf(pong, sizeof(pong), "PONG%.240s\r\n", line + 4);
        send_raw(pong);
        return ACT_NONE;
    }

    char *p = line;
    char *tags = NULL, *prefix = NULL;

    if (*p == '@') {                       /* @tags ... */
        char *sp = strchr(p, ' ');
        if (!sp) return ACT_NONE;
        *sp = '\0';
        tags = p + 1;
        p = sp + 1;
    }
    if (*p == ':') {                       /* :prefixe ... */
        char *sp = strchr(p, ' ');
        if (!sp) return ACT_NONE;
        *sp = '\0';
        prefix = p + 1;
        p = sp + 1;
    }

    char *cmd = p;
    char *params = strchr(p, ' ');
    if (params)
        *params++ = '\0';

    if (strcmp(cmd, "001") == 0) {
        /* login ok */
        s.authed = s.use_auth;
        s.connected = true;
        s.dc_announced = false;
        push_system("Connected to chat");
    } else if (strcmp(cmd, "PRIVMSG") == 0 && params) {
        handle_privmsg(tags, prefix, params);
    } else if (strcmp(cmd, "NOTICE") == 0 && params && !s.connected &&
               (strstr(params, "authentication failed") ||
                strstr(params, "Improperly formatted"))) {
        return ACT_AUTHFAIL;               /* bad token -> fall back to anonymous */
    } else if (strcmp(cmd, "RECONNECT") == 0) {
        return ACT_RECONNECT;
    }
    return ACT_NONE;
}

/* --------------------------------------------------------------- network -- */

// deliberately BLOCKING connect: on the 3DS SOC, non-blocking + select does not
// work (connect returns EINPROGRESS and select never signals the socket ready).
// we publish the fd in s.sock so irc_stop can kill it, and switch to non-blocking AFTER.
static int irc_open_socket(void)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(IRC_HOST, IRC_PORT, &hints, &res) != 0 || !res)
        return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (s.quit)
            break;
        int f = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (f < 0)
            continue;

        LightLock_Lock(&s.slock);           /* publish the fd before blocking on connect */
        s.sock = f;
        LightLock_Unlock(&s.slock);

        int rc = connect(f, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            int fl = fcntl(f, F_GETFL, 0);  /* switch to non-blocking for reading */
            if (fl >= 0)
                fcntl(f, F_SETFL, fl | O_NONBLOCK);
            fd = f;
            break;
        }

        LightLock_Lock(&s.slock);
        if (s.sock == f)
            s.sock = -1;
        LightLock_Unlock(&s.slock);
        close(f);
    }
    freeaddrinfo(res);
    return fd;
}

static bool send_handshake(void)
{
    char buf[256];

    if (!send_raw("CAP REQ :twitch.tv/tags twitch.tv/commands\r\n"))
        return false;

    if (s.use_auth) {
        snprintf(buf, sizeof(buf), "PASS %s\r\n", s.oauth);
        if (!send_raw(buf))
            return false;
        snprintf(buf, sizeof(buf), "NICK %s\r\n", s.username);
        if (!send_raw(buf))
            return false;
    } else {
        snprintf(buf, sizeof(buf), "NICK justinfan%05u\r\n",
                 (unsigned)(osGetTime() % 100000));
        if (!send_raw(buf))
            return false;
    }

    snprintf(buf, sizeof(buf), "JOIN #%s\r\n", s.channel);
    return send_raw(buf);
}

// reads the socket, reassembles lines up to \r\n. overlong lines are dropped.
static int reader_loop(int fd)
{
    size_t acc_len = 0;
    bool overflow = false;

    while (!s.quit) {
        ssize_t n = recv(fd, s_rbuf, sizeof(s_rbuf), 0);
        if (n == 0)
            return ACT_DISCONNECT;         /* the server closed */
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                svcSleepThread(30 * 1000 * 1000LL);   /* nothing to read, breathe for 30 ms */
                continue;
            }
            if (errno == EINTR)
                continue;
            return ACT_DISCONNECT;
        }

        for (ssize_t i = 0; i < n; i++) {
            char c = s_rbuf[i];
            if (c == '\n') {
                if (!overflow && acc_len > 0) {
                    if (s_acc[acc_len - 1] == '\r')
                        acc_len--;
                    s_acc[acc_len] = '\0';
                    if (acc_len > 0) {
                        int act = handle_line(s_acc);
                        if (act != ACT_NONE)
                            return act;
                    }
                }
                acc_len = 0;
                overflow = false;
            } else if (acc_len < sizeof(s_acc) - 1) {
                s_acc[acc_len++] = c;
            } else {
                overflow = true;           /* line too long, trash it */
            }
        }
    }
    return ACT_NONE;
}

/* 2 s nap, cut short if quit goes true */
static void backoff_sleep(void)
{
    for (int i = 0; i < 20 && !s.quit; i++)
        svcSleepThread(100 * 1000 * 1000LL);
}

static void irc_thread(void *arg)
{
    (void)arg;
    int attempts = 0;

    while (!s.quit && attempts < IRC_MAX_RETRY) {
        int fd = irc_open_socket();
        if (fd < 0) {
            attempts++;
            if (!s.quit && attempts < IRC_MAX_RETRY)
                backoff_sleep();
            continue;
        }

        LightLock_Lock(&s.slock);
        s.sock = fd;
        LightLock_Unlock(&s.slock);

        int why = ACT_DISCONNECT;
        if (send_handshake())
            why = reader_loop(fd);

        /* close under lock so irc_send never lands on an already-dead fd */
        LightLock_Lock(&s.slock);
        if (s.sock >= 0) {
            close(s.sock);
            s.sock = -1;
        }
        LightLock_Unlock(&s.slock);

        bool was_logged = s.connected;
        s.connected = false;
        s.authed = false;

        if (s.quit)
            break;

        if (why == ACT_AUTHFAIL) {
            /* the token fails: fall back to anonymous (read-only) right away */
            s.use_auth = false;
            push_system("Authentication failed, read-only");
            attempts = 0;
            continue;
        }

        if (was_logged) {
            push_system("Disconnected");
            s.dc_announced = true;
            attempts = 0;                  /* we had been connected: reset the counter */
        }
        attempts++;
        if (attempts < IRC_MAX_RETRY)
            backoff_sleep();
    }

    if (!s.quit && !s.dc_announced)
        push_system("Disconnected");         /* giving up after too many failures */
}

/* ------------------------------------------------------------ API irc.h -- */

Result irc_start(const char *channel, const char *username, const char *oauth)
{
    if (!channel || !channel[0])
        return -1;

    irc_stop();                            /* we are a singleton: start clean */

    LightLock_Init(&s.qlock);
    LightLock_Init(&s.slock);
    s.quit = false;
    s.connected = false;
    s.authed = false;
    s.dc_announced = false;
    s.sock = -1;
    s.qhead = s.qcount = 0;

    /* normalize the channel: lowercase, without the '#' */
    if (channel[0] == '#')
        channel++;
    size_t i = 0;
    for (; channel[i] && i < sizeof(s.channel) - 1; i++)
        s.channel[i] = (char)tolower((unsigned char)channel[i]);
    s.channel[i] = '\0';
    if (!s.channel[0])
        return -1;

    s.use_auth = username && username[0] && oauth && oauth[0];
    if (s.use_auth) {
        for (i = 0; username[i] && i < sizeof(s.username) - 1; i++)
            s.username[i] = (char)tolower((unsigned char)username[i]);
        s.username[i] = '\0';
        if (strncmp(oauth, "oauth:", 6) == 0)
            scopy(s.oauth, sizeof(s.oauth), oauth);
        else
            snprintf(s.oauth, sizeof(s.oauth), "oauth:%s", oauth);
    } else {
        s.username[0] = '\0';
        s.oauth[0] = '\0';
    }

    push_system("Connecting to chat...");

    s.th = threadCreate(irc_thread, NULL, IRC_STACK, IRC_PRIO, 1, false);
    if (!s.th)                             /* core 1 full, try the app core */
        s.th = threadCreate(irc_thread, NULL, IRC_STACK, IRC_PRIO, 0, false);
    if (!s.th)                             /* still no: any free core */
        s.th = threadCreate(irc_thread, NULL, IRC_STACK, IRC_PRIO, -2, false);
    if (!s.th) {
        LightLock_Lock(&s.qlock);
        s.qhead = s.qcount = 0;
        LightLock_Unlock(&s.qlock);
        return -1;
    }

    s.running = true;
    return 0;
}

void irc_stop(void)
{
    if (!s.running)
        return;

    s.quit = true;

    /* shutdown to unblock the recv/send possibly running in the thread */
    LightLock_Lock(&s.slock);
    if (s.sock >= 0)
        shutdown(s.sock, SHUT_RDWR);
    LightLock_Unlock(&s.slock);

    threadJoin(s.th, U64_MAX);
    threadFree(s.th);
    s.th = NULL;

    if (s.sock >= 0) {                     /* normally the thread already closed it, just in case */
        close(s.sock);
        s.sock = -1;
    }

    s.running = false;
    s.connected = false;
    s.authed = false;
    s.use_auth = false;

    LightLock_Lock(&s.qlock);
    s.qhead = s.qcount = 0;
    LightLock_Unlock(&s.qlock);
}

bool irc_connected(void)
{
    return s.connected;
}

bool irc_can_send(void)
{
    return s.connected && s.authed;
}

int irc_poll(ChatMsg *out, int max)
{
    if (!out || max <= 0)
        return 0;

    LightLock_Lock(&s.qlock);
    int n = 0;
    while (n < max && s.qcount > 0) {
        out[n++] = s.queue[s.qhead];
        s.qhead = (s.qhead + 1) % IRC_QUEUE_LEN;
        s.qcount--;
    }
    LightLock_Unlock(&s.qlock);
    return n;
}

bool irc_send(const char *text)
{
    if (!text || !text[0] || !irc_can_send())
        return false;

    /* replace CR/LF with spaces, else we could inject IRC commands */
    char clean[IRC_TEXT_MAX];
    size_t o = 0, i = 0;
    for (; text[i] && o + 1 < sizeof(clean); i++)
        clean[o++] = (text[i] == '\r' || text[i] == '\n') ? ' ' : text[i];
    if (text[i]) {
        /* it overflowed: rewind so we do not leave a cut UTF-8 character */
        while (o > 0 && ((unsigned char)clean[o - 1] & 0xC0) == 0x80)
            o--;
        if (o > 0 && ((unsigned char)clean[o - 1] & 0x80))
            o--;                           /* lone lead byte */
    }
    clean[o] = '\0';

    char buf[IRC_TEXT_MAX + 96];
    snprintf(buf, sizeof(buf), "PRIVMSG #%s :%s\r\n", s.channel, clean);
    if (!send_raw(buf))
        return false;

    /* Twitch does not send our own messages back, so local echo */
    push_msg(s.username, "#9146FF", clean, false);
    return true;
}
