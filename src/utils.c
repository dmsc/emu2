/* Platform dependent utility functions */
#include "utils.h"
#include "dbg.h"
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

const char *get_program_exe_path(void)
{
#if defined(__linux__) || defined(__CYGWIN__)

    static char exe_path[4096] = {
        0,
    };
    if(readlink("/proc/self/exe", exe_path, 4095) == -1)
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
