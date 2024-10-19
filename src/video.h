#pragma once

#include <stdint.h>

void intr10(void);
// Redraws terminal screen
void check_screen(void);
// Returns 1 if video emulation is active.
int video_active(void);
// Writes a character to the video screen
void video_putch(char ch);
// Get current column in current page
int video_get_col(void);
// CRTC port read/write
uint8_t video_crtc_read(int port);
void video_crtc_write(int port, uint8_t value);
// Initializes emulated video memory and tables
void video_init_mem(void);
