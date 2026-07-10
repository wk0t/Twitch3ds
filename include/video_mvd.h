// video_mvd.h : décodage H.264 hardware via MVDSTD (New 3DS/2DS only).
// Pipeline : AU Annex-B -> NAL -> mvdstdProcessVideoFrame -> render RGB565
//            -> GX_DisplayTransfer -> C3D_Tex -> C2D_Image.
// Singleton : une seule vidéo à la fois.
#pragma once
#include <citro2d.h>

/* width/height : dims codées de la rendition (ex 284x160, 640x360) */
Result video_init(int width, int height);
void   video_exit(void);
bool   video_ready(void);

/* décode une AU complète. Retour :
 *   1  = nouvelle image dispo dans la texture
 *   0  = consommé sans image (SPS/PPS...)
 *  <0  = erreur de décodage (on peut enchaîner sur l'AU suivante) */
int video_decode_au(const u8 *au, size_t len);

/* image à dessiner avec C2D_DrawImageAt ; valide après le premier retour 1 */
C2D_Image video_image(void);
/* dims utiles de l'image (la texture, elle, est en pow2) */
void video_dimensions(int *w, int *h);
