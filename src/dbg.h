#pragma once

#include <stdio.h>

extern char *prog_name;

void print_version(int quit);
void print_usage(void);
void print_usage_error(const char *format, ...);
void print_error(const char *format, ...);

enum debug_type
{
    debug_cpu,
    debug_int,
    debug_port,
    debug_dos,
    debug_video,
    debug_MAX
};

void init_debug(const char *name);
void debug(enum debug_type, const char *format, ...);
int debug_active(enum debug_type);
