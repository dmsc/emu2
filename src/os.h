#ifndef OS_H_INCLUDED
#define OS_H_INCLUDED

/* Platforms which are missing cfmakeraw() */
#if defined(__sun)
# if !defined(NO_CFMAKERAW)
#  define NO_CFMAKERAW
# endif
#endif

#endif
