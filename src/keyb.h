#pragma once
#include <stdint.h>

void update_keyb(void);
int getch(int detect_brk);
int kbhit(void);
void intr16(void);
uint8_t keyb_read_port(unsigned port);
void keyb_write_port(unsigned port, uint8_t value);
void suspend_keyboard(void);
// Disable throttling the next keyboard calls
void keyb_wakeup(void);
void keyb_handle_irq(void);
