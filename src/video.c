#include "video.h"
#include "dbg.h"
#include "emu.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// Simulated character screen.
// This is a copy of the currently displayed output in the terminal window.
// We have a max of 128 rows of up to 256 columns, total of 32KB.
static uint16_t term_screen[64][256];
// Current line in output, lines bellow this are not currently displayed.
// This allows using only part of the terminal.
static int output_row;
// Current cursor row/column position in the terminal.
static unsigned term_posx, term_posy, term_color, term_cursor;
// Current terminal sizes
static unsigned term_sx, term_sy;
// Current emulated video sizes and cursor position
static unsigned vid_sx, vid_sy, vid_posx, vid_posy, vid_cursor, vid_color;
// Signals that the terminal size needs updating
static volatile int term_needs_update;
// Terminal FD, allows video output even with redirection.
static FILE *tty_file;
// Video is already initialized
static int video_initialized;

// Forward
static void term_goto_xy(unsigned x, unsigned y);

// Signal handler - terminal size changed
// TODO: not used yet.
#if 0
static void sigwinch_handler(int sig)
{
    //    term_needs_upodate = 1;
}
#endif

static void term_get_size(void)
{
    struct winsize ws;
    if(ioctl(fileno(tty_file), TIOCGWINSZ, &ws) != -1)
    {
        // TODO: perhaps restrict to "known" values
        term_sx = ws.ws_col;
        term_sy = ws.ws_row;
        if(term_sx < 40)
            term_sx = 40;
        else if(term_sx > 240)
            term_sx = 240;
        if(term_sy < 25)
            term_sy = 25;
        else if(term_sy > 64)
            term_sy = 64;
    }
    else
    {
        term_sx = 80;
        term_sy = 25;
    }
}

// Update posx/posy in BIOS memory
static void update_posxy(void)
{
    memory[0x450] = vid_posx;
    memory[0x451] = vid_posy;
    memory[0x462] = 0; // current page = 0
}

// Clears the terminal data - not the actual terminal screen
static void clear_terminal(void)
{
    // Clear screen terminal:
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 256; x++)
            term_screen[y][x] = 0x0720;
    output_row = -1;
    term_posx = 0;
    term_posy = 0;
    // Get current terminal size
    term_get_size();
    putc('\r', tty_file); // Go to column 0
}

static void clear_screen(void)
{
    debug(debug_video, "clear video screen\n");
    // Clear video screen
    uint16_t *vm = (uint16_t *)(memory + 0xB8000);
    for(int i = 0; i < 16384; i++)
        vm[i] = 0x0720;
    vid_posx = 0;
    vid_posy = 0;
    vid_color = 0x07;
    vid_cursor = 1;
    // TODO: support other video modes
    vid_sx = 80;
    vid_sy = 25;
    memory[0x449] = 0x03; // video mode
    memory[0x44A] = vid_sx;
    memory[0x484] = vid_sy - 1;
    update_posxy();
}

static unsigned get_last_used_row(void)
{
    unsigned max = 0;
    for(unsigned y = 0; y < vid_sy; y++)
        for(unsigned x = 0; x < vid_sx; x++)
            if(term_screen[y][x] != 0x700 && term_screen[y][x] != 0x720)
                max = y + 1;
    return max;
}

static void exit_video(void)
{
    vid_cursor = 1;
    check_screen();
    unsigned max = get_last_used_row();
    term_goto_xy(0, max);
    fputs("\x1b[m", tty_file);
    fclose(tty_file);
    debug(debug_video, "exit video - row %d\n", max);
}

static void init_video(void)
{
    debug(debug_video, "starting video emulation.\n");
    int tty_fd = open("/dev/tty", O_NOCTTY | O_WRONLY);
    if(tty_fd < 0)
    {
        print_error("error at open TTY, %s\n", strerror(errno));
        exit(1);
    }
    tty_file = fdopen(tty_fd,"w");
    atexit(exit_video);
    video_initialized = 1;

    // Set video mode
    clear_screen();
    clear_terminal();
    term_needs_update = 0;
    term_cursor = 1;
    term_color = 0x07;
}

int video_active(void)
{
    return video_initialized;
}

