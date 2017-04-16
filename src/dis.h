#pragma once

#include <stdint.h>

const char *disa(const uint8_t *ip, uint16_t reg_ip, int seg_override);
