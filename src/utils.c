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
