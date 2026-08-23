#ifndef SPLASH_H
#define SPLASH_H

/* A start-up notice saying what this build is.
 *
 * db4a is an unofficial native port, not a Sega release and not connected to
 * the rights holders, and anyone who runs it should be told so before they see
 * the game's own title screen. It also carries the repository address, so a
 * copy that has travelled away from here can be traced back.
 *
 * Drawn into the emulator's framebuffer with a font written for this file --
 * the project has no text rendering otherwise, and a splash screen is not a
 * good reason to take on a font dependency.
 *
 * `splash = 0` in db4a.conf skips the wait.
 */
void splash_draw(unsigned frame);   /* paint one frame of the notice into FB */
int  splash_frames(void);           /* how long to hold it, 0 = disabled */

#endif
