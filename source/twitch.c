// twitch.c : API Twitch non officielle (GQL + usher). singleton à état statique.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "twitch.h"
#include "cJSON.h"

#define TW_PAT_SHA256 "0828119ded1c13477966434e15800ff57ddacf13ba1911c129dc2200705b0712"
#define TW_USHER_FMT \
    "https://usher.ttvnw.net/api/channel/hls/%s.m3u8?client_id=%s&token=%s&sig=%s" \
    "&allow_source=true&allow_audio_only=true&player=twitchweb&type=any"

/* scratch statique : évite ~2 Ko de pile par TwitchQuality (les threads 3DS
   ont une petite pile). du coup pas réentrant, mais on a un seul lecteur. */
static TwitchQuality s_qtmp;

/* ---------------------------------------------------------------- headers */

#define TW_MAX_CTX 8
static HttpCtx *s_seen_ctx[TW_MAX_CTX];

/* pose "Client-ID" une seule fois par ctx. les ctx vivent toute l'appli,
   donc pas de souci de réutilisation d'adresse après http_ctx_free. */
static void tw_headers(HttpCtx *c)
{
    for (int i = 0; i < TW_MAX_CTX; i++) {
        HttpCtx *cur = __atomic_load_n(&s_seen_ctx[i], __ATOMIC_ACQUIRE);
        if (cur == c)
            return;
        if (cur == NULL) {
            HttpCtx *expected = NULL;
            if (__atomic_compare_exchange_n(&s_seen_ctx[i], &expected, c,
                                            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                http_ctx_add_header(c, "Client-ID: " TWITCH_CLIENT_ID);
                return;
            }
            if (expected == c)
                return;
            /* slot pris par un autre ctx entre-temps : on continue */
        }
    }
    /* Table pleine (improbable) : header dupliqué sans gravité. */
    http_ctx_add_header(c, "Client-ID: " TWITCH_CLIENT_ID);
}

/* ------------------------------------------------------------------ utils */

/* Copie bornée qui ne coupe jamais une séquence UTF-8 multi-octets. */
static void utf8_copy(char *dst, size_t dst_size, const char *src)
{
    if (!src || dst_size == 0) {
        if (dst_size) dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_size) {
        n = dst_size - 1;
        /* recule tant qu'on est sur un octet de continuation */
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Extrait obj[key] (chaîne) vers dst, "" si absent/null. */
static void json_copy_str(const cJSON *obj, const char *key, char *dst, size_t dst_size)
{
    dst[0] = '\0';
    if (!obj)
        return;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring)
        utf8_copy(dst, dst_size, it->valuestring);
}

/* percent-encoding (unreserved = alnum + - _ . ~).
   retour : longueur écrite, -1 si dst trop petit. */
static int url_encode(char *dst, size_t dst_size, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *src; src++) {
        unsigned char ch = (unsigned char)*src;
        bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                          (ch >= '0' && ch <= '9') ||
                          ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (unreserved) {
            if (o + 1 >= dst_size) return -1;
            dst[o++] = (char)ch;
        } else {
            if (o + 3 >= dst_size) return -1;
            dst[o++] = '%';
            dst[o++] = hex[ch >> 4];
            dst[o++] = hex[ch & 0x0F];
        }
    }
    dst[o] = '\0';
    return (int)o;
}

/* ---------------------------------------------------------------- GQL POST */

/* POST body sur l'endpoint GQL, retourne la racine cJSON ou NULL.
   *code_out (optionnel) : négatif = transport / -1000 parse, sinon code HTTP. */
/* dernier message d'erreur Twitch (champ GQL "errors"), pour l'UI. */
static char tw_err[192];

const char *twitch_last_error(void)
{
    return tw_err;
}

/* Si la réponse GQL contient un tableau "errors", copie le 1er message. */
static void capture_gql_error(cJSON *root)
{
    cJSON *errs = cJSON_GetObjectItemCaseSensitive(root, "errors");
    if (!cJSON_IsArray(errs) || cJSON_GetArraySize(errs) == 0)
        return;
    cJSON *e0  = cJSON_GetArrayItem(errs, 0);
    cJSON *msg = e0 ? cJSON_GetObjectItemCaseSensitive(e0, "message") : NULL;
    if (cJSON_IsString(msg) && msg->valuestring)
        snprintf(tw_err, sizeof tw_err, "%s", msg->valuestring);
}

static cJSON *gql_post(HttpCtx *c, const char *body, int *code_out)
{
    tw_err[0] = '\0';
    tw_headers(c);
    HttpBuf buf = {0};
    int code = http_post(c, TWITCH_GQL_URL, body, &buf);

    cJSON *root = NULL;
    if (code == 200 && buf.data) {
        root = cJSON_ParseWithLength((const char *)buf.data, buf.len);
        if (!root && code_out)
            code = -1000;
        if (root)
            capture_gql_error(root);
    }
    http_buf_free(&buf);
    if (code_out)
        *code_out = code;
    return root;
}

/* Traduit l'échec d'un gql_post en code d'erreur négatif exploitable par l'UI. */
static int gql_err(int code)
{
    if (code < 0) return code;    /* -(CURLcode) transport, ou -1000 parse */
    if (code == 0) return -9003;  /* curl OK mais aucun code HTTP (anormal) */
    return -code;                 /* code HTTP non-200 (ex 403 -> -403) */
}

/* Enveloppe {"query": doc} sérialisée ; à libérer avec cJSON_free. */
static char *body_from_doc(const char *doc)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    if (!cJSON_AddStringToObject(o, "query", doc)) {
        cJSON_Delete(o);
        return NULL;
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

/* data.<field>.edges[].node -> out[]. build_preview : reconstruit l'URL de
   miniature depuis le login (searchStreams ne renvoie pas previewImageURL). */
static int parse_stream_list(cJSON *root, const char *field,
                             TwitchStream *out, int max, bool build_preview)
{
    cJSON *data  = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *coll  = data ? cJSON_GetObjectItemCaseSensitive(data, field) : NULL;
    cJSON *edges = coll ? cJSON_GetObjectItemCaseSensitive(coll, "edges") : NULL;
    if (!cJSON_IsArray(edges))
        return -1;

    int n = 0;
    cJSON *edge;
    cJSON_ArrayForEach(edge, edges) {
        if (n >= max)
            break;
        cJSON *node = cJSON_GetObjectItemCaseSensitive(edge, "node");
        if (!cJSON_IsObject(node))
            continue;

        TwitchStream *s = &out[n];
        memset(s, 0, sizeof *s);

        cJSON *bc = cJSON_GetObjectItemCaseSensitive(node, "broadcaster");
        json_copy_str(bc, "login", s->login, sizeof s->login);
        json_copy_str(bc, "displayName", s->display, sizeof s->display);
        if (!s->login[0])
            continue; /* node inexploitable sans login */
        if (!s->display[0])
            utf8_copy(s->display, sizeof s->display, s->login);

        json_copy_str(node, "title", s->title, sizeof s->title);
        /* game peut être null -> "" */
        json_copy_str(cJSON_GetObjectItemCaseSensitive(node, "game"),
                      "displayName", s->game, sizeof s->game);

        cJSON *v = cJSON_GetObjectItemCaseSensitive(node, "viewersCount");
        s->viewers = cJSON_IsNumber(v) ? v->valueint : 0;

        if (!build_preview)
            json_copy_str(node, "previewImageURL", s->preview_url, sizeof s->preview_url);
        if (!s->preview_url[0])
            snprintf(s->preview_url, sizeof s->preview_url,
                     "https://static-cdn.jtvnw.net/previews-ttv/live_user_%s-320x180.jpg",
                     s->login);
        n++;
    }
    return n;
}

/* ------------------------------------------------------------- API listes */

int twitch_top_streams(HttpCtx *c, TwitchStream *out, int max)
{
    if (!c || !out || max <= 0)
        return -9001;
    int first = max > 30 ? 30 : max;   /* Twitch plafonne streams(first:) à 30 */

    char doc[384];
    snprintf(doc, sizeof doc,
             "query { streams(first: %d) { edges { node { "
             "title viewersCount broadcaster { login displayName } "
             "game { displayName } previewImageURL(width: 320, height: 180) "
             "} } } }", first);

    char *body = body_from_doc(doc);
    if (!body)
        return -9002;
    int code = 0;
    cJSON *root = gql_post(c, body, &code);
    cJSON_free(body);
    if (!root)
        return gql_err(code);

    int n = parse_stream_list(root, "streams", out, max, false);
    cJSON_Delete(root);
    return n;
}

int twitch_search_streams(HttpCtx *c, const char *query, TwitchStream *out, int max)
{
    if (!c || !query || !query[0] || !out || max <= 0)
        return -1;
    int first = max > 30 ? 30 : max;   /* même plafond côté recherche */

    /* échappement de la query : cJSON produit une chaîne quotée valide en
       GraphQL (mêmes règles que JSON). */
    cJSON *qs = cJSON_CreateString(query);
    if (!qs)
        return -1;
    char *quoted = cJSON_PrintUnformatted(qs);
    cJSON_Delete(qs);
    if (!quoted)
        return -1;

    char doc[2048];
    int w = snprintf(doc, sizeof doc,
                     "query { searchStreams(userQuery: %s, first: %d) { edges { node { "
                     "title viewersCount broadcaster { login displayName } "
                     "game { displayName } } } } }", quoted, first);
    cJSON_free(quoted);
    if (w < 0 || w >= (int)sizeof doc)
        return -1;

    char *body = body_from_doc(doc);
    if (!body)
        return -1;
    int code = 0;
    cJSON *root = gql_post(c, body, &code);
    cJSON_free(body);
    if (!root)
        return gql_err(code);

    int n = parse_stream_list(root, "searchStreams", out, max, true);
    cJSON_Delete(root);
    return n;
}

/* --------------------------------------------------------- master playlist */

/* Parse les attributs après "#EXT-X-STREAM-INF:". Les valeurs quotées
 * peuvent contenir des virgules (CODECS="avc1...,mp4a...") : on respecte
 * les guillemets. */
static void parse_stream_inf(const char *p, TwitchQuality *q)
{
    while (*p) {
        const char *ks = p;
        while (*p && *p != '=' && *p != ',')
            p++;
        if (*p != '=') {          /* attribut sans valeur : on saute */
            if (*p) p++;
            continue;
        }
        size_t klen = (size_t)(p - ks);
        p++;

        char val[256];
        size_t vl = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (vl + 1 < sizeof val) val[vl++] = *p;
                p++;
            }
            if (*p == '"') p++;
        } else {
            while (*p && *p != ',') {
                if (vl + 1 < sizeof val) val[vl++] = *p;
                p++;
            }
        }
        val[vl] = '\0';
        if (*p == ',') p++;

        if (klen == 9 && !strncmp(ks, "BANDWIDTH", 9)) {
            q->bandwidth = atoi(val);
        } else if (klen == 10 && !strncmp(ks, "RESOLUTION", 10)) {
            sscanf(val, "%dx%d", &q->width, &q->height);
        } else if (klen == 5 && !strncmp(ks, "VIDEO", 5)) {
            if (!strcmp(val, "chunked")) {
                utf8_copy(q->name, sizeof q->name, "source");
            } else {
                utf8_copy(q->name, sizeof q->name, val);
                if (!strcmp(val, "audio_only"))
                    q->audio_only = true;
            }
        }
    }
}

