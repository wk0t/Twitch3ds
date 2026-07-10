// net_http.h : couche HTTPS libcurl+mbedtls.
// un HttpCtx par thread réseau (handle curl gardé pour le keep-alive TLS),
// jamais partagé entre deux threads en même temps.
#pragma once
#include <3ds.h>
#include <stddef.h>

typedef struct {
    u8    *data;   // toujours terminé par un NUL (pas compté dans len)
    size_t len;
    size_t cap;
} HttpBuf;

typedef struct HttpCtx HttpCtx;

// socInit + curl_global_init, une seule fois au démarrage (thread principal)
Result http_global_init(void);
void   http_global_exit(void);

HttpCtx *http_ctx_new(void);
void     http_ctx_free(HttpCtx *c);

// en-tête collé à toutes les requêtes du ctx, ex "Client-ID: xxx"
void http_ctx_add_header(HttpCtx *c, const char *header_line);

/* retour = code HTTP (>=100), ou négatif pour une erreur libcurl (-CURLcode).
   `out` est (ré)alloué ici, à libérer avec http_buf_free. On peut repasser un
   buffer déjà utilisé, il est réinitialisé. */
int http_get(HttpCtx *c, const char *url, HttpBuf *out);
int http_post(HttpCtx *c, const char *url, const char *body, HttpBuf *out);
// pareil que http_post mais en x-www-form-urlencoded (pour l'OAuth Twitch)
int http_post_form(HttpCtx *c, const char *url, const char *body, HttpBuf *out);

/* annulation depuis un AUTRE thread : pose juste un booléen que le transfert
   en cours voit via le callback de progression, et il abandonne. Sûr à appeler
   pendant qu'un http_get/post tourne (on ne touche jamais le handle curl), ça
   évite d'attendre le timeout de 10-15s pour débloquer le thread.
   Le flag reste posé -> http_ctx_uncancel() le lève avant la requête suivante. */
void http_ctx_cancel(HttpCtx *c);
void http_ctx_uncancel(HttpCtx *c);

// texte de la dernière erreur sur ce ctx ("" si rien), genre "HTTP 403"
const char *http_ctx_last_error(HttpCtx *c);

void http_buf_free(HttpBuf *b);
