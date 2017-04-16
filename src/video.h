#pragma once

void int10(void);
// Redraws terminal screen
void check_screen(void);
// Returns 1 if video emulation is active.
int video_active(void);
// Writes a character to the video screen
void video_putch(char ch);
