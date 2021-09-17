#pragma once

#include <stdint.h>

void int10_v(void);
// Redraws terminal screen
void check_screen_v(void);
// Returns 1 if video emulation is active.
int video_active_v(void);
// Writes a character to the video screen
void video_putch_v(char ch);
// Get current column in current page
int video_get_col_v(void);
