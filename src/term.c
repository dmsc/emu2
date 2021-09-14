#define NCURSES_WIDECHAR 1
#include <curses.h>
#include "emu.h"
#include "dbg.h"
#include "term.h"
#include "codepage.h"
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <locale.h>

static int curses_initialized=0;
static int video_initialized=0;
int flag_s=0, flag_c=0;

//void get_cursor_pos(int* x, int* y);
void b8_to_term();

// transform pc (attr+chr) to ncursesw cchar_t (attr+chr)
static cchar_t *mk_cchar(uint16_t x)
{
    static cchar_t y0 = (cchar_t){ 0, { 0 }};
    int y_att = (x&0x700) | ((x&0x7000)>>1);
    if(x&0x8000) y_att|=A_BLINK;
    if(x&0x0800) y_att|=A_BOLD;
    int y_chr=get_unicode(x&0xff);
    cchar_t y = (cchar_t){ y_att, {y_chr} };
    y0=y;
    return &y0;
}
void xx_addch(uint16_t x)
{
    add_wch(mk_cchar(x));
}
uint16_t xx_inch(void)
{
    cchar_t y;
    in_wch(&y);
    int y_chr=y.chars[0]; // ...
    int x = get_dos_char(y_chr);
    int y_att=y.attr;
    if(y_att&A_BLINK) x |= 0x8000;
    if(y_att&A_BOLD)  x |= 0x0800;
    x |= (y_att&0x700) | ((y_att&0x3800)<<1);
    return x;
}
static void _write_box_curses_W(uint16_t *buf, int y1, int x1, int y2, int x2)
{
    for(int i=0; i<y2-y1; i++)
    {
        move(i+y1,0);
        for(int j=0;j<x2-x1;j++)
            xx_addch(*buf++);
    }
}

void cleanup_curses()
{
    //clear();
    refresh();
    if(!flag_s) b8_to_term();
    noraw();
    echo();
    resetty();
    //endwin();
    system("/bin/stty sane");
}
static void init_curses()
{
    if(curses_initialized)
        return;
    setlocale(LC_ALL,"");
    set_codepage("437"); // XXX should be configurable
    stdscr=initscr();
    savetty();

    static uint16_t c[]={0,4,2,6,1,5,3,7};
    start_color();
    int i,j;
    for(i=0;i<8;i++)
        for(j=0;j<8;j++) init_pair( (c[i]<<3)|c[j], j, i);

    //keypad(stdscr,TRUE);
    //meta(stdscr,TRUE);
    raw();
    noecho();
    //echo();
    if(flag_c) scrollok(stdscr,1);
    else scrollok(stdscr,0);
    curses_initialized=1;
    atexit(cleanup_curses);
}

#pragma pack(1)
typedef struct bios_info      //video-bios-gegevens (256 bytes)
{
    uint8_t _xx1[0x45];
    uint32_t fncty_tab;     // "functionality" table
    uint8_t vid_mode;       // video mode
    uint16_t scr_w;         // screen width
    uint16_t vbufsize;      // size of a 1 screen page
    uint16_t vbufoffs;      // offset of actual page
    uint16_t cursor[8];     // cursors for 8 pages
    uint16_t cursor_shape;  // shape/size of the cursor
    uint8_t vpage;          // screen-page
    uint16_t vport;         // video port-address ...
    uint16_t _xx2;
    uint8_t _xx3[0x1d];
    uint8_t scr_h;          // screen height -1
    uint16_t chr_hgt;       // char height
    uint16_t _yy;
    uint8_t _xx4[0x77];
} bios_info;
#pragma pack()

static bios_info *bi;
static void init_functionality_table(void);
static uint16_t b_get_cursor(uint8_t page);


