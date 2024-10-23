#pragma once

#include "os.h"

#include <stdio.h>

extern char *prog_name;

void print_version(void);
NORETURN void print_usage(void);
NORETURN void print_usage_error(PRINTF_FORMAT const char *format, ...) PRINTF_FORMAT_ATTR(1, 2);
NORETURN void print_error(PRINTF_FORMAT const char *format, ...) PRINTF_FORMAT_ATTR(1, 2);

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
void debug(enum debug_type, PRINTF_FORMAT const char *format, ...) PRINTF_FORMAT_ATTR(2, 3);
int debug_active(enum debug_type);
