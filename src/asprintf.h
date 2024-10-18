#ifdef _AIX
# ifndef NEED_ASPRINTF
#  define NEED_ASPRINTF
#include <stdarg.h>
int vasprintf(char **str, const char *fmt, va_list ap);
int asprintf(char **str, const char *fmt, ...);
# endif
#endif