static void init_b8(void)
{
    uint16_t *buf=(uint16_t *)(memory+0xb8000);
    for(int i=0; i<0x4000; i++)
        buf[i]=0x0720;
}
void init_video2(void)
{
    if(video_initialized)
        return;
    if(!curses_initialized)
        if(!flag_s) init_curses();
    bi=(bios_info *)(memory+0x400);
    bi->vid_mode=3;
    bi->vbufsize=0x8000; // ?
    bi->vbufoffs=0;
    init_functionality_table(); // ???
    init_b8();
    int x,y;
    if(curses_initialized)
    {
        /*
        reset_shell_mode();
        getyx(stdscr,y,x); // XXX doesn't work...
        bi->cursor[0]=(y<<8)|x;
        debug(debug_video, "\tsetting cursor to y=%d, x=%d\n", y, x);
        reset_prog_mode();
        */
        bi->vpage=0;
        getmaxyx(stdscr,y,x);
        bi->scr_w=x; // getmaxyx ?
        bi->scr_h=y-1;
        debug(debug_video, "\tsetting screen to %d rows, %d cols\n", y, x);
    }
    else
    {
        bi->scr_w=80;
        bi->scr_h=24;
    }
    video_initialized=1;
}

void b8_to_term()
{
    // used to map b8000 to a curses screen
    if(flag_s) return; // ignore in stdio mode
    if(flag_c) return; // not used if int10 API is translated to curses
    if(!video_initialized)
        init_video2();
    if(!curses_initialized)
        init_curses();
    scrollok(stdscr,0); // don't scroll XXX: redundant -> is already set
    int page=bi->vpage;
    uint16_t *vbuf = (uint16_t *)(memory+0xb8000+page*0x1000);
    _write_box_curses_W(vbuf, 0, 0, bi->scr_h+1, bi->scr_w);
    // we also need to handle the cursor
    unsigned cursor=b_get_cursor(page);
    move( (cursor>>8), (cursor&0xff) );
    curs_set(1);
    refresh();
}

static uint8_t b_get_page(void)
{
    return bi->vpage;
}
static uint16_t b_get_cols(void)
{
    return bi->scr_w;
}
static uint16_t b_get_vmode(void)
{
    return bi->vid_mode;
}
static uint16_t b_get_cursor(uint8_t page)
{
    return bi->cursor[page];
}
static uint16_t b_get_cshape(uint8_t page)
{
    return bi->cursor_shape;
}
static void b_set_cursor(uint8_t page, uint16_t cursor)
{
    bi->cursor[page]=cursor;
}
static void b_move(int page, int y, int x)
{
    b_set_cursor(page, (y<<8)|x);
}
static void b_set_cshape(uint8_t page, uint16_t shape)
{
    bi->cursor_shape=shape;
}
static void b_set_page(uint8_t page)
{
    bi->vpage=page;
    bi->vbufoffs=page*0x1000; // XXX not always 0x1000
}

static void b_scrollup(
    uint8_t page, uint16_t top, uint16_t bot, uint8_t battr, uint8_t nlines)
{
    int w=bi->scr_w, h=bi->scr_h;
    int top_x=top&0xff, bot_x=bot&0xff;
    if((top_x==0) && (bot_x==w) && (nlines==1))
    {
        uint16_t *base=(uint16_t *)(memory+0xb8000+page*0x1000);
        memmove(base, base+w, w*h*2);
        // and add empty line...
        uint16_t *ptr = base + w*h;
        for(int i=0; i<bi->scr_w; i++)
            ptr[i]=(battr<<8)|0x20;
    }
    else
    {
        debug(debug_video, "UNHANDLED partial scroll up (INT 10.%04x)\n",
                cpuGetAX());
    }
}
static void b_scrolldown(
    uint8_t page, uint16_t top, uint16_t bot, uint8_t battr, uint8_t nlines)
{
    int w=bi->scr_w, h=bi->scr_h;
    int top_x=top&0xff, bot_x=bot&0xff;
    if((top_x==0) && (bot_x==w) && (nlines==1))
    {
        uint16_t *base=(uint16_t *)(memory+0xb8000+page*0x1000);
        memmove(base+w, base, w*h*2);
        // and add empty line...
        uint16_t *ptr = base;
        for(int i=0; i<bi->scr_w; i++)
            ptr[i]=(battr<<8)|0x20;
    }
    else
    {
        debug(debug_video, "UNHANDLED partial scroll down (INT 10.%04x)\n",
                cpuGetAX());
    }
}

