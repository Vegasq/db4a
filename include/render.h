#ifndef RENDER_H
#define RENDER_H
#include <stdint.h>
#include <stdio.h>
/* FB_W and FB_H are the ALLOCATION size. The live size is fb_width x
   fb_height, which is 320x224 unless widescreen is enabled -- see
   docs/widescreen.md. Keeping the buffer at its maximum means one binary
   serves every size and the choice is a runtime one.
   .
   1024x1024 IS THE GAME'S OWN MAP, and that is why the cap is there rather
   than anywhere else. Measured from the camera limits the cartridge itself
   writes ($FFE3D2/$FFE3D4 and $FFE3CE/$FFE3D0), mission 1 and mission 2 both
   give X 512..1216 and Y 512..1312, which with the 320x224 the camera frames
   is a world exactly 1024 pixels square. A view that size shows the whole
   map at once and pins the camera; a view LARGER than that can only add
   backdrop, because there is no more map to look at. Raising these numbers
   again would cost memory and buy nothing.
   .
   Measured at 1024x1024, over data/recordings/wide.txt with DB4A_MAPCHECK:
   the map-sourced margin stays at the same 0.03% mismatch it has at 400x224,
   the cartridge's own 320x224 is byte-exact inside the larger view, and the
   80-entry sprite table never once saturates (0 of 14661 frames over a whole
   mission). See docs/widescreen.md.
   .
   Cost: FB is 3 bytes a pixel and plane_hi, plane_any and `taken` in
   src/render.c are 1 each, so the buffers are 6 bytes a pixel -- 6 MiB at
   1024x1024, against 768 KiB at the old 512x256. All of it is BSS, so the
   binary on disk does not grow and untouched pages are never faulted in: a
   320x224 run resident set is unchanged. */
#define FB_W 1024
#define FB_H 1024
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
