// update.c : regarde la dernière release GitHub et dit s'il y a mieux à jour.
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "update.h"
#include "version.h"
#include "net_http.h"
#include "cJSON.h"

#define GH_LATEST "https://api.github.com/repos/" GH_OWNER "/" GH_REPO "/releases/latest"

static volatile bool s_started;
static volatile bool s_available;
static char s_latest[32];
static char s_asset[512];   // url du .3dsx de la release

// "v1.2.3" -> 1002003, pour comparer deux versions
static long ver_num(const char *v)
{
    while (*v == 'v' || *v == 'V') v++;
    int a = 0, b = 0, c = 0;
    sscanf(v, "%d.%d.%d", &a, &b, &c);
    return a * 1000000L + b * 1000L + c;
}

static void check_thread(void *arg)
{
    (void)arg;
    HttpCtx *c = http_ctx_new();
    if (!c)
        return;

    HttpBuf buf = {0};
    int code = http_get(c, GH_LATEST, &buf);
    if (code == 200 && buf.data) {
        cJSON *root = cJSON_ParseWithLength((const char *)buf.data, buf.len);
        if (root) {
            cJSON *tag = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
            if (cJSON_IsString(tag) && tag->valuestring) {
                snprintf(s_latest, sizeof s_latest, "%s", tag->valuestring);
                if (ver_num(tag->valuestring) > ver_num(APP_VERSION))
                    s_available = true;
            }
            // on récupère l'url du .3dsx attaché à la release
            cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
            cJSON *a;
            cJSON_ArrayForEach(a, assets) {
                cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
                cJSON *url  = cJSON_GetObjectItemCaseSensitive(a, "browser_download_url");
                if (cJSON_IsString(name) && name->valuestring &&
                    cJSON_IsString(url) && url->valuestring &&
                    strstr(name->valuestring, ".3dsx")) {
                    snprintf(s_asset, sizeof s_asset, "%s", url->valuestring);
                    break;
                }
            }
            cJSON_Delete(root);
        }
    }
    http_buf_free(&buf);
    http_ctx_free(c);
}

void update_start_check(void)
{
    if (s_started)
        return;
    s_started = true;
    // détaché : on ne le rejoint jamais, il se libère tout seul
    threadCreate(check_thread, NULL, 32 * 1024, 0x30, -2, true);
}

bool update_available(void)      { return s_available; }
const char *update_latest(void)  { return s_latest; }

int update_download(void)
{
    if (!s_asset[0])
        return -1;

    HttpCtx *c = http_ctx_new();
    if (!c)
        return -1;
    HttpBuf buf = {0};
    int code = http_get(c, s_asset, &buf);
    http_ctx_free(c);

    int rc = -1;
    if (code == 200 && buf.data && buf.len > 1000) {
        // on écrit à côté puis on renomme, pour ne pas casser le .3dsx si ça foire
        FILE *f = fopen("sdmc:/3ds/twitch3ds.new", "wb");
        if (f) {
            size_t w = fwrite(buf.data, 1, buf.len, f);
            fclose(f);
            if (w == buf.len) {
                remove("sdmc:/3ds/twitch3ds.3dsx");
                rc = (rename("sdmc:/3ds/twitch3ds.new",
                             "sdmc:/3ds/twitch3ds.3dsx") == 0) ? 0 : -1;
            } else {
                remove("sdmc:/3ds/twitch3ds.new");
            }
        }
    }
    http_buf_free(&buf);
    return rc;
}
