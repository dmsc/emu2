#pragma once

#include <stdio.h>

extern char *prog_name;

void print_usage(void);
void print_usage_error(const char *format, ...) __attribute__((format(printf, 1, 2)));
void print_error(const char *format, ...) __attribute__((format(printf, 1, 2)));

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
void debug(enum debug_type, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
int debug_active(enum debug_type);
