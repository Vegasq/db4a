#ifndef INPUT_H
#define INPUT_H
#include <stdint.h>

/* Mega Drive 3-button pad. Bit order matches the hardware read. */
enum {
    PAD_UP = 0, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
    PAD_A, PAD_B, PAD_C, PAD_START, PAD_COUNT
};

void    pad_set(int button, int pressed);
uint8_t pad_read_data(int port);          /* $A10003 / $A10005 */
void    pad_write_data(int port, uint8_t v);
void    pad_write_ctrl(int port, uint8_t v);
#endif