/* Parse in-place (text modifié) de la master playlist. */
static int parse_master(char *text, TwitchQuality *out, int max)
{
    int count = 0;
    bool pending = false;

    char *line = text;
    while (line && count < max) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r')
            line[--len] = '\0';

        if (len == 0) {
            /* ligne vide : ignorée */
        } else if (line[0] == '#') {
            if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
                memset(&s_qtmp, 0, sizeof s_qtmp);
                parse_stream_inf(line + 18, &s_qtmp);
                pending = true;
            }
            /* les autres tags entre STREAM-INF et l'URI n'annulent pas l'attente */
        } else {
            if (pending && s_qtmp.name[0] && s_qtmp.bandwidth > 0) {
                snprintf(s_qtmp.url, sizeof s_qtmp.url, "%s", line);
                out[count++] = s_qtmp;
            }
            pending = false;
        }
        line = nl ? nl + 1 : NULL;
    }
    return count;
}

/* Login Twitch : lettres, chiffres, underscore uniquement (anti-injection URL). */
static bool login_valid(const char *login)
{
    size_t n = strlen(login);
    if (n == 0 || n >= 64)
        return false;
    for (size_t i = 0; i < n; i++) {
        char ch = login[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_';
        if (!ok)
            return false;
    }
    return true;
}

int twitch_get_qualities(HttpCtx *c, const char *login, TwitchQuality *out, int max)
{
    if (!c || !login || !out || max <= 0 || !login_valid(login))
        return -1;

    /* 1. Persisted query PlaybackAccessToken */
    cJSON *body = cJSON_CreateObject();
    if (!body)
        return -1;
    cJSON_AddStringToObject(body, "operationName", "PlaybackAccessToken");
    cJSON *ext = cJSON_CreateObject();
    cJSON *pq  = cJSON_CreateObject();
    if (!ext || !pq) {
        cJSON_Delete(body);
        cJSON_Delete(ext);
        cJSON_Delete(pq);
        return -1;
    }
    cJSON_AddNumberToObject(pq, "version", 1);
    cJSON_AddStringToObject(pq, "sha256Hash", TW_PAT_SHA256);
    cJSON_AddItemToObject(ext, "persistedQuery", pq);
    cJSON_AddItemToObject(body, "extensions", ext);
    cJSON *vars = cJSON_CreateObject();
    if (!vars) {
        cJSON_Delete(body);
        return -1;
    }
    cJSON_AddBoolToObject(vars, "isLive", 1);
    cJSON_AddStringToObject(vars, "login", login);
    cJSON_AddBoolToObject(vars, "isVod", 0);
    cJSON_AddStringToObject(vars, "vodID", "");
    cJSON_AddStringToObject(vars, "playerType", "embed");
    cJSON_AddItemToObject(body, "variables", vars);

    char *body_s = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_s)
        return -1;

    int pat_code = 0;
    cJSON *root = gql_post(c, body_s, &pat_code);
    cJSON_free(body_s);
    if (!root)
        return gql_err(pat_code);

    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *tok  = data ? cJSON_GetObjectItemCaseSensitive(data, "streamPlaybackAccessToken") : NULL;
    if (!cJSON_IsObject(tok)) {
        /* token null -> chaîne hors-ligne */
        cJSON_Delete(root);
        return 0;
    }
    cJSON *jval = cJSON_GetObjectItemCaseSensitive(tok, "value");
    cJSON *jsig = cJSON_GetObjectItemCaseSensitive(tok, "signature");
    if (!cJSON_IsString(jval) || !jval->valuestring ||
        !cJSON_IsString(jsig) || !jsig->valuestring) {
        cJSON_Delete(root);
        return 0;
    }

    /* value est un JSON stringifié : URL-encoding intégral. */
    size_t enc_cap = strlen(jval->valuestring) * 3 + 1;
    char *enc = malloc(enc_cap);
    if (!enc) {
        cJSON_Delete(root);
        return -1;
    }
    if (url_encode(enc, enc_cap, jval->valuestring) < 0) {
        free(enc);
        cJSON_Delete(root);
        return -1;
    }
    char sig[128];
    snprintf(sig, sizeof sig, "%s", jsig->valuestring);
    cJSON_Delete(root);

    /* 2. GET usher */
    size_t url_cap = strlen(enc) + strlen(login) + strlen(sig) + sizeof TW_USHER_FMT + 64;
    char *url = malloc(url_cap);
    if (!url) {
        free(enc);
        return -1;
    }
    snprintf(url, url_cap, TW_USHER_FMT, login, TWITCH_CLIENT_ID, enc, sig);
    free(enc);

    tw_headers(c);
    HttpBuf buf = {0};
    int code = http_get(c, url, &buf);
    free(url);

    if (code == 404) { /* hors-ligne */
        http_buf_free(&buf);
        return 0;
    }
    if (code != 200 || !buf.data) {
        http_buf_free(&buf);
        return code < 0 ? code : -1;
    }

    /* 3. Parse de la master playlist */
    int count = parse_master((char *)buf.data, out, max);
    http_buf_free(&buf);

    /* 4. tri par bande passante croissante (tri sélection) */
    for (int i = 0; i + 1 < count; i++) {
        int m = i;
        for (int j = i + 1; j < count; j++)
            if (out[j].bandwidth < out[m].bandwidth)
                m = j;
        if (m != i) {
            s_qtmp = out[i];
            out[i] = out[m];
            out[m] = s_qtmp;
        }
    }
    return count;
}

