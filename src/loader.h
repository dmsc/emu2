#pragma once

#include <stdint.h>
#include <stdio.h>

// EXE loader
uint16_t create_PSP(const char *cmdline, const char *environment, int env_size,
                    const char *progname);
unsigned get_current_PSP(void);
void set_current_PSP(unsigned psp_seg);

// DOS Memory handling
int mem_resize_segment(int seg, int size);
void mem_free_segment(int seg);
int mem_alloc_segment(int size, int *max);
uint8_t mem_get_alloc_strategy(void);
void mem_set_alloc_strategy(uint8_t s);

// Init internal memory handling
void mcb_init(uint16_t mem_start, uint16_t mem_end);

// Loaders
int dos_load_exe(FILE *f, uint16_t psp_mcb);
int dos_read_overlay(FILE *f, uint16_t load_seg, uint16_t reloc_seg);
