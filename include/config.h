// config.h : petit ini cle=valeur (username / oauth / quality / volume)
#pragma once
#include <3ds.h>

typedef struct {
    char username[64];
    char oauth[128];
    char quality[24];
    int  volume;      // 0-100
} AppConfig;

extern AppConfig g_config;

void config_load(void);
void config_save(void);
