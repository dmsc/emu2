#include "ems.h"
#include "emu.h"
#include "dbg.h"

#include <stdlib.h>

#ifdef EMS_SUPPORT

int use_ems = 0;

#define EMS_MAXHANDLES	255
#define EMS_PAGESIZE	(16*1024)
#define EMS_HEADER_SEG	(EMS_PAGEFRAME_SEG + 0x1000)

static unsigned ems_maxpages;
static unsigned ems_freepages;
static unsigned ems_handle_cnt;

#define EMS_ADDR_BEGIN	(EMS_PAGEFRAME_SEG << 4)
#define EMS_ADDR_END	(EMS_ADDR_BEGIN + 0x10000)

enum EMM_STATUS {
    EMM_STATUS_SUCCESS = 0x00,
    EMM_STATUS_MALFUNCTION_SOFT = 0x80,
    EMM_STATUS_MALFUNCTION_HARD = 0x81,
    EMM_STATUS_HANDLE_NOT_FOUND = 0x83,
    EMM_STATUS_NOT_DEFINED = 0x84,
    EMM_STATUS_HANDLE_EXTHOUSTED = 0x85,
    EMM_STATUS_HAVE_PAGE_SAVE_DATA = 0x86,
    EMM_STATUS_MORE_THAN_TOTAL_PAGES = 0x87,
    EMM_STATUS_MORE_THAN_FREE_PAGES = 0x88,
    EMM_STATUS_ZERO_PAGE_COULDNT = 0x89,
    EMM_STATUS_LOGICAL_PAGE_OUT_OF_RANGE = 0x8a,
    EMM_STATUS_PHYSICAL_PAGE_OUT_OF_RANGE = 0x8b,
    EMM_STATUS_PAGE_SAVE_AREA_IS_FULL = 0x8c,
    EMM_STATUS_PAGE_SAVE_AREA_IS_ALREADY_USED = 0x8d,
    EMM_STATUS_PAGE_SAVE_AREA_IS_NOT_USED = 0x8e,
    EMM_STATUS_SUBFUNCTION_IS_NOT_EXIST = 0x8f,
    EMM_STATUS_BAD_ATTRIBUTE = 0x90,
    EMM_STATUS_NON_VOLATILE_IS_NOT_SUPPORTED = 0x91,
    EMM_STATUS_EMS_SRC_AND_DEST_IS_OVERLAPPED_VALID = 0x92,
    EMM_STATUS_EMS_SRC_OR_DEST_IS_OUT_OF_RANGE = 0x93,
    EMM_STATUS_CONVENTIONAL_RANGE_IS_OUT_OF_RANGE = 0x94,
    EMM_STATUS_OFFSET_IS_OUT_OF_RANGE = 0x95,
    EMM_STATUS_REGION_EXCEEDS_1M = 0x96,
    EMM_STATUS_EMS_SRC_AND_DEST_IS_OVERLAPPED_INVALID = 0x97,
    EMM_STATUS_SRC_OR_DEST_TYPE_IS_NOT_SUPPORTED = 0x98,
    EMM_STATUS_ALTER_SPECIFIED_IS_NOT_SUPPPORTED = 0x9a,
    EMM_STATUS_ALTER_MAP_IS_ALLOCATED = 0x9b,
    EMM_STATUS_ALTER_MAP_IS_ZERO = 0x9c,
    EMM_STATUS_ALTER_MAP_IS_NOT_DEFINDED = 0x9d,
    EMM_STATUS_DEDICATE_DMA_CH_IS_NOT_SUPPORTED = 0x9e,
    EMM_STATUS_DEDICATE_DMA_CH_SPECIFIED_IS_NOT_SUPPORTED = 0x9f,
    EMM_STATUS_HANDLE_NAME_NOT_FOUND = 0xa0,
    EMM_STATUS_HANDLE_NAME_ALREADY_EXIST = 0xa1,
    EMM_STATUS_1M_WRAP_DURING_MOVE_EXCHANGE = 0xa2,
    EMM_STATUS_BAD_ARGUMENT = 0xa3,
    EMM_STATUS_OPERATION_DENIED = 0xa4,
};    

struct ems_data;
struct ems_map {
    struct ems_data *ems_data[4];
    int log_page[4];
};

struct ems_data {
    struct ems_data *next;
    unsigned handle;
    unsigned pages;
    uint8_t name[8];
    int is_map_saved;
    struct ems_map saved_map;
    uint8_t memory[1];
};

static struct ems_data *ems_memory;
static struct ems_map ems_map;
static struct ems_map ems_call_save_map;

