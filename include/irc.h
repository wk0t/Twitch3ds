// irc.h: the Twitch chat (plaintext IRC). runs in its own thread, irc_poll drains the queue.
// Singleton.
#pragma once
#include <3ds.h>
#include <stdbool.h>

#define IRC_NICK_MAX 40
#define IRC_TEXT_MAX 400

typedef struct {
    char nick[IRC_NICK_MAX];  /* display-name, or the login if no tag */
    char color[8];            /* "#RRGGBB" ou "" */
    char text[IRC_TEXT_MAX];  /* UTF-8 */
    bool is_system;           /* our own status messages (connecting...) */
} ChatMsg;

// starts the thread and JOINs #channel.
// empty/NULL username/oauth => anonymous read-only.
// oauth expected as "oauth:xxxx"; the prefix is added if missing.
Result irc_start(const char *channel, const char *username, const char *oauth);
void   irc_stop(void);

bool irc_connected(void);
bool irc_can_send(void);   /* true only when logged in with an account */

/* drains up to max messages, returns how many were read */
int irc_poll(ChatMsg *out, int max);

/* sends to the channel. false if anonymous or disconnected. */
bool irc_send(const char *text);
