/* Platform dependent utility functions */
#include "utils.h"
#include "dbg.h"
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#if defined(__illumos__) || ((defined(__sun) || defined(__sun__)) && \
           (defined(__SVR4) || defined(__svr4__)))
# define LIKE_SOLARIS
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
#elif defined(LIKE_SOLARIS)
# define PROC_SELF "/proc/self/path/a.out"
#endif

const char *get_program_exe_path(void)
{
#if defined(__linux__) || defined(__NetBSD__) || defined(LIKE_SOLARIS) || \
    defined(__DragonFly__) || defined(__CYGWIN__) || defined(__serenity__) || \
    defined(GNU_HURD) || defined(__EMSCRIPTEN__)

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
#else

    /* No implementation */
    return 0;

#endif
}
