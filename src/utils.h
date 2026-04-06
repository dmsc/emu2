/* Platform dependent utility functions */
#pragma once

/* Returns the full path to the program executable */
const char *get_program_exe_path(void);

/* Define a function to get time used to slow-down emulation.
 * This uses CLOCK_MONOTONIC if available, otherwise uses standard gettimeofday */
#include <time.h>
#if defined(CLOCK_MONOTONIC)
# define EMU_CLOCK_TYPE struct timespec
#else
# include <sys/time.h>
# define EMU_CLOCK_TYPE struct timeval
#endif

/* Sets "ret" to the current time. */
void emu_get_time(EMU_CLOCK_TYPE *ret);

/* Advances time in "tm" adding "us" microseconds. */
void emu_advance_time(int us, EMU_CLOCK_TYPE *tm);

/* Returns true if current time is more than target */
int emu_compare_time(EMU_CLOCK_TYPE *target);