static uint16_t b_inch(int page)
{
    uint16_t cursor=b_get_cursor(page);
    int idx=(cursor&0xff) + (cursor>>8)*bi->scr_w;
    int offset=page*0x1000;
    uint16_t *ptr=(uint16_t *)(memory+0xb8000+offset+idx*2);
    return *ptr;
}
static void b_addch(int page, uint16_t cha, int rep)
{
    uint16_t cursor=b_get_cursor(page);
    int idx=(cursor&0xff) + (cursor>>8)*bi->scr_w;
    int offset=page*0x1000;
    uint16_t *ptr=(uint16_t *)(memory+0xb8000+offset+idx*2);
    for(int i=0; i<rep; i++)
        *(ptr++)=cha;
}
// XXX should preserve original attributes
static void b_putch(int page, uint8_t ch, int rep)
{
    uint16_t cursor=b_get_cursor(page);
    int idx=(cursor&0xff) + (cursor>>8)*bi->scr_w;
    int offset=page*0x1000;
    uint16_t *ptr=(uint16_t *)(memory+0xb8000+offset+idx*2);
    for(int i=0; i<rep; i++)
    {
        uint8_t *cptr=(uint8_t *)ptr;
        *(cptr)=ch;
        ptr++;
    }
}

// HAPPY 1: suppress scroll na char output XXX
#define HAPPY 0

int easy_switch(int ch, int easy)
{
    switch(easy)
    {
    case 1:
        return ch;
    case 2:
        return 0xff;
    default:
        return ch&0xff;
    }
}

static void b_putchar(int page, uint16_t ch, int easy)
{
    int h=bi->scr_h+1, w=bi->scr_w;
    page &= 7;
    // scroll params:
    int scr_attr=0x07;
    uint16_t scr_beg=1<<8;
    uint16_t scr_end=((bi->scr_h+1)<<8) | bi->scr_w;
    uint16_t cursor=b_get_cursor(page);
    int y=(cursor>>8), x=(cursor&0xff);
    switch( easy_switch(ch,easy) )
    {
    case 0xd:
        x=0;
        break;
    case 0xa:
        y++;
        while(y>=h)
        {
            b_scrollup(page, scr_beg, scr_end, scr_attr, 1);
            y--;
        }
        break;
    case 8:
        if(x>0) x--;
        // FIXME: should delete
        break;
    case 9: // FIXME
    default:
        b_addch(page, ch, 1);
        x++;
        if(x>=w) y++, x-=w;
        if(y>=h && (HAPPY&1)) return; // XXX HAPPY magic
        while(y>=h)
        {
            b_scrollup(page, scr_beg, scr_end, scr_attr, 1);
            y--;
        }
        break;
    }
    b_move(page,y,x);
}