/* ============================ OAuth Device Flow ============================ */

#define TW_DEVICE_URL   "https://id.twitch.tv/oauth2/device"
#define TW_TOKEN_URL    "https://id.twitch.tv/oauth2/token"
#define TW_VALIDATE_URL "https://id.twitch.tv/oauth2/validate"
#define TW_SCOPES       "chat:read chat:edit"

/* Copie bornée d'un champ chaîne JSON. Retour : true si présent. */
static bool json_str(cJSON *o, const char *key, char *dst, size_t sz)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsString(v) || !v->valuestring) {
        if (sz) dst[0] = '\0';
        return false;
    }
    snprintf(dst, sz, "%s", v->valuestring);
    return true;
}

int twitch_device_start(HttpCtx *c, TwitchDeviceAuth *out)
{
    if (!c || !out)
        return -1;
    memset(out, 0, sizeof *out);
    out->interval = 5;

    const char *body = "client_id=" TWITCH_CLIENT_ID "&scopes=" TW_SCOPES;
    HttpBuf buf = {0};
    int code = http_post_form(c, TW_DEVICE_URL, body, &buf);
    if (code != 200 || !buf.data) {
        http_buf_free(&buf);
        return code < 0 ? code : -1;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)buf.data, buf.len);
    http_buf_free(&buf);
    if (!root)
        return -1;

    bool ok = json_str(root, "device_code", out->device_code, sizeof out->device_code)
            & json_str(root, "user_code", out->user_code, sizeof out->user_code)
            & json_str(root, "verification_uri", out->verify_uri, sizeof out->verify_uri);
    cJSON *iv = cJSON_GetObjectItemCaseSensitive(root, "interval");
    if (cJSON_IsNumber(iv) && iv->valueint > 0)
        out->interval = iv->valueint;
    cJSON *ex = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    if (cJSON_IsNumber(ex))
        out->expires_in = ex->valueint;
    cJSON_Delete(root);

    return ok ? 0 : -1;
}

