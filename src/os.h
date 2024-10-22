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

/* Platforms which are missing cfmakeraw() */
#if defined(__sun)
# if !defined(NO_CFMAKERAW)
#  define NO_CFMAKERAW
# endif
#endif

#endif
