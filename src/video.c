#include "video.h"
#include "codepage.h"
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
static unsigned vid_font_lines, vid_no_blank;
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
    debug(debug_video, "clear terminal shadow\n");
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

static void set_text_mode(int clear)
{
    debug(debug_video, "set text mode%s\n", clear ? " and clear" : "");
    // Clear video screen
    if(clear)
    {
        uint16_t *vm = (uint16_t *)(memory + 0xB8000);
        for(int i = 0; i < 16384; i++)
            vm[i] = 0x0720;
    }
    vid_posx = 0;
    vid_posy = 0;
    vid_color = 0x07;
    vid_cursor = 1;
    // TODO: support other video modes
    vid_sx = 80;
    vid_sy = 25;
    vid_font_lines = 16;
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
    tty_file = fdopen(tty_fd, "w");
    atexit(exit_video);
    video_initialized = 1;

    // Fill the functionality table
    memory[0xC0100] = 0x08; // Only mode 3 supported
    memory[0xC0101] = 0x00;
    memory[0xC0102] = 0x00;
    memory[0xC0107] = 0x07; // Support 300, 350 and 400 scanlines
    memory[0xC0108] = 0x00; // Active character blocks?
    memory[0xC0109] = 0x00; // MAximum character blocks?
    memory[0xC0108] = 0xFF; // Support functions

    // Set video mode
    set_text_mode(1);
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
    uint16_t uc = get_unicode(c);
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
        // Set background color to black, as some terminals insert lines with
        // the current background color.
        set_color(term_color & 0x0F);
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
    term_goto_xy(x, y);
    set_color(color);

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

static void vid_scroll_up(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, int n)
{
    debug(debug_video, "scroll up %d: (%d, %d) - (%d, %d)\n", n, x0, y0, x1, y1);

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

static void vid_scroll_dwn(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, unsigned n)
{
    debug(debug_video, "scroll down %d: (%d, %d) - (%d, %d)\n", n, x0, y0, x1, y1);

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
            vid_posx--;
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
    debug(debug_video, "V-10%04X: BX=%04X\n", cpuGetAX(), cpuGetBX());
    if(!video_initialized)
        init_video();
    unsigned ax = cpuGetAX();
    switch(ax >> 8)
    {
    case 0x00: // SET VIDEO MODE
        if((ax & 0x7F) > 3)
            debug(debug_video, "-> SET GRAPHICS MODE %x<-\n", ax & 0xFF);
        else
        {
            set_text_mode((ax & 0x80) == 0);
            vid_no_blank = ax & 0x80;
        }
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
        for(int i = cpuGetCX(); i > 0; i--)
        {
            set_xy(px, py, (ax & 0xFF) + (vid_color << 8), 0);
            px++;
            if(px >= vid_sx)
            {
                px = 0;
                py++;
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
        cpuSetAX((vid_sx << 8) | 0x0003 | vid_no_blank); // 80x25 mode
        cpuSetBX(0);
        break;
    case 0x10:
        if(ax == 0x1002) // TODO: Set pallete registers - ignore
            break;
        else if(ax == 0x1003) // TODO: Set blinking state
            break;
        debug(debug_video, "UNHANDLED INT 10, AX=%04x\n", ax);
        break;
    case 0x11:
        if(ax == 0x1130)
        {
            cpuSetDX((vid_sy - 1) & 0xFF);
            cpuSetCX(vid_font_lines);
        }
        else if(ax == 0x1104 || ax == 0x1111 || ax == 0x1114)
        {
            // Clear end-of-screen
            unsigned max = get_last_used_row();
            debug(debug_video, "set 25 lines mode %d\n", max);
            if(max > 25)
            {
                term_goto_xy(0, 24);
                set_color(0x07);
                fputs("\x1b[J", tty_file);
                for(int y = 25; y < 64; y++)
                    for(int x = 0; x < 256; x++)
                        term_screen[y][x] = 0x0720;
                if(output_row > 24)
                    output_row = 24;
            }
            // Set 8x16 font - 80x25 mode:
            vid_sy = 25;
            vid_font_lines = 16;
            memory[0x484] = vid_sy - 1;
        }
        else if(ax == 0x1102 || ax == 0x1112)
        {
            // Set 8x8 font - 80x43 or 80x50 mode:
            debug(debug_video, "set 43/50 lines mode\n");
            // Hack - QBASIC.EXE assumes that the mode is always 50 lines on VGA,
            // and *sets* the height into the BIOS area!
            if(memory[0x484] > 42)
                vid_sy = 50;
            else
                vid_sy = 43;
            vid_font_lines = 8;
            memory[0x484] = vid_sy - 1;
        }
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
    }
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
        if(ax & 2)
        {
            while(cnt && addr < 0xFFFFF)
            {
                video_putchar(memory[addr], memory[addr + 1]);
                addr += 2;
                cnt--;
            }
        }
        else
        {
            uint8_t at = cpuGetBX() >> 8;
            while(cnt && addr <= 0xFFFFF)
            {
                video_putchar(memory[addr], at);
                addr++;
                cnt--;
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
                memory[addr + 0] = 0x00;
                memory[addr + 1] = 0x01;
                memory[addr + 2] = 0x00;
                memory[addr + 3] = 0xC0; // static-func table at C000:0000
                memory[addr + 4] = 0x03; // Video mode
                memory[addr + 5] = vid_sx;
                memory[addr + 6] = vid_sx >> 8;
                memory[addr + 11] = vid_posx; // page 0
                memory[addr + 12] = vid_posy;
                memory[addr + 27] = vid_cursor * 6; // cursor start scanline
                memory[addr + 28] = vid_cursor * 7; // cursor end scanline
                memory[addr + 29] = 0;              // current page
                memory[addr + 30] = 0xD4;
                memory[addr + 31] = 0x03; // CRTC port: 03D4
                memory[addr + 34] = vid_sy;
                memory[addr + 35] = vid_font_lines;
                memory[addr + 36] = 0x00; // font lines: 0010
                memory[addr + 39] = 0x10;
                memory[addr + 40] = 0x00; // # of colors: 0010
                memory[addr + 42] = 2;    // # of scan-lines - get from vid_sy
                memory[addr + 49] = 3;    // 256k memory
                cpuSetAX(0x1B1B);
            }
        }
        break;
    case 0xEF: // TEST MSHERC.COM DISPLAY TYPE
        // Ignored
        break;
    default:
        debug(debug_video, "UNHANDLED INT 10, AX=%04x\n", ax);
    }
}

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
            debug(debug_video, "CRTC port write [%02x] <- %02x\n", crtc_port, value);
    }
    else
        crtc_port = value;
}

int video_get_col(void)
{
    return vid_posx;
}
