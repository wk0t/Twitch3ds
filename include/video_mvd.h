// video_mvd.h: H.264 hardware decoding via MVDSTD (New 3DS/2DS only).
// Pipeline : AU Annex-B -> NAL -> mvdstdProcessVideoFrame -> render RGB565
//            -> GX_DisplayTransfer -> C3D_Tex -> C2D_Image.
// Singleton: one video at a time.
#pragma once
#include <citro2d.h>

/* width/height: coded dims of the rendition (e.g. 284x160, 640x360) */
Result video_init(int width, int height);
void   video_exit(void);
bool   video_ready(void);

/* decodes one complete AU. Returns:
 *   1  = new image available in the texture
 *   0  = consumed without an image (SPS/PPS...)
 *  <0  = decode error (you can chain on the next AU) */
int video_decode_au(const u8 *au, size_t len);

/* image to draw with C2D_DrawImageAt; valid after the first return of 1 */
C2D_Image video_image(void);
/* useful dims of the image (the texture itself is pow2) */
void video_dimensions(int *w, int *h);
