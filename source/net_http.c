// net_http.c : petite couche HTTPS (libcurl + mbedtls sur soc:u)
// le handle curl est gardé et réutilisé entre requêtes -> keep-alive TLS,
// parce qu'un handshake complet coûte ~1-2s sur la 3DS.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <curl/curl.h>
#include <3ds.h>

#include "net_http.h"

#define SOC_BUF_SIZE  0x100000
#define SOC_BUF_ALIGN 0x1000
#define HTTP_BUF_MIN  0x4000   /* 16 Ko */

struct HttpCtx {
    CURL              *easy;
    struct curl_slist *headers;
    volatile bool      cancel;    // posé par un autre thread pour couper
    char               err[128];
};

static u32 *s_soc_buf;
static bool s_curl_ready;

Result http_global_init(void)
{
    if (s_soc_buf)
        return 0;

    s_soc_buf = (u32 *)memalign(SOC_BUF_ALIGN, SOC_BUF_SIZE);
    if (!s_soc_buf)
        return MAKERESULT(RL_FATAL, RS_OUTOFRESOURCE, RM_SOC, RD_OUT_OF_MEMORY);

    Result rc = socInit(s_soc_buf, SOC_BUF_SIZE);
    if (R_FAILED(rc)) {
        free(s_soc_buf);
        s_soc_buf = NULL;
        return rc;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        socExit();
        free(s_soc_buf);
        s_soc_buf = NULL;
        return MAKERESULT(RL_FATAL, RS_INTERNAL, RM_SOC, RD_NOT_INITIALIZED);
    }

    s_curl_ready = true;
    return 0;
}

void http_global_exit(void)
{
    if (s_curl_ready) {
        curl_global_cleanup();
        s_curl_ready = false;
    }
    if (s_soc_buf) {
        socExit();
        free(s_soc_buf);
        s_soc_buf = NULL;
    }
}

HttpCtx *http_ctx_new(void)
{
    HttpCtx *c = (HttpCtx *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;

    c->easy = curl_easy_init();
    if (!c->easy) {
        free(c);
        return NULL;
    }
    return c;
}

void http_ctx_free(HttpCtx *c)
{
    if (!c)
        return;
    if (c->easy)
        curl_easy_cleanup(c->easy);
    curl_slist_free_all(c->headers);
    free(c);
}

void http_ctx_cancel(HttpCtx *c)
{
    if (c)
        c->cancel = true;
}

void http_ctx_uncancel(HttpCtx *c)
{
    if (c)
        c->cancel = false;
}

// curl rappelle ça pendant le transfert (et l'attente) : retour !=0 = on coupe
static int xfer_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                   curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    HttpCtx *c = (HttpCtx *)userdata;
    return c->cancel ? 1 : 0;   // -> CURLE_ABORTED_BY_CALLBACK
}

void http_ctx_add_header(HttpCtx *c, const char *header_line)
{
    if (!c || !header_line)
        return;
    struct curl_slist *nl = curl_slist_append(c->headers, header_line);
    if (nl)
        c->headers = nl;
}

// alloue si besoin puis vide le buffer, mais garde l'alloc existante
static int buf_reset(HttpBuf *b)
{
    if (!b->data) {
        b->data = (u8 *)malloc(HTTP_BUF_MIN);
        if (!b->data)
            return -1;
        b->cap = HTTP_BUF_MIN;
    }
    b->len = 0;
    b->data[0] = 0;
    return 0;
}

void http_buf_free(HttpBuf *b)
{
    if (!b)
        return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

// grossit le buffer x2 au besoin, garde toujours un NUL après les données
static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    HttpBuf *b = (HttpBuf *)userdata;
    size_t n = size * nmemb;
    if (n == 0)
        return 0;

    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap : HTTP_BUF_MIN;
        while (ncap < b->len + n + 1)
            ncap *= 2;
        u8 *nd = (u8 *)realloc(b->data, ncap);
        if (!nd)
            return 0;   // -> CURLE_WRITE_ERROR
        b->data = nd;
        b->cap = ncap;
    }

    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = 0;
    return n;
}

