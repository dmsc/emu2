#include <stdio.h>
#include "emu.h"
#include "dos.h"

#pragma pack(1)
struct ems_move
{
        uint32_t size;              //Grootte van het te verpl. blok
        uint8_t stype;              //0=DOS, 1=EMS
        uint16_t shandle,soff,sseg;   //Gegevens brongebied
        uint8_t dtype;              //0=DOS, 1=EMS
        uint16_t dhandle,doff,dseg;   //Gegevens doelgebied
};
#pragma pack()


void ems_to_frame(int fys, int handle, int log);
void ems_move(struct ems_move *ptr);

void int67(void)
{
    unsigned ax = cpuGetAX();
    unsigned ah = ax >> 8;
    unsigned al = ax & 0xff;
    //printf("EMS...calling the interrupt\n");
    switch(ah)
    {
    case 0x40: // status
        cpuSetAX(ax & 0xff); // OK
        break;
    case 0x41: // frame
        // out: bx=frame
        cpuSetAX(ax & 0xff); // OK
        cpuSetBX(0xe000);
        break;
    case 0x44: // map
        // in: al=fys, dx=hand, bx=log
        // handle not used yet
        ems_to_frame(al, cpuGetDX(), cpuGetBX());
        cpuSetAX(ax & 0xff); // OK
        break;
    case 0x43: // get handle & alloc (DUMMY)
    case 0x5a: // get handle & alloc std/raw (DUMMY)
        // in: bx=nr_pages
        // out: dx=handle
        cpuSetDX(1); // 1 is dummy
        cpuSetAX(ax & 0xff); // OK
        break;
    case 0x51: // realloc (DUMMY)
        // in: bx=nr_pages dx=handle
        // out: bx=nr_pages
        cpuSetAX(ax & 0xff); // OK
        break;
    case 0x57: // move
        if(al == 0) // move
        {
            // in: ds:si=structure
            struct ems_move *ptr =
                (struct ems_move *)(memory + cpuGetDS()*16 + cpuGetSI());
            ems_move(ptr);
            // "projection" needed here...
        }
        else if(al == 1) // exchange
        {
            // in: ds:si=structure
        }
        cpuSetAX(ax & 0xff); // OK
        break;
    case 0x45: // free (DUMMY)
        cpuSetAX(ax & 0xff); // OK
        break;
    case 0x42: // count (DUMMY)
        // out: dx=tot_pages bx=free_pages
        cpuSetDX(128); // dummy
        cpuSetBX(64); // dummy
        cpuSetAX(ax & 0xff); // OK
        break;
    default:
        printf("Not implemented: %02x!", ah);
        break;
    }
}

// as a test we always use handle nr 1
// which has 1 MiB
static uint8_t ems_memory[0x100000]; // 1 MiB
static int16_t ems_map[4]={-1, -1, -1, -1};
static int16_t emsh_map[4]={-1, -1, -1, -1};

// sync frame back to ems, to emulate mapping
void frame_to_ems(int fys, int handle, int log)
{
    int size=0x4000;
    int fys_off=0xe0000+fys*size;
    int log_off=log*size;
    memcpy(ems_memory+log_off, memory+fys_off, size);
}
// for map
void ems_to_frame(int fys, int handle, int log)
{
    // if(ems_map[fys]>=0) frame_to_ems(fys, emsh_map[fys], ems_map[fys]);
    // is ok if frame holds different page...
    int size=0x4000;
    int fys_off=0xe0000+fys*size;
    int log_off=log*size;
    memcpy(memory+fys_off, ems_memory+log_off, size);
    ems_map[fys]=log;
    emsh_map[fys]=handle;
}

// for ems move
// copy ems memory to dos memory...
/*
void copy_ems_to_dos(int dos_addr, int handle, int ems_addr, int size)
{
    for(int fys=0; fys<4; fys++)
        if(ems_map[fys]>=0) frame_to_ems(fys, emsh_map[fys], ems_map[fys]);

    // needs to use handle
    memcpy(memory+dos_addr, ems_memory+ems_addr, size);
}
// copy dos memory to ems memory...
void copy_dos_to_ems(int dos_addr, int handle, int ems_addr, int size)
{
    // needs to use handle
    memcpy(ems_memory+ems_addr, memory+dos_addr, size);
}
*/

void ems_move(struct ems_move *emove)
{
    // update EMS with frame contents
    for(int fys=0; fys<4; fys++)
        if(ems_map[fys]>=0) frame_to_ems(fys, emsh_map[fys], ems_map[fys]);
    // the move
    if(emove->stype==0 && emove->dtype==1)
    {
        // DOS to EMS
        memmove(
                ems_memory + emove->dseg*0x4000 + emove->doff,
                memory + emove->sseg*16 + emove->soff,
                emove->size
              );

    }
    else if(emove->stype==1 && emove->dtype==0)
    {
        // EMS to DOS
        memmove(
                memory + emove->dseg*16 + emove->doff,
                ems_memory + emove->sseg*0x4000 + emove->soff,
                emove->size
              );
    }
    else if(emove->stype==1 && emove->dtype==1)
    {
        // EMS to EMS
        memmove(
                ems_memory + emove->dseg*0x4000 + emove->doff,
                ems_memory + emove->sseg*0x4000 + emove->soff,
                emove->size
              );
    }
    else if(emove->stype==0 && emove->dtype==0)
    {
        // DOS to EMS
        memmove(
                memory + emove->dseg*16 + emove->doff,
                memory + emove->sseg*16 + emove->soff,
                emove->size
              );

    }
    if(emove->dtype==1){
        for(int fys=0; fys<4; fys++)
            if(ems_map[fys]>=0) ems_to_frame(fys, emsh_map[fys], ems_map[fys]);
    }
}
