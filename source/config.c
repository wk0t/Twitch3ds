// config.c : lecture/sauvegarde de la config dans sdmc:/3ds/twitch3ds/config.ini
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include "config.h"

#define CFG_DIR1 "sdmc:/3ds"
#define CFG_DIR2 "sdmc:/3ds/twitch3ds"
#define CFG_PATH CFG_DIR2 "/config.ini"

AppConfig g_config;

static void set_defaults(void)
{
    memset(&g_config, 0, sizeof(g_config));
    strcpy(g_config.quality, "160p");
    g_config.volume = 100;
}

// vire espaces et \r\n aux deux bouts, modifie s en place
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    *end = 0;
    return s;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

void config_load(void)
{
    set_defaults();

    FILE *f = fopen(CFG_PATH, "r");
    if (!f)
        return;   // pas de fichier = premier lancement, on garde les defauts

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (*p == 0 || *p == '#')
            continue;

        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *key = trim(p);
        const char *val = trim(eq + 1);

        if (strcmp(key, "username") == 0) {
            copy_str(g_config.username, sizeof(g_config.username), val);
        } else if (strcmp(key, "oauth") == 0) {
            copy_str(g_config.oauth, sizeof(g_config.oauth), val);
        } else if (strcmp(key, "quality") == 0) {
            copy_str(g_config.quality, sizeof(g_config.quality), val);
        } else if (strcmp(key, "volume") == 0) {
            int v = atoi(val);
            if (v < 0)   v = 0;
            if (v > 100) v = 100;
            g_config.volume = v;
        }
        // clé inconnue : on ignore
    }
    fclose(f);

    if (g_config.quality[0] == 0)
        strcpy(g_config.quality, "160p");
}

void config_save(void)
{
    /* si les dossiers existent deja mkdir renvoie EEXIST, osef */
    mkdir(CFG_DIR1, 0777);
    mkdir(CFG_DIR2, 0777);

    FILE *f = fopen(CFG_PATH, "w");
    if (!f)
        return;   // SD en lecture seule ? tant pis, on laisse tomber

    fprintf(f, "username=%s\n", g_config.username);
    fprintf(f, "oauth=%s\n",    g_config.oauth);
    fprintf(f, "quality=%s\n",  g_config.quality);
    fprintf(f, "volume=%d\n",   g_config.volume);
    fclose(f);
}
