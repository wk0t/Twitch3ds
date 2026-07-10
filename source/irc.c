// irc.c : le chat Twitch. thread lecteur + file circulaire, reco auto, repli anonyme si le login foire.
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

/* pourquoi reader_loop a rendu la main */
enum {
    ACT_NONE = 0,
    ACT_DISCONNECT,   /* socket morte / erreur réseau */
    ACT_AUTHFAIL,     /* login refusé */
    ACT_RECONNECT,    /* Twitch nous demande de reconnecter */
};

static struct {
    Thread th;
    bool   running;

    volatile bool quit;
    volatile bool connected;    /* on a reçu le 001 */
    volatile bool authed;       /* loggué avec le compte (pas justinfan) */
    bool use_auth;              /* tenter PASS/NICK au prochain connect */
    bool dc_announced;          /* évite de re-poster "Déconnecté" en boucle */

    int  sock;                  /* -1 si fermé, protégé par slock */
    char channel[64];           /* sans le '#', en minuscules */
    char username[64];
    char oauth[160];            /* "oauth:xxxx" */

    LightLock qlock;            /* la file */
    LightLock slock;            /* sock + envois */
    ChatMsg queue[IRC_QUEUE_LEN];
    int qhead, qcount;
} s = { .sock = -1 };

/* que le thread lecteur y touche, donc static global ok */
static char s_acc[IRC_LINE_MAX];
static char s_rbuf[2048];

/* ---------------------------------------------------------------- utils -- */

/* strlcpy maison : si ça déborde, on recule pour pas couper un char UTF-8 en deux */
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
        s.qhead = (s.qhead + 1) % IRC_QUEUE_LEN;   /* file pleine : on vire le plus vieux */
    else
        s.qcount++;
    LightLock_Unlock(&s.qlock);
}

static void push_system(const char *text)
{
    push_msg("", "", text, true);
}

// envoi complet sous slock. socket non-bloquante donc on boucle sur EAGAIN,
// avec un budget (~2s) pour pas rester coincé ici si ça part en vrille.
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

/* ------------------------------------------------------------ parse IRC -- */

// récupère un tag IRCv3 (display-name, color...) en déséchappant \s \: \\ \r \n
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
            /* on déséchappe dans un buffer local puis scopy pour couper propre :
             * un pseudo bidon ne doit pas laisser d'UTF-8 tronqué dans out */
            char tmp[512];
            for (size_t i = 0; i < vlen && o + 1 < sizeof(tmp); i++) {
                char c = v[i];
                if (c == '\\') {
                    if (i + 1 >= vlen) break;   /* backslash tout seul à la fin */
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
    /* params = "#chan :message", le texte commence après le ':' */
    char *text = strchr(params, ':');
    if (!text)
        return;
    text++;

    /* un /me arrive emballé dans \x01ACTION ... \x01, on déballe */
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
        /* pas de display-name : on prend le login dans "login!login@..." */
        const char *bang = strchr(prefix, '!');
        size_t n = bang ? (size_t)(bang - prefix) : strlen(prefix);
        if (n >= sizeof(nick)) n = sizeof(nick) - 1;
        memcpy(nick, prefix, n);
        nick[n] = '\0';
    }

    push_msg(nick, color, text, false);
}

/* traite une ligne complète (sans le \r\n). rend un ACT_*. */
static int handle_line(char *line)
{
    /* le PONG doit partir tout de suite, avant de parser quoi que ce soit */
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
        push_system("Connecté au chat");
    } else if (strcmp(cmd, "PRIVMSG") == 0 && params) {
        handle_privmsg(tags, prefix, params);
    } else if (strcmp(cmd, "NOTICE") == 0 && params && !s.connected &&
               (strstr(params, "authentication failed") ||
                strstr(params, "Improperly formatted"))) {
        return ACT_AUTHFAIL;               /* token pourri -> on repasse anonyme */
    } else if (strcmp(cmd, "RECONNECT") == 0) {
        return ACT_RECONNECT;
    }
    return ACT_NONE;
}

/* --------------------------------------------------------------- réseau -- */