static void set_color(uint8_t c)
{
    if(term_color != c)
    {
        static char cn[8] = "04261537";
        fprintf(tty_file, "\x1b[%c;3%c;4%cm", (c & 0x08) ? '1' : '0', cn[c & 7],
               cn[(c >> 4) & 7]);
        term_color = c;
    }
}

// Writes a DOS character to the current terminal position
static void put_vc(uint8_t c)
{
    static const uint16_t trans[0x100] = {
        0x0020, 0x263a, 0x263b, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022, 0x25d8,
        0x25cb, 0x25d9, 0x2642, 0x2640, 0x266a, 0x266b, 0x263c, 0x25b6, 0x25c0,
        0x2195, 0x203c, 0x00b6, 0x00a7, 0x25ac, 0x21a8, 0x2191, 0x2193, 0x2192,
        0x2190, 0x221f, 0x2194, 0x25b2, 0x25bc, 0x0020, 0x0021, 0x0022, 0x0023,
        0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002a, 0x002b, 0x002c,
        0x002d, 0x002e, 0x002f, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035,
        0x0036, 0x0037, 0x0038, 0x0039, 0x003a, 0x003b, 0x003c, 0x003d, 0x003e,
        0x003f, 0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
        0x0048, 0x0049, 0x004a, 0x004b, 0x004c, 0x004d, 0x004e, 0x004f, 0x0050,
        0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059,
        0x005a, 0x005b, 0x005c, 0x005d, 0x005e, 0x005f, 0x0060, 0x0061, 0x0062,
        0x0063, 0x0064, 0x0065, 0x0066, 0x0067, 0x0068, 0x0069, 0x006a, 0x006b,
        0x006c, 0x006d, 0x006e, 0x006f, 0x0070, 0x0071, 0x0072, 0x0073, 0x0074,
        0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007a, 0x007b, 0x007c, 0x007d,
        0x007e, 0x2302, 0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5,
        0x00e7, 0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5,
        0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9, 0x00ff,
        0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192, 0x00e1, 0x00ed,
        0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba, 0x00bf, 0x2310, 0x00ac,
        0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb, 0x2591, 0x2592, 0x2593, 0x2502,
        0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255d,
        0x255c, 0x255b, 0x2510, 0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c,
        0x255e, 0x255f, 0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c,
        0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
        0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580, 0x03b1,
        0x03b2, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4, 0x03a6, 0x0398,
        0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229, 0x2261, 0x00b1, 0x2265,
        0x2264, 0x2320, 0x2321, 0x00f7, 0x2248, 0x00b0, 0x2219, 0x00b7, 0x221a,
        0x207f, 0x00b2, 0x25a0, 0x00a0};
    uint16_t uc = trans[c];
    if(uc < 128)
        putc(uc, tty_file);
    else if(uc < 0x800)
    {
        putc(0xC0 | (uc >> 6), tty_file);
        putc(0x80 | (uc & 0x3F), tty_file);
    }
    else
    {
        putc(0xE0 | (uc >> 12), tty_file);
        putc(0x80 | ((uc >> 6) & 0x3F), tty_file);
        putc(0x80 | (uc & 0x3F), tty_file);
    }
}

// Move terminal cursor to the position
static void term_goto_xy(unsigned x, unsigned y)
{
    if(term_posy < y && (int)term_posy < output_row)
    {
        int inc = (int)y < output_row ? y - term_posy : output_row - term_posy;
        fprintf(tty_file, "\x1b[%dB", inc);
        term_posy += inc;
    }
    if(term_posy < y)
    {
        putc('\r', tty_file);
        // TODO: Draw new line with background color from video screen
        for(unsigned i = term_posy; i < y; i++)
            putc('\n', tty_file);
        term_posx = 0;
        term_posy = y;
    }
    if(term_posy > y)
    {
        fprintf(tty_file, "\x1b[%dA", term_posy - y);
        term_posy = y;
    }
    if(x == 0 && term_posx != 0)
    {
        putc('\r', tty_file);
        term_posx = 0;
    }
    if(term_posx < x)
    {
        fprintf(tty_file, "\x1b[%dC", x - term_posx);
        term_posx = x;
    }
    if(term_posx > x)
    {
        fprintf(tty_file, "\x1b[%dD", term_posx - x);
        term_posx = x;
    }
}

