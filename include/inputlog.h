#ifndef INPUTLOG_H
#define INPUTLOG_H
#include <stdint.h>

/* Record and replay exact controller input.
 *
 * Scripted scenarios express intent ("press down twice") and have to guess at
 * things the game does not make observable -- how far a cursor travels, where
 * the camera scrolls to, which tile is under it afterwards. Recording sidesteps
 * all of that: play the sequence once by hand, and the exact presses and
 * releases become a replayable file.
 *
 * Format is plain text, one event per line, so a recording can be read, edited
 * and diffed:
 *
 *     # db4a input recording
 *     frame button down
 *     2400 start 1
 *     2406 start 0
 */

/* Mouse motion, recorded alongside the buttons.
 *
 * Without this a session steered with the pointer cannot be replayed at all:
 * the buttons come back and the camera never moves, so the recording does not
 * reproduce the thing it was made to capture. Three bug reports were lost that
 * way before this existed. Positions are in GAME pixels, already converted
 * from window coordinates, so a replay does not depend on the window size the
 * recording was made at. */
void inputlog_record_mouse(unsigned frame, int x, int y);
int  inputlog_replay_mouse(unsigned frame, int *x, int *y);
int  inputlog_replay_has_mouse(void);

void inputlog_record_open(const char *path);
void inputlog_record(unsigned frame, int button, int down);
void inputlog_record_close(void);

int  inputlog_replay_open(const char *path);   /* 1 if a file was loaded */
void inputlog_replay_frame(unsigned frame);    /* apply this frame's events */
unsigned inputlog_replay_last_frame(void);
#endif
