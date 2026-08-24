#ifndef RENDER_H
#define RENDER_H
#include <stdint.h>
#include <stdio.h>
/* FB_W is the ALLOCATION width. The live width is fb_width, which is 320
   unless widescreen is enabled -- see docs/widescreen.md. Keeping the buffer
   at its maximum means one binary serves both and the flag is a runtime
   switch rather than a build variant. */
#define FB_W 512
/* FB_H is the ALLOCATION height, as FB_W is for width. The live height is
   fb_height, which is 224 -- the lines a Mega Drive shows in its usual V28
   mode -- unless something asks for more. Keeping the buffer at its maximum
   means one binary serves every size and the choice is a runtime one. */
#define FB_H 256
extern uint8_t FB[FB_H][FB_W][3];
extern int fb_width;      /* live width,  <= FB_W */
extern int fb_height;     /* live height, <= FB_H */
int render_world_offset(void);
int render_widescreen_gameplay(void);
void render_wide_stats(unsigned long *guard, unsigned long *ext, int *hsa, int *hsb);
/* Scroll state is latched per scanline while the frame executes, because the
   game changes vertical scroll mid-frame and a real VDP uses the value in
   effect at each line. Call render_line_latch(line) at the START of each
   scanline; render_frame() then draws from the latched values, not live
   registers. render_frame_begin() resets the latch for a new frame. */
void render_frame_begin(void);
void render_line_latch(unsigned line);
void render_frame(void);
void render_sprites(void);
int  render_write_ppm(const char *path);
#endif