// VIDEO int
void int10_t()
{
    if(!video_initialized)
        init_video2();

    unsigned ax = cpuGetAX();
    unsigned ah=ax>>8, al=ax&0xff;
    unsigned bx = cpuGetBX();
    unsigned bh=bx>>8, bl=bx&0xff;
    unsigned cx = cpuGetCX();
    unsigned dx = cpuGetDX();
    //unsigned dh=dx>>8, dl=dx&0xff;

    debug(debug_video, "\tint 0x10.%02x, rows=%d\n", ah, bi->scr_h+1);
    switch(ah)
    {
    case 0x00: // SET VIDEO MODE
        // XXX
        break;
    case 0x01: // SET CURSOR SHAPE
        b_set_cshape(bh, cx);
        break;
    case 0x02: // SET CURSOR POS
        // bh=page dh=row dl=col
        b_set_cursor(bh, dx);
        b8_to_term();
        break;
    case 0x03: // GET CURSOR POS / SHAPE
        // in: bh=page
        // out: dh=row dl=col ch cl cusor shape
        cpuSetDX(b_get_cursor(bh));
        cpuSetCX(b_get_cshape(bh));
        break;
    case 0x05: // SELECT DISPLAY PAGE
        // in: ah=page
        b_set_page(ah);
        //b8_to_term();
        break;
    case 0x06: // SCROLL UP WINDOW
        // al=no-lines bh=blanc-attr
        // ch,cl=top-left dh,dl=bot-right
        b_scrollup(b_get_page(), cx, dx, bh, al);
        break;
    case 0x07: // SCROLL DOWN WINDOW
        // al=no-lines bh=blanc-attr
        // ch,cl=top-left dh,dl=bot-right
        b_scrolldown(b_get_page(), cx, dx, bh, al);
        break;
    case 0x08: // READ CHAR AT CURSOR
        // in: bh=page
        // out: ah=attr al=char
        cpuSetAX(b_inch(bh));
        break;
    case 0x09: // WRITE CHAR AT CURSOR
        // in: bh=page al=char bl=attr cx=count
        // out: none, doesn't move cursor
        b_addch(bh, (bl<<8)|al, cx);
        b8_to_term();
        break;
    case 0x0A: // WRITE CHAR ONLY AT CURSOR
        // idem, bl ignored
        b_putch(bh, al, cx);
        b8_to_term();
        break;
    case 0x0E: // TELETYPE OUTPUT
        // in: al=char bh=page
        // cursor moves
        // control chars handled
        b_putchar(bh, 0x0700|al, 0); // XXX attr?
        break;
    case 0x0F: // GET VIDEO STATE
        // out: bh=page al=mode ah=screencols
        cpuSetAX((b_get_cols()<<8)|(b_get_vmode()));
        cpuSetBX((b_get_page()<<8)|bl);
        break;
    case 0x10:
        // ignore
        if(ax == 0x1002)
            // TODO: Set pallete registers - ignore
            break;
        else if(ax == 0x1003)
            // TODO: Set blinking state
            break;
        debug(debug_video, "UNHANDLED INT 10, AX=%04x\n", ax);
        break;
    case 0x11: // CHARGEN / FONT
        // 1130: dl returns screen rows...
        if(ax==0x1130)
        {
            cpuSetDX((dx&0xff00)|bi->scr_h);
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
        int page=bh;
        int saved_cursor=b_get_cursor(page);
        uint8_t *strptr_b = memory + (cpuGetES()<<4) + cpuGetBP();
        uint16_t *strptr_w=(uint16_t *)strptr_b;
        b_set_cursor(page, dx);
        for(int i=0; i<cx; i++)
        {
            if(!(al&2)) b_putchar(bh, (bl<<8)|strptr_b[i], 0);
            else b_putchar(bh, strptr_w[i], 0);
        }
        if(!(al&1)) b_set_cursor(page, saved_cursor);
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

void int10(void)
{
    if(flag_c) int10_c();
    else int10_t();
}
int video_active(void)
{
    return 1;
}
void video_putch(char ch)
{
    int page=bi->vpage;
    if(!flag_c) b_putchar(page, 0x0700|ch, 0);
    else
    {
        xx_putchar(0x700|(ch&0xff), 0);
        refresh();
    }
    if(flag_s) putchar(ch);
    if(!flag_s) b8_to_term();
}
int video_get_col(void)
{
    int cursor=b_get_cursor(b_get_page());
    return cursor&0xff;
}


static void init_functionality_table(void)
{
    // Fill the functionality table (used by int10.1b)
    // XXX ??????
    memory[0xC0100] = 0x08; // Only mode 3 supported
    memory[0xC0101] = 0x00;
    memory[0xC0102] = 0x00;
    memory[0xC0107] = 0x07; // Support 300, 350 and 400 scanlines
    memory[0xC0108] = 0x00; // Active character blocks?
    memory[0xC0109] = 0x00; // MAximum character blocks?
    memory[0xC0108] = 0xFF; // Support functions
}


//* crtc emulation copied from "video.c"
// CRTC port emulation, some software use it to fix "snow" in CGA modes.
static uint8_t crtc_port;
static uint16_t crtc_cursor_loc;

uint8_t video_crtc_read(int port)
{
    if(port & 1)
    {
        if(crtc_port == 0x0E)
            return crtc_cursor_loc >> 8;
        if(crtc_port == 0x0F)
            return crtc_cursor_loc;
        else
            return 0;
    }
    else
        return crtc_port;
}

void video_crtc_write(int port, uint8_t value)
{
    if(port & 1)
    {
        if(crtc_port == 0x0E)
            crtc_cursor_loc = (crtc_cursor_loc & 0xFF) | (value << 8);
        if(crtc_port == 0x0F)
            crtc_cursor_loc = (crtc_cursor_loc & 0xFF00) | (value);
        else
            debug(debug_video, "CRTC port write [%02x] <- %02x\n",
                    crtc_port, value);
    }
    else
        crtc_port = value;
}
// */
