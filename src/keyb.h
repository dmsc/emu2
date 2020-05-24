#pragma once
#include <stdint.h>

void update_keyb(void);
int getch(int detect_brk);
int kbhit(void);
void int16(void);
uint8_t keyb_read_port(unsigned port);
void suspend_keyboard(void);
