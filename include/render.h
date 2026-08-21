#ifndef RENDER_H
#define RENDER_H
#include <stdint.h>
#include <stdio.h>
#define FB_W 320
#define FB_H 224
extern uint8_t FB[FB_H][FB_W][3];
void render_frame(void);
void render_sprites(void);
int  render_write_ppm(const char *path);
#endif
