// net_http.h : couche HTTPS libcurl+mbedtls.
// one HttpCtx per network thread (curl handle kept for TLS keep-alive),
// never shared between two threads at the same time.
#pragma once
#include <3ds.h>
#include <stddef.h>

typedef struct {
    u8    *data;   // always NUL-terminated (not counted in len)
    size_t len;
    size_t cap;
} HttpBuf;

typedef struct HttpCtx HttpCtx;

// socInit + curl_global_init, once at startup (main thread)
Result http_global_init(void);
void   http_global_exit(void);

HttpCtx *http_ctx_new(void);
void     http_ctx_free(HttpCtx *c);

// header stuck to every request of the ctx, e.g. "Client-ID: xxx"
void http_ctx_add_header(HttpCtx *c, const char *header_line);

/* returns the HTTP code (>=100), or negative on a libcurl error (-CURLcode).
   `out` is (re)allocated here, free it with http_buf_free. You can pass an
   already-used buffer, it is reset. */
int http_get(HttpCtx *c, const char *url, HttpBuf *out);
int http_post(HttpCtx *c, const char *url, const char *body, HttpBuf *out);
// same as http_post but x-www-form-urlencoded (for Twitch OAuth)
int http_post_form(HttpCtx *c, const char *url, const char *body, HttpBuf *out);

/* cancellation from ANOTHER thread: just sets a flag that the ongoing transfer
   sees via the progress callback, and it aborts. Safe to call while an
   http_get/post is running (we never touch the curl handle), this avoids
   waiting for the 10-15 s timeout to unblock the thread.
   The flag stays set -> http_ctx_uncancel() clears it before the next request. */
void http_ctx_cancel(HttpCtx *c);
void http_ctx_uncancel(HttpCtx *c);

// text of the last error on this ctx ("" if none), e.g. "HTTP 403"
const char *http_ctx_last_error(HttpCtx *c);

void http_buf_free(HttpBuf *b);
