/* Platform dependent utility functions */
#include "utils.h"
#include "dbg.h"
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef __HAIKU__
#include <OS.h>
#include <FindDirectory.h>
#endif

#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif

#if defined(__GNU__) && !defined(__linux__) && !defined(__HAIKU__)
# define GNU_HURD
#endif
#if defined(__linux__) || defined(__CYGWIN__) || \
    defined(__serenity__) || defined(GNU_HURD) || defined(__EMSCRIPTEN__)
# define PROC_SELF "/proc/self/exe"
#elif defined(__NetBSD__)
# define PROC_SELF "/proc/curproc/exe"
#elif defined(__DragonFly__)
# define PROC_SELF "/proc/curproc/file"
#elif defined(__illumos__) || defined(__sun)
# define PROC_SELF "/proc/self/path/a.out"
#endif

const char *get_program_exe_path(void)
{
#if defined(__linux__) || defined(__NetBSD__) || defined(__illumos__) || \
    defined(__sun) || defined(__DragonFly__) || defined(__CYGWIN__) || \
    defined(__serenity__) || defined(GNU_HURD) || defined(__EMSCRIPTEN__)

    static char exe_path[4096] = {
        0,
    };
    if(readlink(PROC_SELF, exe_path, 4095) == -1)
        return 0;
    else
        return exe_path;

#elif defined(__APPLE__)

    static char exe_path[4096] = {
        0,
    };
    uint32_t length = 4095;
    if(_NSGetExecutablePath(exe_path, &length))
        return 0;
    else
        return exe_path;

#elif defined(__HAIKU__)

    static char exe_path[4096] = {
        0,
    };
    status_t ret = find_path(B_APP_IMAGE_SYMBOL,
                             B_FIND_PATH_IMAGE_PATH,
                             NULL, exe_path, 4095);
    if(ret == B_OK)
        return exe_path;
    else
        return 0;

#elif defined(__FreeBSD__)

    static char exe_path[4096] = {
        0,
    };
    size_t size = 4095;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    int ret = sysctl(mib, 4, exe_path, &size, NULL, 0);
    if(ret == 0)
        return exe_path;
    else
        return 0;

#else

    /* No implementation */
    return 0;

#endif
}

#if defined(CLOCK_MONOTONIC)
# define CLK_MULT   1000
# define CLK_SEC    1000000000
# define CLK_MIN    tv_nsec
# define CLK_GET(a) clock_gettime(CLOCK_MONOTONIC, a)
#else
# define CLK_MULT   1
# define CLK_SEC    1000000
# define CLK_MIN    tv_usec
# define CLK_GET(a) gettimeofday(a, 0)
#endif

void emu_get_time(EMU_CLOCK_TYPE *ret)
{
    CLK_GET(ret);
}

/* Advances time in "tm" adding "us" microseconds. */
void emu_advance_time(int us, EMU_CLOCK_TYPE *tm)
{
    while(us >= 1000000)
    {
        tm->tv_sec++;
        us -= 1000000;
    }

    tm->CLK_MIN += us * CLK_MULT;
    while(tm->tv_nsec >= CLK_SEC)
    {
        tm->tv_sec++;
        tm->tv_nsec -= CLK_SEC;
    }
}

/* Returns true if time "left" is more or equal than time "right" */
static int emu_compare_times(EMU_CLOCK_TYPE *left, EMU_CLOCK_TYPE *right)
{
    return (left->tv_sec > right->tv_sec) ||
           ((left->tv_sec == right->tv_sec) && (left->CLK_MIN >= right->CLK_MIN));
}

/* Returns true if current time is more than target */
int emu_compare_time(EMU_CLOCK_TYPE *target)
{
    EMU_CLOCK_TYPE now;
    emu_get_time(&now);
    return emu_compare_times(&now, target);
}