static uint8_t ems_header[] = {
    // 0x00: dummy
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    // 0x08: EMM signature
    0xff, 0xff, 'E', 'M', 'M', 'X', 'X', 'X',
    // 0x10:
    'X', '0', 0x00, 0xff, 0xff, 0xff, 0xff, 0xff,
    // 0x18: Far JMP to 0x0000:0067 (bios call)
    0xea, 0x67, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 0x20: INT 67h (bios call)
    0xcd, 0x67,
};

int
ems_get8(int addr)
{
    int i;
    if (addr < EMS_ADDR_BEGIN || addr >= EMS_ADDR_END)
	return memory[addr];
    addr -= EMS_ADDR_BEGIN;
    for (i = 0; i < 4; i++) {
	if (addr < EMS_PAGESIZE) {
	    if (ems_map.ems_data[i] == NULL)
		return 0xff;
	    uint8_t *ems_mem = ems_map.ems_data[i]->memory;
	    addr += EMS_PAGESIZE * ems_map.log_page[i];
	    return ems_mem[addr];
	}
	addr -= EMS_PAGESIZE;
    }
    return 0xff;
}

void
ems_put8(int addr, int value)
{
    int i;
    if (addr < EMS_ADDR_BEGIN || addr >= EMS_ADDR_END) {
	memory[addr] = value;
	return;
    }
    addr -= EMS_ADDR_BEGIN;
    for (i = 0; i < 4; i++) {
	if (addr < EMS_PAGESIZE) {
	    if (ems_map.ems_data[i] == NULL)
		return;
	    uint8_t *ems_mem = ems_map.ems_data[i]->memory;
	    addr += EMS_PAGESIZE * ems_map.log_page[i];
	    ems_mem[addr] = value;
	    return;
	}
	addr -= EMS_PAGESIZE;
    }
}

/* XXX This function may be slow, so fix in the future */
int
ems_putmem(uint32_t dest, const uint8_t *src, unsigned size)
{
    unsigned i;
    for (i = 0; i < size; i++)
	ems_put8(dest++, *src++);
    return 0;
}

/* XXX This function may be slow, so fix in the future */
int
ems_getmem(uint8_t *dest, uint32_t src, unsigned size)
{
    unsigned i;
    for (i = 0; i < size; i++)
	*dest++ = ems_get8(src++);
    return 0;
}

void
init_ems(int pages)
{
    ems_freepages = ems_maxpages = pages;

    // Install INT 67 handler for EMS check
    uint32_t ems_header_addr =  (EMS_HEADER_SEG << 4);
    memcpy(memory + ems_header_addr, ems_header, sizeof ems_header);
    put16(0x67*4, 0x18);
    put16(0x67*4+2, EMS_HEADER_SEG);
    
    use_ems = 1;
}

static struct ems_data **
search_handle(unsigned handle)
{
    struct ems_data **pp;
    for (pp = &ems_memory; *pp != NULL; pp = &((*pp)->next)) {
	if ((*pp)->handle == handle) {
	    return pp;
	}
    }
    return NULL;
}

static struct ems_data *
search_handle_name(uint8_t *name)
{
    for (struct ems_data *p = ems_memory; p != NULL; p = p->next) {
	if (memcmp(p->name, name, 8) == 0) {
	    return p;
	}
    }
    return NULL;
}

static void
validate_emsmap(void)
{
    struct ems_data *p = ems_memory;
    for (int i = 0; i < 4; i++) {
	int found = 0;
	for (; p != NULL; p = p->next) {
	    if (p == ems_map.ems_data[i]) {
		if (ems_map.log_page[i] < p->pages) {
		    found = 1;
		}
		break;
	    }
	}
	if (!found)
	    ems_map.ems_data[i] = NULL;
    }
}

static void
set_emm_result(uint16_t ax, uint16_t error)
{
    cpuSetAX((ax & 0x00FF) | (error << 8));
}

