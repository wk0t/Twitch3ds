// thumb.h: stream thumbnails (JPEG 320x180) loaded in the background.
#pragma once
#include <citro2d.h>

typedef struct {
    C3D_Tex   tex;
    C2D_Image img;
    bool ready;      /* img usable */
    bool failed;
    // internal
    int  state;
    char url[256];
    u16 *pixels;     /* RGB565 buffer filled by the worker */
    int  pw, ph;
} Thumb;

void thumb_system_init(void);
void thumb_system_exit(void);

/* starts (or restarts if the URL changed) the async load */
void thumb_request(Thumb *t, const char *url);
/* call every frame: turns finished downloads into textures */
void thumb_update(void);
/* frees texture + buffers (main thread) */
void thumb_release(Thumb *t);