// Outputs a character with the given attributes at the given position
static void put_vc_xy(uint8_t vc, uint8_t color, unsigned x, unsigned y)
{
    set_color(color);
    term_goto_xy(x, y);

    put_vc(vc);
    term_posx++;
    if(term_posx >= term_sx)
    {
        term_posx = 0;
        term_posy++;
    }

    if(output_row < (int)term_posy)
        output_row = term_posy;
}

// Compares current screen with memory data
void check_screen(void)
{
    // Exit if not in video mode
    if(!video_initialized)
        return;

    uint16_t *vm = (uint16_t *)(memory + 0xB8000);
    unsigned max = output_row + 1;
    for(unsigned y = output_row + 1; y < vid_sy; y++)
        for(unsigned x = 0; x < vid_sx; x++)
            if(vm[x + y * vid_sx] != term_screen[y][x])
                max = y + 1;

    for(unsigned y = 0; y < max; y++)
        for(unsigned x = 0; x < vid_sx; x++)
        {
            int16_t vc = vm[x + y * vid_sx];
            if(vc != term_screen[y][x])
            {
                // Output character
                term_screen[y][x] = vc;
                put_vc_xy(vc & 0xFF, vc >> 8, x, y);
            }
        }
    if(term_cursor != vid_cursor)
    {
        term_cursor = vid_cursor;
        if(vid_cursor)
            fputs("\x1b[?25h", tty_file);
        else
            fputs("\x1b[?25l", tty_file);
    }
    if(term_cursor)
        // Move cursor
        term_goto_xy(vid_posx, vid_posy);
    fflush(tty_file);
}

static void vid_scroll_up(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1,
                          int n)
{
    debug(debug_video, "scroll up %d: (%d, %d) - (%d, %d)\n", n,
          x0, y0, x1, y1);

    // Check parameters
    if(x1 >= vid_sx)
        x1 = vid_sx - 1;
    if(y1 >= vid_sy)
        y1 = vid_sy - 1;
    if(y0 > y1 || x0 > x1)
        return;
    if(n > y1 - y0 + 1 || !n)
        n = y1 + 1 - y0;

    // Scroll TERMINAL if we are scrolling (almost) the entire screen
    if(y0 == 0 && y1 >= vid_sy - 2 && x0 < 2 && x1 >= vid_sx - 2)
    {
        // Update screen before
        check_screen();
        int m = n > output_row + 1 ? output_row + 1 : n;
        if(term_posy < m)
            term_goto_xy(0, m);
        output_row -= m;
        term_posy -= m;
        for(unsigned y = 0; y + m < term_sy; y++)
            for(unsigned x = 0; x < term_sx; x++)
                term_screen[y][x] = term_screen[y + m][x];
        for(unsigned y = term_sy - m; y < term_sy; y++)
            for(unsigned x = 0; x < term_sx; x++)
                term_screen[y][x] = 0x0720;
    }

    // Scroll VIDEO
    uint16_t *vm = (uint16_t *)(memory + 0xB8000);
    for(unsigned y = y0; y + n <= y1; y++)
        for(unsigned x = x0; x <= x1; x++)
            vm[x + y * vid_sx] = vm[x + y * vid_sx + n * vid_sx];
    // Set last rows
    for(unsigned y = y1 - (n - 1); y <= y1; y++)
        for(unsigned x = x0; x <= x1; x++)
            vm[x + y * vid_sx] = (vid_color << 8) + 0x20;
}

static void vid_scroll_dwn(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1,
                           unsigned n)
{
    debug(debug_video, "scroll up %d: (%d, %d) - (%d, %d)\n", n,
          x0, y0, x1, y1);

    // Check parameters
    if(x1 >= vid_sx)
        x1 = vid_sx - 1;
    if(y1 >= vid_sy)
        y1 = vid_sy - 1;
    if(y0 > y1 || x0 > x1)
        return;
    if(n > y1 - y0 + 1 || !n)
        n = y1 + 1 - y0;

    // TODO: try to scroll TERMINAL

    // Scroll VIDEO
    uint16_t *vm = (uint16_t *)(memory + 0xB8000);
    for(unsigned y = y1; y >= y0 + n; y--)
        for(unsigned x = x0; x <= x1; x++)
            vm[x + y * vid_sx] = vm[x + y * vid_sx - n * vid_sx];
    // Set first rows
    for(unsigned y = y0; y < y0 + n; y++)
        for(unsigned x = x0; x <= x1; x++)
            vm[x + y * vid_sx] = (vid_color << 8) + 0x20;
}

