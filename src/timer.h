#pragma once

#include <stdint.h>

// BIOS TIMER code
void update_timer(void);
uint32_t get_bios_timer(void);
void intr1A(void);
uint8_t port_timer_read(uint16_t port);
void port_timer_write(uint16_t port, uint8_t val);
