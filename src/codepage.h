#pragma once

#include <stdint.h>

/* Set the current codepage to the given one.
 * You can pass a number (like "437", or "852"), a short name (like "CP437", or
 * "IBM850") or a file name with the codepage definition.
 */
void set_codepage(const char *cp_name);

/* Set codepage from environmebt variable, if found */
void init_codepage(void);

/* Transforms a DOS char to Unicode */
int get_unicode(uint8_t cp);

/* Transforms a Unicode code-point to the DOS char */
int get_dos_char(int uc);