// connect BLOQUANT exprès : sur le SOC du 3DS le non-bloquant + select marche pas
// (connect rend EINPROGRESS et select signale jamais le socket prêt). on publie le
// fd dans s.sock pour qu'irc_stop puisse le tuer, et on passe non-bloquant APRES.
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

        LightLock_Lock(&s.slock);           /* publie le fd avant de bloquer sur connect */
        s.sock = f;
        LightLock_Unlock(&s.slock);

        int rc = connect(f, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            int fl = fcntl(f, F_GETFL, 0);  /* passe non-bloquant pour la lecture */
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

// lit le socket, recompose les lignes jusqu'au \r\n. les lignes trop longues sont jetées.
static int reader_loop(int fd)
{
    size_t acc_len = 0;
    bool overflow = false;

    while (!s.quit) {
        ssize_t n = recv(fd, s_rbuf, sizeof(s_rbuf), 0);
        if (n == 0)
            return ACT_DISCONNECT;         /* le serveur a fermé */
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                svcSleepThread(30 * 1000 * 1000LL);   /* rien à lire, on souffle 30 ms */
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
                overflow = true;           /* ligne trop longue, poubelle */
            }
        }
    }
    return ACT_NONE;
}

/* dodo de 2s, coupé net si quit passe à true */
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

        /* on ferme sous verrou pour qu'irc_send tombe jamais sur un fd déjà mort */
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
            /* le token passe pas : on rebascule direct en anonyme (lecture seule) */
            s.use_auth = false;
            push_system("Échec d'authentification, lecture seule");
            attempts = 0;
            continue;
        }

        if (was_logged) {
            push_system("Déconnecté");
            s.dc_announced = true;
            attempts = 0;                  /* on avait été connecté : compteur remis à zéro */
        }
        attempts++;
        if (attempts < IRC_MAX_RETRY)
            backoff_sleep();
    }

    if (!s.quit && !s.dc_announced)
        push_system("Déconnecté");         /* on abandonne après trop d'échecs */
}

/* ------------------------------------------------------------ API irc.h -- */

Result irc_start(const char *channel, const char *username, const char *oauth)
{
    if (!channel || !channel[0])
        return -1;

    irc_stop();                            /* on est un singleton : on repart propre */

    LightLock_Init(&s.qlock);
    LightLock_Init(&s.slock);
    s.quit = false;
    s.connected = false;
    s.authed = false;
    s.dc_announced = false;
    s.sock = -1;
    s.qhead = s.qcount = 0;

    /* normalise le canal : minuscules et sans le '#' */
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

    push_system("Connexion au chat...");

    s.th = threadCreate(irc_thread, NULL, IRC_STACK, IRC_PRIO, 1, false);
    if (!s.th)                             /* core 1 plein, on tente le core appli */
        s.th = threadCreate(irc_thread, NULL, IRC_STACK, IRC_PRIO, 0, false);
    if (!s.th)                             /* toujours pas : n'importe quel coeur libre */
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

    /* shutdown pour débloquer le recv/send qui tourne peut-être dans le thread */
    LightLock_Lock(&s.slock);
    if (s.sock >= 0)
        shutdown(s.sock, SHUT_RDWR);
    LightLock_Unlock(&s.slock);

    threadJoin(s.th, U64_MAX);
    threadFree(s.th);
    s.th = NULL;

    if (s.sock >= 0) {                     /* normalement le thread l'a déjà fermé, au cas où */
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

    /* on remplace les CR/LF par des espaces, sinon on peut injecter des commandes IRC */
    char clean[IRC_TEXT_MAX];
    size_t o = 0, i = 0;
    for (; text[i] && o + 1 < sizeof(clean); i++)
        clean[o++] = (text[i] == '\r' || text[i] == '\n') ? ' ' : text[i];
    if (text[i]) {
        /* ça a débordé : on rembobine pour pas laisser un caractère UTF-8 coupé */
        while (o > 0 && ((unsigned char)clean[o - 1] & 0xC0) == 0x80)
            o--;
        if (o > 0 && ((unsigned char)clean[o - 1] & 0x80))
            o--;                           /* octet de tête tout seul */
    }
    clean[o] = '\0';

    char buf[IRC_TEXT_MAX + 96];
    snprintf(buf, sizeof(buf), "PRIVMSG #%s :%s\r\n", s.channel, clean);
    if (!send_raw(buf))
        return false;

    /* Twitch nous renvoie pas nos propres messages, donc écho local */
    push_msg(s.username, "#9146FF", clean, false);
    return true;
}