int twitch_device_poll(HttpCtx *c, const char *device_code,
                       char *token, size_t token_size,
                       char *refresh, size_t refresh_size)
{
    if (!c || !device_code || !token || token_size == 0)
        return -1;
    token[0] = '\0';
    if (refresh && refresh_size) refresh[0] = '\0';

    char body[256];
    snprintf(body, sizeof body,
             "client_id=" TWITCH_CLIENT_ID "&scopes=" TW_SCOPES
             "&device_code=%s&grant_type=urn:ietf:params:oauth:grant-type:device_code",
             device_code);

    HttpBuf buf = {0};
    int code = http_post_form(c, TW_TOKEN_URL, body, &buf);
    if ((code < 200 && code != 400) || !buf.data) {
        http_buf_free(&buf);
        return -1;   /* erreur transport */
    }

    cJSON *root = cJSON_ParseWithLength((const char *)buf.data, buf.len);
    http_buf_free(&buf);
    if (!root)
        return -1;

    int ret;
    if (code == 200 && json_str(root, "access_token", token, token_size)) {
        if (refresh)
            json_str(root, "refresh_token", refresh, refresh_size);
        ret = 1;                                  /* autorisé */
    } else {
        char msg[64] = {0};
        json_str(root, "message", msg, sizeof msg);
        ret = strstr(msg, "authorization_pending") ? 0 : -1;
    }
    cJSON_Delete(root);
    return ret;
}

int twitch_get_login(HttpCtx *c, const char *token, char *login, size_t size)
{
    (void)c; /* contexte dédié : en-têtes Authorization isolés */
    if (!token || !login || size == 0)
        return -1;
    login[0] = '\0';

    /* /validate marche avec n'importe quel jeton (contrairement à Helix qui
       veut un client-id enregistré). en-tête "Authorization: OAuth <token>". */
    HttpCtx *ac = http_ctx_new();
    if (!ac)
        return -1;
    char auth[512];
    snprintf(auth, sizeof auth, "Authorization: OAuth %s", token);
    http_ctx_add_header(ac, auth);

    HttpBuf buf = {0};
    int code = http_get(ac, TW_VALIDATE_URL, &buf);
    http_ctx_free(ac);
    if (code != 200 || !buf.data) {
        http_buf_free(&buf);
        return -1;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)buf.data, buf.len);
    http_buf_free(&buf);
    if (!root)
        return -1;

    bool ok = json_str(root, "login", login, size);
    cJSON_Delete(root);
    return ok ? 0 : -1;
}
