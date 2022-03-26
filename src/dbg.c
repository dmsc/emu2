#include "dbg.h"
#include "env.h"
#include "version.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

char *prog_name;

void print_usage(void)
{
    printf("EMU2 - Simple x86 + DOS Emulator, version " EMU2_VERSION "\n"
           "\n"
           "Usage: %s [options] <prog.exe> [args...] [-- environment vars]\n"
           "\n"
           "Options (processed before program name):\n"
           "  -h            Show this help.\n"
           "  -b <addr>     Load header-less binary at address.\n"
           "  -r <seg>:<ip> Specify a run address to start execution.\n"
           "                (only for binary loaded data).\n"
           "\n"
           "Environment variables:\n"
           "  %-18s  Base name of a file to write the debug log, defaults to\n"
           "\t\t      the exe name if not given.\n"
           "  %-18s  List of debug options to activate, from the following:\n"
           "\t\t      'cpu', 'int', 'port', 'dos', 'video'.\n"
           "  %-18s  DOS program name, if not given use the unix name.\n"
           "  %-18s  DOS default (current) drive letter, if not given use 'C:'\n"
           "  %-18s  DOS current working directory, use 'C:\\' if not given.\n"
           "  %-18s  Set unix path as root of drive 'n', by default all drives\n"
           "\t\t      point to the unix working directory.\n"
           "  %-18s  Set DOS code-page. Set to '?' to show list of code-pages.\n"
           "  %-18s  Limit DOS memory to 512KB, fixes some old buggy programs.\n"
           "  %-18s  Specifies a DOS append paths, separated by ';'.\n",
           prog_name, ENV_DBG_NAME, ENV_DBG_OPT, ENV_PROGNAME, ENV_DEF_DRIVE, ENV_CWD,
           ENV_DRIVE "n", ENV_CODEPAGE, ENV_LOWMEM, ENV_APPEND);
    exit(EXIT_SUCCESS);
}

void print_usage_error(const char *format, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", prog_name);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fprintf(stderr, "\nTry '%s -h' for more information.\n", prog_name);
    exit(EXIT_FAILURE);
}

void print_error(const char *format, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", prog_name);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

static FILE *debug_files[debug_MAX];
static const char *debug_names[debug_MAX] = {"cpu", "int", "port", "dos", "video"};

static FILE *open_log_file(const char *base, const char *type)
{
    char log_name[64 + strlen(base) + strlen(type)];
    int fd = -1;
    for(int i = 0; fd == -1 && i < 1000; i++)
    {
        sprintf(log_name, "%s-%s.%d.log", base, type, i);
        fd = open(log_name, O_CREAT | O_EXCL | O_WRONLY, 0666);
    }
    if(fd == -1)
        print_error("can't open debug log '%s'\n", log_name);
    fprintf(stderr, "%s: %s debug log on file '%s'.\n", prog_name, type, log_name);
    return fdopen(fd, "w");
}

void init_debug(const char *base)
{
    if(getenv(ENV_DBG_NAME))
        base = getenv(ENV_DBG_NAME);
    if(getenv(ENV_DBG_OPT))
    {
        // Parse debug types:
        const char *spec = getenv(ENV_DBG_OPT);
        for(int i = 0; i < debug_MAX; i++)
        {
            if(strstr(spec, debug_names[i]))
                debug_files[i] = open_log_file(base, debug_names[i]);
        }
    }
}

int debug_active(enum debug_type dt)
{
    if(dt >= 0 && dt < debug_MAX)
        return debug_files[dt] != 0;
    else
        return 0;
}

void debug(enum debug_type dt, const char *format, ...)
{
    va_list ap;
    if(debug_active(dt))
    {
        va_start(ap, format);
        vfprintf(debug_files[dt], format, ap);
        va_end(ap);
        fflush(debug_files[dt]);
    }
}
