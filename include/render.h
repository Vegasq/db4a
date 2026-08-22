#ifndef RENDER_H
#define RENDER_H
#include <stdint.h>
#include <stdio.h>
#define FB_W 320
#define FB_H 224
extern uint8_t FB[FB_H][FB_W][3];
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
