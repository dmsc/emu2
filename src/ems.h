#ifndef EMS_H
#define EMS_H

#ifdef EMS_SUPPORT

#include <stdint.h>

#define EMS_PAGEFRAME_SEG 0xD000

extern int use_ems;

static inline int
in_ems_pageframe(uint32_t addr)
{
    if (use_ems &&
	addr >= (EMS_PAGEFRAME_SEG << 4) &&
	addr < (EMS_PAGEFRAME_SEG << 4) + 0x10000)
	return 1;
    return 0;
}

static inline int
in_ems_pageframe2(uint32_t addr, int size)
{
    if (use_ems &&
	addr + size - 1 >= (EMS_PAGEFRAME_SEG << 4) &&
	addr - size + 1< (EMS_PAGEFRAME_SEG << 4) + 0x10000)
	return 1;
    return 0;
}

void init_ems(int pages);
int ems_get8(int addr);
void ems_put8(int addr, int val);
int ems_putmem(uint32_t dest, const uint8_t *src, unsigned size);
int ems_getmem(uint8_t *dest, uint32_t src, unsigned size);
void int67(void);

#endif /* EMS_SUPPORT */

#endif /* EMS_H */
