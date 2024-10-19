#pragma once

#include <stdint.h>
#include <stdio.h>

void init_dos(int argc, char **argv);
void intr20(void);
void intr21(void);
void intr2f(void);
void intr22(void);
void intr28(void);
void intr29(void);
