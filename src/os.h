#ifndef OS_H_INCLUDED
#define OS_H_INCLUDED

/* Detect proper "does not return" annotation */
#if !defined(__noreturn)
# if defined(__STDC_VERSION__)
#  if __STDC_VERSION__ >= 202311L
#   define __noreturn [[noreturn]] /* C23-style */
#  elif __STDC_VERSION__ >= 201112L
#   define __noreturn _Noreturn /* C11-style */
#  endif
# endif
#endif
#if !defined(__noreturn)
# if defined(__GNUC__) || defined(__SUNPRO_C) || defined(__SUNPRO_CC) || \
     defined(__xlc__) || defined(__ibmxl__)
#  define __noreturn __attribute__((noreturn)) /* IBM/Sun/GNU-style */
# endif
#endif
#if !defined(__noreturn)
# define __noreturn /* Fallback */
#endif

/* Platforms which are missing cfmakeraw() */
#if defined(__sun)
# if !defined(NO_CFMAKERAW)
#  define NO_CFMAKERAW
# endif
#endif

#endif