void
int67(void)
{
    // EMS interrupt
    unsigned ax = cpuGetAX(), ah = ax >> 8;

    // IP check (call return)
    uint16_t ip = cpuGetIP();
    uint16_t cs = cpuGetCS();
    if (cs == EMS_HEADER_SEG && ip == 0x0022) {
	ems_map = ems_call_save_map;
	set_emm_result(ax, EMM_STATUS_SUCCESS);
	return;
    }
    
    debug(debug_dos,
	  "D-67%04X: BX=%04X CX:%04X DX:%04X DI=%04X DS:%04X ES:%04X\n",
          ax, cpuGetBX(), cpuGetCX(), cpuGetDX(), cpuGetDI(),
	  cpuGetDS(), cpuGetES());
    
    switch (ah) {
    case 0x40: // Get manager status
	set_emm_result(ax, EMM_STATUS_SUCCESS);
	break;

    case 0x41: // Page frame segment
	cpuSetBX(EMS_PAGEFRAME_SEG);
	set_emm_result(ax, EMM_STATUS_SUCCESS);
	break;

    case 0x42: // Get number of pages
	cpuSetBX(ems_freepages);
	cpuSetDX(ems_maxpages);
	set_emm_result(ax, EMM_STATUS_SUCCESS);
	break;

    case 0x43: // Get handle and allocate memory
	{
	    int alloc_pages = cpuGetBX();
	    if (alloc_pages > ems_maxpages) {
		set_emm_result(ax, EMM_STATUS_MORE_THAN_TOTAL_PAGES);
		break;
	    }
	    if (alloc_pages > ems_freepages) {
		set_emm_result(ax, EMM_STATUS_MORE_THAN_FREE_PAGES);
		break;
	    }
	    if (alloc_pages == 0) {
		set_emm_result(ax, EMM_STATUS_ZERO_PAGE_COULDNT);
		break;
	    }
	    unsigned handle = 1;
	    struct ems_data **pp = &ems_memory;
	    while (*pp != NULL) {
		if ((*pp)->handle == handle) {
		    handle++;
		}
		pp = &(*pp)->next;
	    }
	    if (handle > EMS_MAXHANDLES) {
		set_emm_result(ax, EMM_STATUS_HANDLE_EXTHOUSTED);
		break;
	    }
	    
	    int size = sizeof(struct ems_data) + EMS_PAGESIZE * alloc_pages;
	    struct ems_data *new_ems = malloc(size);
	    if (new_ems == NULL) {
		set_emm_result(ax, EMM_STATUS_MALFUNCTION_SOFT);
		break;
	    }
	    memset(new_ems, 0, sizeof(struct ems_data));
	    new_ems->pages = alloc_pages;
	    new_ems->handle = handle;
	    *pp = new_ems;

	    ems_handle_cnt++;
	    ems_freepages -= alloc_pages;
	    cpuSetDX(handle);
	    set_emm_result(ax, EMM_STATUS_SUCCESS);
	    debug(debug_dos,
		  "EMM Alloc:(%d) %d pages\n",
		  handle, alloc_pages);
	}
	break;
	
    case 0x44: // Map memory
	{
	    struct ems_data **pp = search_handle(cpuGetDX());
	    if (pp == NULL) {
		set_emm_result(ax, EMM_STATUS_HANDLE_NOT_FOUND);
		break;
	    }
	    struct ems_data *p = *pp;
	    int phy_page = ax & 0x00FF;
	    int log_page = cpuGetBX();
	    if (log_page != 0xffff && log_page >= p->pages) {
		set_emm_result(ax, EMM_STATUS_LOGICAL_PAGE_OUT_OF_RANGE);
		break;
	    }
	    if (phy_page >= 4) {
		set_emm_result(ax, EMM_STATUS_PHYSICAL_PAGE_OUT_OF_RANGE);
		break;
	    }
	    ems_map.ems_data[phy_page] = log_page == 0xffff ? NULL : p;
	    ems_map.log_page[phy_page] = log_page;
	    //validate_emsmap();
	    set_emm_result(ax, EMM_STATUS_SUCCESS);
	}
	break;
	
    case 0x45: // Release handle and memory
	{
	    struct ems_data **pp = search_handle(cpuGetDX());
	    if (pp == NULL) {
		set_emm_result(ax, EMM_STATUS_HANDLE_NOT_FOUND);
		break;
	    }
	    if ((*pp)->is_map_saved) {
		set_emm_result(ax, EMM_STATUS_HAVE_PAGE_SAVE_DATA);
		break;
	    }

	    ems_freepages += (*pp)->pages;
	    struct ems_data *next = (*pp)->next;
	    free(*pp);
	    *pp = next;
	    ems_handle_cnt--;
	    
	    validate_emsmap();
	    set_emm_result(ax, EMM_STATUS_SUCCESS);
	}
	break;
	    
    case 0x46: // Get EMM version
	cpuSetAX(0x0040); // LIM-EMS 4.0
	break;
	
    case 0x47: // Save mapping context
	{
	    struct ems_data **pp = search_handle(cpuGetDX());
	    if (pp == NULL) {
		set_emm_result(ax, EMM_STATUS_HANDLE_NOT_FOUND);
		return;
	    }
	    struct ems_data *p = *pp;
	    if (p->is_map_saved) {
		set_emm_result(ax, EMM_STATUS_PAGE_SAVE_AREA_IS_ALREADY_USED);
		return;
	    }
	    validate_emsmap();
	    p->saved_map = ems_map;
	    p->is_map_saved = 1;
	    set_emm_result(ax, EMM_STATUS_SUCCESS);
	}
	break;
	
    case 0x48: // Restore mapping context
	{
	    struct ems_data **pp = search_handle(cpuGetDX());
	    if (pp == NULL) {
		set_emm_result(ax, EMM_STATUS_HANDLE_NOT_FOUND);
		break;
	    }
	    struct ems_data *p = *pp;
	    if (! p->is_map_saved) {
		set_emm_result(ax, EMM_STATUS_PAGE_SAVE_AREA_IS_NOT_USED);
		//set_emm_result(ax, EMM_STATUS_SUCCESS);
		break;
	    }
	    p->is_map_saved = 0;
	    ems_map = p->saved_map;
	    validate_emsmap();
	    set_emm_result(ax, EMM_STATUS_SUCCESS);
	}
	break;

    case 0x4B: // Get number of EMM handle
	cpuSetBX(ems_handle_cnt + 1); // +1 for system handle
	set_emm_result(ax, EMM_STATUS_SUCCESS);
	break;
	
    case 0x4C: // Get pages owned by handle
	{
	    struct ems_data **pp = search_handle(cpuGetDX());
	    if (pp == NULL) {
		set_emm_result(ax, EMM_STATUS_HANDLE_NOT_FOUND);
		break;
	    }
	    cpuSetBX((*pp)->pages);
	    set_emm_result(ax, EMM_STATUS_SUCCESS);
	}
	break;

    case 0x4D: // Get pages for all handles
	{
	    uint32_t addr = cpuGetAddrES(cpuGetDI());
	    int handle_num = 1;
	    // System handle
	    put16(addr, 0);
	    put16(addr+2, 0);
	    addr += 4;
	    for (struct ems_data *p = ems_memory; p != NULL; p = p->next) {
		put16(addr, p->handle);
		put16(addr+2, p->pages);
		addr += 4;
		handle_num++;
	    }
	    cpuSetBX(handle_num);
	    set_emm_result(ax, EMM_STATUS_SUCCESS);
	}
	break;
	
    case 0x4E: // Get or set page map
	switch (ax) {
	case 0x4E00: // get all mapping registers
	    {
		uint32_t addr = cpuGetAddrES(cpuGetDI());
		ems_putmem(addr, (const uint8_t *)&ems_map, sizeof ems_map);
		set_emm_result(ax, EMM_STATUS_SUCCESS);
	    }
	    break;
	    
	case 0x4E01: // set all mapping registers
	    {
		uint32_t addr = cpuGetAddrDS(cpuGetSI());
		ems_getmem((uint8_t *)&ems_map, addr, sizeof ems_map);
		validate_emsmap();
		set_emm_result(ax, EMM_STATUS_SUCCESS);
	    }
	    break;

	case 0x4E02: // swap all mapping registers
	    {
		uint32_t old_addr = cpuGetAddrES(cpuGetDI());
		uint32_t new_addr = cpuGetAddrDS(cpuGetSI());
		ems_putmem(old_addr, (const uint8_t *)&ems_map, sizeof ems_map);
		ems_getmem((uint8_t *)&ems_map, new_addr, sizeof ems_map);
		validate_emsmap();
		set_emm_result(ax, EMM_STATUS_SUCCESS);
	    }
	    break;

	case 0x4E03: // query size of buffer
	    cpuSetAX(sizeof ems_map);
	    break;

	default:
	    goto unknown;
	}
	break;
	
    case 0x4F: // 4.0: Get/set partial page map
	/* XXX This function may use memory a lota, so fix in the future */
	switch (ax) {
	case 0x4F00: // get partial mapping registers
	    {
		uint32_t addr = cpuGetAddrES(cpuGetDI());
		uint32_t table = cpuGetAddrDS(cpuGetSI());
		int num = get16(table);
		uint8_t flag = 0;
		for (int i = 0; i < num; i++) {
		    table += 2;
		    int seg = get16(table);
		    flag |= 0x01 << seg;
		}
		ems_putmem(addr, (const uint8_t *)&ems_map, sizeof ems_map);
		put8(addr + sizeof ems_map, flag);
		set_emm_result(ax, EMM_STATUS_SUCCESS);
	    }
	    break;
	    
	case 0x4F01: // set partial mapping registers
	    {
		uint32_t addr = cpuGetAddrDS(cpuGetSI());
		struct ems_map tmp;
		ems_getmem((uint8_t *)&tmp, addr, sizeof ems_map);
		uint8_t flag = get8(addr + sizeof ems_map);
		for (int i = 0; i < 4; i++) {
		    if ((flag & (0x01 << i)) != 0) {
			ems_map.ems_data[i] = tmp.ems_data[i];
			ems_map.log_page[i] = tmp.log_page[i];
		    }
		}
		validate_emsmap();
		set_emm_result(ax, EMM_STATUS_SUCCESS);
	    }
	    break;

	case 0x4F02: // query size of buffer
	    cpuSetAX(sizeof(ems_map) + sizeof(uint8_t));
	    break;

	default:
	    goto unknown;
	}
	break;
	
    case 0x50: // 4.0: Map/unmap multiple handle pages
	{
	    struct ems_data **pp = search_handle(cpuGetDX());
	    struct ems_map tmp = ems_map;
	    if (pp == NULL) {
		set_emm_result(ax, EMM_STATUS_HANDLE_NOT_FOUND);
		break;
	    }
	    struct ems_data *p = *pp;
	    int segmode = 0;
	    
	    if (ax == 0x5000) {
	    }
	    else if (ax == 0x5001) {
		segmode = 1;
	    }
	    else {
		goto unknown;
	    }
		
	    uint32_t table = cpuGetAddrDS(cpuGetSI());
	    int num = cpuGetCX();
	    for (int i = 0; i < num; i++) {
		int log_page = get16(table);
		int phy_page = get16(table+2);
		table += 4;
		
		if (log_page != 0xffff && log_page >= p->pages) {
		    set_emm_result(ax, EMM_STATUS_LOGICAL_PAGE_OUT_OF_RANGE);
		    break;
		}
		if (segmode) {
		    int i;
		    for (i = 0; i < 4; i++) {
			if (phy_page == EMS_PAGEFRAME_SEG + 0x400*i) {
			    phy_page = i;
			    break;
			}
		    }
		    if (i >= 4) {
			set_emm_result(ax,
				       EMM_STATUS_PHYSICAL_PAGE_OUT_OF_RANGE);
		    }
		}
		else if (phy_page > 4) {
		    set_emm_result(ax, EMM_STATUS_PHYSICAL_PAGE_OUT_OF_RANGE);
		    break;
		}
		tmp.ems_data[phy_page] = (log_page == 0xffff) ? NULL : p;
		tmp.log_page[phy_page] = log_page;
	    }
	    ems_map = tmp;
	}
	break;
	
    case 0x51: // 4.0: Reallocate pages
	{
	    struct ems_data **pp = search_handle(cpuGetDX());
	    if (pp == NULL) {
		set_emm_result(ax, EMM_STATUS_HANDLE_NOT_FOUND);
		break;
	    }
	    
	    int alloc_pages = cpuGetBX();
	    if (alloc_pages == (*pp)->pages) {
		set_emm_result(ax, EMM_STATUS_SUCCESS);
		break;
	    }
	    if (alloc_pages > ems_maxpages) {
		set_emm_result(ax, EMM_STATUS_MORE_THAN_TOTAL_PAGES);
		break;
	    }
	    if (alloc_pages > ems_freepages + (*pp)->pages) {
		set_emm_result(ax, EMM_STATUS_MORE_THAN_FREE_PAGES);
		break;
	    }
	    
	    ems_freepages += (*pp)->pages;
	    int size = sizeof(struct ems_data) + EMS_PAGESIZE * alloc_pages;
	    struct ems_data *new_ems = realloc(*pp, size);
	    new_ems->pages = alloc_pages;
	    *pp = new_ems;
	    ems_freepages -= new_ems->pages;
	    
	    validate_emsmap();
	    set_emm_result(ax, EMM_STATUS_SUCCESS);
	}
	break;

    case 0x52: // 4.0: Get/set handle attributes
	{
	    struct ems_data **pp = search_handle(cpuGetDX());
	    if (pp == NULL) {
		set_emm_result(ax, EMM_STATUS_HANDLE_NOT_FOUND);
		break;
	    }
	    switch (ax) {
	    case 0x5200: // query handle attribute
		cpuSetAX(ax & 0xFF00);
		break;
		
	    case 0x5201: // set handle attribute
		set_emm_result(ax, EMM_STATUS_BAD_ATTRIBUTE);
		break;

	    case 0x5202: // query capablility
		cpuSetAX(ax & 0xFF00);
		break;
		
	    default:
		goto unknown;
	    }
	}
	break;

    case 0x53: // 4.0: Get/set handle name
	{
	    struct ems_data **pp = search_handle(cpuGetDX());
	    if (pp == NULL) {
		set_emm_result(ax, EMM_STATUS_HANDLE_NOT_FOUND);
		break;
	    }
	    
	    switch (ax) {
	    case 0x5300: // Get handle name
		ems_putmem(cpuGetAddrES(cpuGetDI()), (*pp)->name, 8);
		set_emm_result(ax, EMM_STATUS_SUCCESS);
		break;
		
	    case 0x5301: // Set handle name
		{
		    uint8_t name[8];
		    ems_getmem(name, cpuGetAddrDS(cpuGetSI()), 8);
		    if (search_handle_name(name) != NULL) {
			set_emm_result(ax,
				       EMM_STATUS_HANDLE_NAME_ALREADY_EXIST);
		    }
		    else {
			memcpy((*pp)->name, name, 8);
			set_emm_result(ax, EMM_STATUS_SUCCESS);
		    }
		}
		break;
		
	    default:
		goto unknown;
	    }
	}
	break;
	
    case 0x54: // 4.0: Get handle directory
	switch (ax) {
	case 0x5400: // get handle directory
	    {
		uint32_t addr = cpuGetAddrES(cpuGetDI());
		int handle_num = 1;
		// System handle
		put16(addr, 0);
		ems_putmem(addr+2, (const uint8_t *)"\0\0\0\0\0\0\0\0", 8);
		for (struct ems_data *p = ems_memory; p != NULL; p = p->next) {
		    put16(addr, p->handle);
		    ems_putmem(addr+2, p->name, 8);
		    addr += 10;
		    handle_num++;
		}
		cpuSetBX(handle_num);
		set_emm_result(ax, EMM_STATUS_SUCCESS);
	    }
	    break;
	    
	case 0x5401: // search for named handle
	    {
		uint8_t name[8];
		ems_getmem(name, cpuGetAddrDS(cpuGetSI()), 8);
		struct ems_data *p = search_handle_name(name);
		if (p != NULL) {
		    cpuSetDX(p->handle);
		    set_emm_result(ax, EMM_STATUS_SUCCESS);
		}
		else {
		    set_emm_result(ax, EMM_STATUS_HANDLE_NAME_NOT_FOUND);
		}
	    }
	    break;
	    
	case 0x5402: // get total number of handles
	    cpuSetBX(ems_handle_cnt + 1); // +1 for system handle
	    //cpuSetBX(EMS_MAXHANDLES);
	    set_emm_result(ax, EMM_STATUS_SUCCESS);
	    break;

	default:
	    goto unknown;
	}
	break;

    case 0x55: // 4.0: Alter page map and jump
    case 0x56: // 4.0: Alter page map and call
	{
	    struct ems_data **pp = search_handle(cpuGetDX());
	    struct ems_map tmp = ems_map;
	    if (pp == NULL) {
		set_emm_result(ax, EMM_STATUS_HANDLE_NOT_FOUND);
		break;
	    }
	    struct ems_data *p = *pp;
	    int segmode = 0;
	    int call = 0;
	    
	    if (ax == 0x5500) {
	    }
	    else if (ax == 0x5501) {
		segmode = 1;
	    }
	    else if (ax == 0x5600) {
		call = 1;
	    }
	    else if (ax == 0x5601) {
		call = 1;
		segmode = 1;
	    }
	    else {
		goto unknown;
	    }
		
	    uint32_t table = cpuGetAddrDS(cpuGetSI());
	    uint16_t target_off = get16(table);
	    uint16_t target_seg = get16(table+2);
	    int num = get8(table+4);

	    table = cpuGetAddress(get16(table + 7), get16(table + 5));
	    for (int i = 0; i < num; i++) {
		int log_page = get16(table);
		int phy_page = get16(table+2);
		table += 4;
		
		if (log_page != 0xffff && log_page >= p->pages) {
		    set_emm_result(ax, EMM_STATUS_LOGICAL_PAGE_OUT_OF_RANGE);
		    break;
		}
		if (segmode) {
		    int i;
		    for (i = 0; i < 4; i++) {
			if (phy_page == EMS_PAGEFRAME_SEG + 0x400*i) {
			    phy_page = i;
			    break;
			}
		    }
		    if (i >= 4) {
			set_emm_result(ax,
				       EMM_STATUS_PHYSICAL_PAGE_OUT_OF_RANGE);
		    }
		}
		else if (phy_page > 4) {
		    set_emm_result(ax, EMM_STATUS_PHYSICAL_PAGE_OUT_OF_RANGE);
		    break;
		}
		tmp.ems_data[phy_page] = (log_page == 0xffff) ? NULL : p;
		tmp.log_page[phy_page] = log_page;
	    }
	    if (call) {
		ems_call_save_map = ems_map;
		ems_map = tmp;
		uint16_t sp = cpuGetSP();
		put16(cpuGetAddrSS(sp - 2), 0x20);
		put16(cpuGetAddrSS(sp - 4), EMS_HEADER_SEG);
		cpuSetSP(sp - 4);
		cpuSetCS(target_seg);
		cpuSetIP(target_off);
	    }
	    else {
		ems_map = tmp;
		cpuSetSP(cpuGetSP() + 6); // remove IP/CS/FLAGS
		cpuSetCS(target_seg);
		cpuSetIP(target_off);
		set_emm_result(ax, EMM_STATUS_SUCCESS);
	    }
	}
	break;
	
    case 0x57: // 4.0: Move/exchange memory region
	{
	    int exchange = 0;
	    if (ax == 0x5700) {
		exchange = 0;
	    }
	    else if (ax == 0x5701) {
		exchange = 1;
	    }
	    else {
		goto unknown;
	    }
	    uint32_t addr = cpuGetAddrDS(cpuGetSI());
	    
	    uint32_t len = get32(addr);
	    int src_type = get8(addr + 4);
	    uint16_t src_handle = get16(addr + 5);
	    uint16_t src_pg = get16(addr + 7);
	    uint32_t src_offset = get16(addr + 9);
	    int dest_type = get8(addr + 11);
	    uint16_t dest_handle = get16(addr + 12);
	    uint16_t dest_pg = get16(addr + 14);
	    uint32_t dest_offset = get16(addr + 16);

	    struct ems_data *src_ems = NULL;
	    struct ems_data *dest_ems = NULL;

	    struct ems_data **pp;
	    if (src_type) {
		pp = search_handle(src_handle);
		if (pp == NULL) {
		    set_emm_result(ax, EMM_STATUS_HANDLE_NOT_FOUND);
		    break;
		}
		src_ems = *pp;
	    }
	    if (dest_type) {
		pp = search_handle(dest_handle);
		if (pp == NULL) {
		    set_emm_result(ax, EMM_STATUS_HANDLE_NOT_FOUND);
		    break;
		}
		dest_ems = *pp;
	    }
	    int overlapped = 0;

	    if (src_ems == NULL && dest_ems == NULL) { /* conv-to-conv */
		uint32_t src_addr = cpuGetAddress(src_pg, src_offset);
		uint32_t dest_addr = cpuGetAddress(dest_pg, dest_offset);
		if (src_addr < dest_addr) {
		    if (src_addr + len > dest_addr) {
			overlapped = 1;
			for (int i = len - 1; i >= 0; i--) {
			    if (exchange) {
				uint8_t tmp = get8(dest_addr + 1);
				put8(dest_addr + i, get8(src_addr + i));
				put8(src_addr + i, tmp);
			    }
			    else {
				put8(dest_addr + i, get8(src_addr + i));
			    }
			}
		    }
		    else {
			for (int i = 0; i < len; i++) {
			    if (exchange) {
				uint8_t tmp = get8(dest_addr + 1);
				put8(dest_addr + i, get8(src_addr + i));
				put8(src_addr + i, tmp);
			    }
			    else {
				put8(dest_addr + i, get8(src_addr + i));
			    }
			}
		    }
		}
		else {
		    if (src_addr < dest_addr + len)
			overlapped = 1;
		    for (int i = 0; i < len; i++) {
			if (exchange) {
			    uint8_t tmp = get8(dest_addr + 1);
			    put8(dest_addr + i, get8(src_addr + i));
			    put8(src_addr + i, tmp);
			}
			else {
			    put8(dest_addr + i, get8(src_addr + i));
			}
		    }
		}
	    }
	    else if (src_ems != NULL && dest_ems == NULL) { /* ems-to-conv */
		uint32_t dest_addr = cpuGetAddress(dest_pg, dest_offset);
		int end_pg = src_pg + (src_offset + len) / EMS_PAGESIZE;
		if (src_ems-> pages < end_pg) {
		    set_emm_result(ax,
				   EMM_STATUS_EMS_SRC_OR_DEST_IS_OUT_OF_RANGE);
		    break;
		}
		src_offset += src_pg * EMS_PAGESIZE;
		
		if (exchange) {
		    uint8_t *buf = malloc(len);
		    if (buf == NULL) {
			set_emm_result(ax, EMM_STATUS_MALFUNCTION_SOFT);
			break;
		    }
		    ems_getmem(buf, dest_addr, len);
		    ems_putmem(dest_addr, &src_ems->memory[src_offset], len);
		    memcpy(&src_ems->memory[src_offset], buf, len);
		    free(buf);
		}
		else {
		    ems_putmem(dest_addr, &src_ems->memory[src_offset], len);
		}
	    }
	    else if (src_ems == NULL && dest_ems != NULL) { /* conv-to-ems */
		uint32_t src_addr = cpuGetAddress(src_pg, src_offset);
		int end_pg = dest_pg + (dest_offset + len) / EMS_PAGESIZE;
		if (dest_ems-> pages < end_pg) {
		    set_emm_result(ax,
				   EMM_STATUS_EMS_SRC_OR_DEST_IS_OUT_OF_RANGE);
		    break;
		}
		dest_offset += dest_pg * EMS_PAGESIZE;
		
		if (exchange) {
		    uint8_t *buf = malloc(len);
		    if (buf == NULL) {
			set_emm_result(ax, EMM_STATUS_MALFUNCTION_SOFT);
			break;
		    }
		    ems_getmem(buf, src_addr, len);
		    ems_putmem(src_addr, &dest_ems->memory[dest_offset], len);
		    memcpy(&dest_ems->memory[dest_offset], buf, len);
		    free(buf);
		}
		else {
		    ems_getmem(&dest_ems->memory[dest_offset], src_addr, len);
		}
	    }
	    else { // ems-to-ems
		int src_end_pg = src_pg + (src_offset + len) / EMS_PAGESIZE;
		int dest_end_pg = dest_pg + (dest_offset + len) / EMS_PAGESIZE;
		if (src_ems->pages < src_end_pg ||
		    dest_ems->pages < dest_end_pg)
		{
		    set_emm_result(ax,
				   EMM_STATUS_EMS_SRC_OR_DEST_IS_OUT_OF_RANGE);
		    break;
		}
		src_offset += src_pg * EMS_PAGESIZE;
		dest_offset += dest_pg * EMS_PAGESIZE;
		
		if ((src_ems == dest_ems) &&
		    ((src_offset < dest_offset &&
		      (src_offset + len) > dest_offset)
		     ||
		     (src_offset > dest_offset &&
		      src_offset < (dest_offset + len)))) {
		    overlapped = 1;
		}
		if (exchange) {
		    uint8_t *buf = malloc(len);
		    if (buf == NULL) {
			set_emm_result(ax, EMM_STATUS_MALFUNCTION_SOFT);
			break;
		    }
		    memcpy(buf, &dest_ems->memory[dest_offset], len);
		    memcpy(&dest_ems->memory[dest_offset],
			   &src_ems->memory[src_offset],
			   len);
		    memcpy(&src_ems->memory[src_offset], buf, len);
		    free(buf);
		}
		else {
		    memmove(&dest_ems->memory[dest_offset],
			    &src_ems->memory[src_offset],
			    len);
		}
	    }
	    set_emm_result(ax,
			   overlapped ?
			   EMM_STATUS_EMS_SRC_AND_DEST_IS_OVERLAPPED_VALID:
			   EMM_STATUS_SUCCESS);
	}
	break;
	
    case 0x58: // 4.0: Get mappable physical address array
	switch (ax) {
	case 0x5800: // get physical addresses
	    {
		uint32_t addr = cpuGetAddrES(cpuGetDI());
		for (int i = 0; i < 4; i++) {
		    put16(addr, EMS_PAGEFRAME_SEG + 0x400 * i);
		    put16(addr + 2, i);
		    addr += 4;
		}
		cpuSetCX(4);
		set_emm_result(ax, EMM_STATUS_SUCCESS);
	    }
	    break;
	    
	case 0x5801: // query num of physical address
	    cpuSetCX(4);
	    set_emm_result(ax, EMM_STATUS_SUCCESS);
	    break;
	    
	default:
	    goto unknown;
	}
	break;
	
    case 0x59: // 4.0: Get expanded memory hardware information
    case 0x5A: // 4.0: Allocate standard/raw pages
    case 0x5C: // 4.0: Prepare expanded memory hadware for warm boot
    case 0x5B: // 4.0: Alternate map register set
    case 0x5D: // 4.0: Enable/disable OS function set functions
	set_emm_result(ax, EMM_STATUS_OPERATION_DENIED);
	break;
    
    unknown:
    default:
	
        debug(debug_dos, "UNHANDLED INT 67, AX=%04x\n", cpuGetAX());
        debug(debug_int, "UNHANDLED INT 67, AX=%04x\n", cpuGetAX());
        cpuSetFlag(cpuFlag_CF);
	set_emm_result(ax, EMM_STATUS_NOT_DEFINED);
    }
    
    debug(debug_dos,
	  "R-67%04X: BX=%04X CX:%04X DX:%04X DI=%04X DS:%04X ES:%04X\n",
          cpuGetAX(), cpuGetBX(), cpuGetCX(), cpuGetDX(), cpuGetDI(),
	  cpuGetDS(), cpuGetES());
}

#endif /* EMS_SUPPORT */
