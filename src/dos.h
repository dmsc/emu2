#pragma once

#include <stdint.h>
#include <stdio.h>

void init_dos(int argc, char **argv);
void int20(void);
void int21(void);
void int22(void);
void int28(void);
uint32_t get_static_memory(uint16_t bytes, uint16_t align);