/* le handle est persistant, donc on réapplique tout à chaque fois :
   sinon il garde les réglages de la requête d'avant */
static void set_common_opts(HttpCtx *c, const char *url, HttpBuf *out)
{
    CURL *e = c->easy;
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(e, CURLOPT_USERAGENT, "twitch3ds/1.0");
    curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");   /* gzip via zlib */
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_BUFFERSIZE, 65536L);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, out);
    // progress activé -> on peut couper tout de suite sur http_ctx_cancel
    curl_easy_setopt(e, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(e, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(e, CURLOPT_XFERINFODATA, c);
}

static int do_perform(HttpCtx *c)
{
    c->err[0] = '\0';
    CURLcode rc = curl_easy_perform(c->easy);
    if (rc != CURLE_OK) {
        snprintf(c->err, sizeof c->err, "%s", curl_easy_strerror(rc));
        return -(int)rc;
    }

    long code = 0;
    curl_easy_getinfo(c->easy, CURLINFO_RESPONSE_CODE, &code);
    if (code != 200)
        snprintf(c->err, sizeof c->err, "HTTP %ld", code);
    return (int)code;
}

const char *http_ctx_last_error(HttpCtx *c)
{
    return (c && c->err[0]) ? c->err : "";
}

int http_get(HttpCtx *c, const char *url, HttpBuf *out)
{
    if (!c || !url || !out)
        return -(int)CURLE_BAD_FUNCTION_ARGUMENT;
    if (buf_reset(out) != 0)
        return -(int)CURLE_OUT_OF_MEMORY;

    set_common_opts(c, url, out);
    curl_easy_setopt(c->easy, CURLOPT_HTTPGET, 1L);   // repasse en GET si un POST traînait
    curl_easy_setopt(c->easy, CURLOPT_HTTPHEADER, c->headers);

    return do_perform(c);
}

static int do_post(HttpCtx *c, const char *url, const char *body,
                   HttpBuf *out, const char *content_type)
{
    if (!c || !url || !out)
        return -(int)CURLE_BAD_FUNCTION_ARGUMENT;
    if (buf_reset(out) != 0)
        return -(int)CURLE_OUT_OF_MEMORY;

    // copie temporaire des headers du ctx + le Content-Type, sans salir c->headers
    struct curl_slist *hdrs = NULL, *nl;
    for (const struct curl_slist *it = c->headers; it; it = it->next) {
        nl = curl_slist_append(hdrs, it->data);
        if (!nl) {
            curl_slist_free_all(hdrs);
            return -(int)CURLE_OUT_OF_MEMORY;
        }
        hdrs = nl;
    }
    nl = curl_slist_append(hdrs, content_type);
    if (!nl) {
        curl_slist_free_all(hdrs);
        return -(int)CURLE_OUT_OF_MEMORY;
    }
    hdrs = nl;

    set_common_opts(c, url, out);
    curl_easy_setopt(c->easy, CURLOPT_POST, 1L);
    curl_easy_setopt(c->easy, CURLOPT_POSTFIELDS, body ? body : "");
    curl_easy_setopt(c->easy, CURLOPT_HTTPHEADER, hdrs);

    int r = do_perform(c);

    // le handle persiste : faut pas y laisser de pointeurs vers la liste temp
    // ou le body, qu'on va libérer juste après
    curl_easy_setopt(c->easy, CURLOPT_HTTPHEADER, c->headers);
    curl_easy_setopt(c->easy, CURLOPT_POSTFIELDS, NULL);
    curl_easy_setopt(c->easy, CURLOPT_HTTPGET, 1L);
    curl_slist_free_all(hdrs);

    return r;
}

int http_post(HttpCtx *c, const char *url, const char *body, HttpBuf *out)
{
    return do_post(c, url, body, out, "Content-Type: application/json");
}

int http_post_form(HttpCtx *c, const char *url, const char *body, HttpBuf *out)
{
    return do_post(c, url, body, out,
                   "Content-Type: application/x-www-form-urlencoded");
}
