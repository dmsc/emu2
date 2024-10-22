#ifndef OS_H_INCLUDED
#define OS_H_INCLUDED

/* Detect proper "format" annotation */
#if defined(_MSC_VER) && !defined(__clang__)
# include <stddef.h>
# undef _USE_ATTRIBUTES_FOR_SAL
# define _USE_ATTRIBUTES_FOR_SAL 1
# include <sal.h>
# define PRINTF_FORMAT _Printf_format_string_
# define PRINTF_FORMAT_ATTR(fmt_p, va_p)
#else
# define PRINTF_FORMAT
# if !defined(__SUNPRO_C) && !defined(__SUNPRO_CC)
#  define PRINTF_FORMAT_ATTR(fmt_p, va_p) \
   __attribute__((format (printf, fmt_p, va_p)))
# else
#  define PRINTF_FORMAT_ATTR(fmt_p, va_p)
#  endif
#endif

/* Platforms which are missing cfmakeraw() */
#if defined(__sun)
# if !defined(NO_CFMAKERAW)
#  define NO_CFMAKERAW
# endif
#endif

#endif
