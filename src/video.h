#pragma once

#include <stdint.h>

void int10(void);
void int10_v(void);
// Redraws terminal screen
void check_screen(void);
void check_screen_v(void);
// Returns 1 if video emulation is active.
int video_active(void);
int video_active_v(void);
// Writes a character to the video screen
void video_putch(char ch);
void video_putch_v(char ch);
// Get current column in current page
int video_get_col(void);
int video_get_col_v(void);
// CRTC port read/write
uint8_t video_crtc_read(int port);
void video_crtc_write(int port, uint8_t value);
