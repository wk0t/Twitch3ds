// thumb.h : miniatures de streams (JPEG 320x180) chargées en tâche de fond.
#pragma once
#include <citro2d.h>

typedef struct {
    C3D_Tex   tex;
    C2D_Image img;
    bool ready;      /* img utilisable */
    bool failed;
    // interne
    int  state;
    char url[256];
    u16 *pixels;     /* buffer RGB565 rempli par le worker */
    int  pw, ph;
} Thumb;

void thumb_system_init(void);
void thumb_system_exit(void);

/* lance (ou relance si l'URL a changé) le chargement async */
void thumb_request(Thumb *t, const char *url);
/* à appeler chaque frame : transforme les downloads finis en textures */
void thumb_update(void);
/* libère texture + buffers (main thread) */
void thumb_release(Thumb *t);
