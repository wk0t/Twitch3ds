// update.h : vérif d'une nouvelle version sur les releases GitHub
#pragma once
#include <stdbool.h>

// lance la vérif en tâche de fond (une seule fois)
void update_start_check(void);

bool update_available(void);      // une version plus récente existe
const char *update_latest(void);  // tag de la dernière release, ex "v1.1.0"

// télécharge le .3dsx de la dernière release et remplace le fichier sur la SD.
// bloquant (quelques secondes). 0 si ok, négatif sinon.
int update_download(void);