static void set_xy(unsigned x, unsigned y, uint16_t c, uint16_t mask)
{
    uint16_t *vm = (uint16_t *)(memory + 0xB8000);
    vm[x + y * vid_sx] = (vm[x + y * vid_sx] & mask) | c;
}

static uint16_t get_xy(unsigned x, unsigned y)
{
    uint16_t *vm = (uint16_t *)(memory + 0xB8000);
    return vm[x + y * vid_sx];
}

static void video_putchar(uint8_t ch, uint16_t at)
{
    if(ch == 0x0A)
    {
        vid_posy++;
        while(vid_posy >= vid_sy)
        {
            vid_posy = vid_sy - 1;
            vid_scroll_up(0, 0, vid_sx - 1, vid_sy - 1, 1);
        }
    }
    else if(ch == 0x0D)
        vid_posx = 0;
    else if(ch == 0x08)
    {
        if(vid_posx > 0)
            vid_posx --;
    }
    else
    {
        if(at & 0xFFFF)
            set_xy(vid_posx, vid_posy, ch, 0xFF00);
        else
            set_xy(vid_posx, vid_posy, ch + (at << 8), 0);
        vid_posx++;
        if(vid_posx >= vid_sx)
        {
            vid_posx = 0;
            vid_posy++;
            while(vid_posy >= vid_sy)
            {
                vid_posy = vid_sy - 1;
                vid_scroll_up(0, 0, vid_sx - 1, vid_sy - 1, 1);
            }
        }
    }
    update_posxy();
}

void video_putch(char ch)
{
    debug(debug_video, "putchar %02x at (%d,%d)\n", ch & 0xFF, vid_posx, vid_posy);
    video_putchar(ch, 0xFF00);
}

