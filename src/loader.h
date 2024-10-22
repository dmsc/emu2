#pragma once

#include <stdint.h>
#include <stdio.h>

// EXE loader
uint16_t create_PSP(const char *cmdline, const char *environment, uint16_t env_size,
                    const char *progname);
unsigned get_current_PSP(void);
void set_current_PSP(uint16_t psp_seg);

// DOS Memory handling
uint16_t mem_resize_segment(uint16_t seg, uint16_t size);
void mem_free_segment(uint16_t seg);
uint16_t mem_alloc_segment(uint16_t size, uint16_t *max);
uint8_t mem_get_alloc_strategy(void);
void mem_set_alloc_strategy(uint8_t s);

// Init internal memory handling
void mcb_init(uint16_t mem_start, uint16_t mem_end);

// Loaders
int dos_load_exe(FILE *f, uint16_t psp_mcb);
int dos_read_overlay(FILE *f, uint16_t load_seg, uint16_t reloc_seg);
