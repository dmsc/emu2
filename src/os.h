#ifndef OS_H_INCLUDED
#define OS_H_INCLUDED


/* Detect proper "does not return" annotation */
#if !defined(NORETURN)
# if defined(__STDC_VERSION__)
#  if __STDC_VERSION__ >= 202311L
#   define NORETURN [[noreturn]] /* C23-style */
#  elif __STDC_VERSION__ >= 201112L
#   define NORETURN _Noreturn /* C11-style */
#  endif
# endif
#endif
#if !defined(NORETURN)
# if defined(__GNUC__) || defined(__SUNPRO_C) || defined(__SUNPRO_CC) || \
     defined(__xlc__) || defined(__ibmxl__)
#  define NORETURN __attribute__((noreturn)) /* IBM/Sun/GNU-style */
# endif
#endif
#if !defined(NORETURN)
# define NORETURN /* Fallback */
#endif

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
#if defined(__illumos__) || defined(__sun)
# if !defined(NO_CFMAKERAW)
#  define NO_CFMAKERAW
# endif
#endif

#endif
