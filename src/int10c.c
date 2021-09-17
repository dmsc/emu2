#define NCURSES_WIDECHAR 1
#include <curses.h>
#ifdef getstr
#undef getstr
#endif
#include "emu_base.h"
#include "dbg.h"
#include "codepage.h"
#include "term.h"
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <locale.h>

static int dummy; // used to make a warning go away...
void xx_putchar(uint16_t ch, int easy)
{
    int y,x; getyx(stdscr,y,x);
    int h; getmaxyx(stdscr,h,dummy);
    switch(easy_switch(ch, easy))
    {
    case 0xd:
        x=0;
        break;
    case 0xa:
        y++;
        while(y>=h)
        {
            wscrl(stdscr,1);
            y--;
        }
        break;
    case 8:
        if(x>0) x--;
        break;
    case 9: // FIXME: add support for tabs
    default:
        xx_addch(ch);
        getyx(stdscr,y,x);
        while(y>=h)
        {
            wscrl(stdscr,1);
            y--;
        }
        break;
    }
    move(y,x);
}


// VIDEO int
// try to translate int 0x10 to curses calls
void int10_c()
{
    init_video2();
    unsigned ax = cpuGetAX();
    unsigned ah=ax>>8, al=ax&0xff;
    unsigned bx = cpuGetBX();
    //unsigned bh=bx>>8, bl=bx&0xff;
    unsigned bl=bx&0xff;
    unsigned cx = cpuGetCX();
    unsigned dx = cpuGetDX();
    //unsigned dh=dx>>8, dl=dx&0xff;

    //debug(debug_video, "\tint 0x10.%02x, rows=%d\n", ah, _bi->scr_hgt+1);
    switch(ah)
    {
    case 0x00: // SET VIDEO MODE
        // ignore
        break;
    case 0x01: // SET CURSOR SHAPE
        // in: bh=page cx=shape
        // TODO: b_set_cshape(bh, cx);
        break;
    case 0x02: // SET CURSOR POS
        // bh=page dh=row dl=col
        move(dx>>8,dx&0xff);
        refresh();
        break;
    case 0x03: // GET CURSOR POS / SHAPE
    {
        // in: bh=page
        // out: dh=row dl=col ch cl cusor shape
        int x,y;
        getyx(stdscr,y,x);
        cpuSetDX((y<<8)|(x&0xff));
        // TODO: cpuSetCX(b_get_cshape(bh));
        refresh();
        break;
    }
    case 0x05: // SELECT DISPLAY PAGE
        // in: ah=page
        // ignore
        break;
    case 0x06: // SCROLL UP WINDOW
        // al=no-lines bh=blanc-attr
        // ch,cl=top-left dh,dl=bot-right
        wscrl(stdscr,1);
        refresh();
        break;
    case 0x07: // SCROLL DOWN WINDOW
        // al=no-lines bh=blanc-attr
        // ch,cl=top-left dh,dl=bot-right
        wscrl(stdscr,-1);
        refresh();
        break;
    case 0x08: // READ CHAR AT CURSOR
        // in: bh=page
        // out: ah=attr al=char
        cpuSetAX(xx_inch());
        break;
    case 0x09: // WRITE CHAR AT CURSOR
    {
        // in: bh=page al=char bl=attr cx=count
        // out: none, doesn't move cursor
        // XXX scroll handling
        scrollok(stdscr,0);
        int y,x; getyx(stdscr,y,x);
        for(int i=0; i<cx; i++)
            xx_addch((bl<<8)|al);
        move(y,x);
        scrollok(stdscr,1);
        refresh();
        break;
    }
    case 0x0A: // WRITE CHAR ONLY AT CURSOR
        // idem, bl ignored
        //b_putch(bh, al, cx);
        //b8_to_term();
        for(int i=0; i<cx; i++)
            xx_addch((xx_inch()&0xff00)|al);
        refresh();
        break;
    case 0x0E: // TELETYPE OUTPUT
        // in: al=char bh=page
        // cursor moves
        // control chars handled
        xx_putchar(al,0); // XXX attr behaviour...
        refresh();
        break;
    case 0x0F: // GET VIDEO STATE
    {
        // out: bh=page al=mode ah=screencols
        // XXX mode=3
        int x; getmaxyx(stdscr,dummy,x);
        cpuSetAX((x<<8)|0x03);
        cpuSetBX(bx&0xff);
        break;
    }
    case 0x10:
        // ignore
        if(ax == 0x1002)
            // Set pallete registers - ignore
            break;
        else if(ax == 0x1003)
            // Set blinking state
            break;
        debug(debug_video, "UNHANDLED INT 10, AX=%04x\n", ax);
        break;
    case 0x11: // CHARGEN / FONT
        // 1130: dl returns screen rows...
        if(ax==0x1130)
        {
            int y; getmaxyx(stdscr,y,dummy);
            //cpuSetDX((dx&0xff00)|_bi->scr_hgt);
            cpuSetDX((dx&0xff00)|(y-1));
            // cx ? XXX
            // es:bp ? table
        }
        else
            debug(debug_video, "UNHANDLED INT 10, AX=%04x\n", ax);
        break;
    case 0x12: // ALT FUNCTION SELECT
    {
        int bl = cpuGetBX() & 0xFF;
        if(bl == 0x10) // GET EGA INFO
        {
            cpuSetBX(0x0003);
            cpuSetCX(0x0000);
            cpuSetAX(0);
        }
        else if(bl == 0x30) // SET VERTICAL RESOLUTION
        {
            // TODO: select 25/28 lines
            cpuSetAX(0x1212);
        }
        else
            debug(debug_video, "UNHANDLED INT 10, AH=12 BL=%02x\n", bl);
        break;
    }
    case 0x13: // WRITE STRING
    {
        // in: es:bp=ptr cx=length
        // bh=page dh=row dl=col
        // al=write-mode bl=attr(mode<2)
        debug(debug_video, "testing INT 10, AX=%04x\n", ax);
        int sy,sx; getyx(stdscr,sy,sx);
        //int my,mx; getmaxyx(stdscr,my,mx);
        uint8_t *strptr_b = memory + (cpuGetES()<<4) + cpuGetBP();
        uint16_t *strptr_w=(uint16_t *)strptr_b;
        move(dx>>8, dx&0xff);
        for(int i=0; i<cx; i++)
        {
            if(!(al&2)) xx_putchar((bl<<8)|strptr_b[i], 0);
            else xx_putchar(strptr_w[i], 0);
        }
        if(!(al&1)) move(sy,sx);
        refresh();
        break;
    }
    case 0x1A: // GET/SET DISPLAY COMBINATION CODE
        cpuSetAX(0x001A);
        cpuSetBX(0x0008); // VGA + analog color display
        break;
    case 0x1B: // STATE INFO
    {
        // TODO check...
        // in: bx=0 es:di=buffer (64 bytes)
        if(bx==0)
        {
            uint8_t *buffer = memory + (cpuGetES()<<4) + cpuGetDI();
            uint8_t *src = memory + 0x445;
            memcpy(buffer, src, 64);
            buffer[0]=0x00;
            buffer[0]=0x01;
            buffer[0]=0x00;
            buffer[0]=0xc0;
            cpuSetAX((ah<<8)|0x1b);
        }
        break;
    }
    case 0x1C: // SAVE/RESTORE VGA STATE
        // ignore
        break;
    case 0xEF: // TEST MSHERC.COM DISPLAY TYPE
        // Ignored
        break;
    default:
        debug(debug_video, "UNHANDLED INT 10, AX=%04x\n", ax);
    }
}

