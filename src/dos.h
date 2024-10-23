#pragma once

#include "os.h"

#include <stdint.h>
#include <stdio.h>

void init_dos(int argc, char **argv);
NORETURN void intr20(void);
void intr21(void);
void intr2f(void);
NORETURN void intr22(void);
void intr28(void);
void intr29(void);
