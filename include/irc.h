// irc.h : le chat Twitch (IRC en clair). tourne dans son thread, irc_poll vide la file.
// singleton.
#pragma once
#include <3ds.h>
#include <stdbool.h>

#define IRC_NICK_MAX 40
#define IRC_TEXT_MAX 400

typedef struct {
    char nick[IRC_NICK_MAX];  /* display-name, ou le login si pas de tag */
    char color[8];            /* "#RRGGBB" ou "" */
    char text[IRC_TEXT_MAX];  /* UTF-8 */
    bool is_system;           /* nos propres messages d'état (connexion...) */
} ChatMsg;

// démarre le thread et JOIN #channel.
// username/oauth vides ou NULL => lecture seule anonyme.
// oauth attendu en "oauth:xxxx", le préfixe est rajouté si besoin.
Result irc_start(const char *channel, const char *username, const char *oauth);
void   irc_stop(void);

bool irc_connected(void);
bool irc_can_send(void);   /* vrai seulement si loggué avec un compte */

/* vide jusqu'à max messages, rend le nombre récupéré */
int irc_poll(ChatMsg *out, int max);

/* envoie dans le canal. false si anonyme ou déconnecté. */
bool irc_send(const char *text);