// VIDEO int
void int10()
{
    debug(debug_int, "V-10%04X: BX=%04X\n", cpuGetAX(), cpuGetBX());
    if(!video_initialized)
        init_video();
    unsigned ax = cpuGetAX();
    switch(ax >> 8)
    {
    case 0x00: // SET VIDEO MODE
        if((ax & 0xFF) > 3)
            debug(debug_video, "-> SET GRAPHICS MODE %x<-\n", ax & 0xFF);
        else
            clear_screen();
        break;
    case 0x01:                              // SET CURSOR SHAPE
        if((cpuGetCX() & 0x6000) == 0x2000) // Hide cursor
            vid_cursor = 0;
        else
            vid_cursor = 1;
        break;
    case 0x02: // SET CURSOR POS
        vid_posx = cpuGetDX() & 0xFF;
        vid_posy = cpuGetDX() >> 8;
        if(vid_posx >= vid_sx)
            vid_posx = vid_sx - 1;
        if(vid_posy >= vid_sy)
            vid_posy = vid_sy - 1;
        update_posxy();
        break;
    case 0x03: // GET CURSOR POS
        cpuSetDX(vid_posx + (vid_posy << 8));
        break;
    case 0x05: // SELECT DISPLAY PAGE
        if((ax & 0xFF) != 0)
            debug(debug_video, "WARN: Select display page != 0!\n");
        break;
    case 0x06: // SCROLL UP WINDOW
    {
        uint16_t cx = cpuGetCX(), dx = cpuGetDX();
        vid_color = cpuGetBX() >> 8;
        vid_scroll_up(cx, cx >> 8, dx, dx >> 8, ax & 0xFF);
        break;
    }
    case 0x07: // SCROLL DOWN WINDOW
    {
        uint16_t cx = cpuGetCX(), dx = cpuGetDX();
        vid_color = cpuGetBX() >> 8;
        vid_scroll_dwn(cx, cx >> 8, dx, dx >> 8, ax & 0xFF);
        break;
    }
    case 0x08: // READ CHAR AT CURSOR
        cpuSetAX(get_xy(vid_posx, vid_posy));
        break;
    case 0x09: // WRITE CHAR AT CURSOR
    {
        uint16_t px = vid_posx;
        uint16_t py = vid_posy;
        vid_color = cpuGetBX() & 0xFF;
        for(int i = cpuGetCX(); i>0; i--)
        {
            set_xy(px, py, (ax & 0xFF) + (vid_color << 8), 0);
            px ++;
            if(px >= vid_sx)
            {
                px = 0;
                py ++;
                if(py >= vid_sy)
                    py = 0;
            }
        }
        break;
    }
    case 0x0E: // TELETYPE OUTPUT
        video_putch(ax);
        break;
    case 0x0F: // GET CURRENT VIDEO MODE
        cpuSetAX((vid_sx << 8) | 0x0003); // 80x25 mode
        cpuSetBX(0);
        break;
    case 0x10:
        if(ax == 0x1002) // TODO: Set pallete registers - ignore
            break;
        debug(debug_int, "UNHANDLED INT 10, AX=%04x\n", ax);
        break;
    case 0x11:
        if(ax == 0x1130)
        {
            cpuSetDX((vid_sy - 1) & 0xFF);
            cpuSetCX(0x0008);
        }
        break;
    case 0x12: // GET EGA INFO
        cpuSetBX(0x0003);
        cpuSetCX(0x0000);
        cpuSetAX(0);
        break;
    case 0x13: // WRITE STRING
        {
            vid_posx = cpuGetDX() & 0xFF;
            vid_posy = cpuGetDX() >> 8;
            if(vid_posx >= vid_sx)
                vid_posx = vid_sx - 1;
            if(vid_posy >= vid_sy)
                vid_posy = vid_sy - 1;
            int save_posx = vid_posx;
            int save_posy = vid_posy;
            int addr = cpuGetAddrES(cpuGetBP());
            int cnt = cpuGetCX();
            if(ax&2)
            {
                while(cnt && addr < 0xFFFFF)
                {
                    video_putchar(memory[addr], memory[addr+1]);
                    addr += 2;
                    cnt --;
                }
            }
            else
            {
                uint8_t at = cpuGetBX() >> 8;
                while(cnt && addr <= 0xFFFFF)
                {
                    video_putchar(memory[addr], at);
                    addr ++;
                    cnt --;
                }
            }
            if(!(ax & 1))
            {
                vid_posx = save_posx;
                vid_posy = save_posy;
            }
            update_posxy();
        }
        break;
    case 0x1A: // GET/SET DISPLAY COMBINATION CODE
        cpuSetAX(0x001A);
        cpuSetBX(0x0008); // VGA + analog color display
        break;
    case 0x1B: // STATE INFO
        if(cpuGetBX() == 0x0000)
        {
            int addr = cpuGetAddrES(cpuGetDI());
            if(addr < 0xFFF00)
            {
                // Store state information
                memset(memory + addr, 0, 64);
                memory[addr+0] = 0x00;
                memory[addr+1] = 0x01;
                memory[addr+2] = 0x00;
                memory[addr+3] = 0xC0; // static-func table at C000:0000
                memory[addr+4] = 0x03; // Video mode
                memory[addr+5] = vid_sx;
                memory[addr+6] = vid_sx >> 8;
                memory[addr+11] = vid_posx; // page 0
                memory[addr+12] = vid_posy;
                memory[addr+27] = vid_cursor * 6; // cursor start scanline
                memory[addr+28] = vid_cursor * 7; // cursor end scanline
                memory[addr+29] = 0; // current page
                memory[addr+30] = 0xD4;
                memory[addr+31] = 0x03; // CRTC port: 03D4
                memory[addr+34] = vid_sy;
                memory[addr+35] = 0x10;
                memory[addr+35] = 0x00; // bytes/char: 0010
                memory[addr+39] = 0x10;
                memory[addr+40] = 0x00; // # of colors: 0010
                memory[addr+42] = 2; // # of scan-lines - get from vid_sy
                memory[addr+49] = 3; // 256k memory
                cpuSetAX(0x1B1B);
            }
        }
        break;
    case 0xEF: // TEST MSHERC.COM DISPLAY TYPE
        // Ignored
        break;
    default:
        debug(debug_int, "UNHANDLED INT 10, AX=%04x\n", ax);
    }
}
