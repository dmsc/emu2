#include "keyb.h"
#include "codepage.h"
#include "dbg.h"
#include "emu.h"
#include "os.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#define MAX_KEYB_CALLS 10

static int term_raw = 0;
static int tty_fd = -1;
static int queued_key = -1;
static int waiting_key = 0;
static int mod_state = 0;
static int throttle_calls = 0;

// Copy mod-state to BIOS memory area
static void update_bios_state(void)
{
    // TODO: should update all keyboard buffer variables
    memory[0x417] = mod_state;
}

// Update keyboard buffer pointer
static void keyb_read_buffer(void)
{
    int ptr = (memory[0x41A] - 0x1E) & 0x1F;
    memory[0x41A] = 0x1E + ((ptr + 2) & 0x1F);
}

// Table of scan codes for keys + modifiers:
static uint8_t special_codes[23][4] = {
    {0x3B, 0x54, 0x5E, 0x68}, // F1
    {0x3C, 0x55, 0x5F, 0x69}, // F2
    {0x3D, 0x56, 0x60, 0x6A}, // F3
    {0x3E, 0x57, 0x61, 0x6B}, // F4
    {0x3F, 0x58, 0x62, 0x6C}, // F5
    {0x40, 0x59, 0x63, 0x6D}, // F6
    {0x41, 0x5A, 0x64, 0x6E}, // F7
    {0x42, 0x5B, 0x65, 0x6F}, // F8
    {0x43, 0x5C, 0x66, 0x70}, // F9
    {0x44, 0x5D, 0x67, 0x71}, // F10
    {0x85, 0x87, 0x89, 0x8B}, // F11
    {0x86, 0x88, 0x8A, 0x8C}, // F12
    {0x48, 0x48, 0x8D, 0x98}, // Up
    {0x50, 0x50, 0x91, 0xA0}, // Down
    {0x4B, 0x4B, 0x73, 0x9B}, // Left
    {0x4D, 0x4D, 0x74, 0x9D}, // Right
    {0x49, 0x49, 0x84, 0x99}, // Pg-Up
    {0x51, 0x51, 0x76, 0xA1}, // Pg-Down
    {0x57, 0x57, 0x77, 0x97}, // Home
    {0x4F, 0x4F, 0x75, 0x9F}, // End
    {0x52, 0x52, 0x92, 0xA2}, // Ins
    {0x53, 0x53, 0x93, 0xA3}, // Del
    {0x4C, 0x4C, 0x8F, 0x00}, // KP-5
};

enum special_keys
{
    KEY_FN1 = 0,
    KEY_UP = 12,
    KEY_DOWN = 13,
    KEY_LEFT = 14,
    KEY_RIGHT = 15,
    KEY_PGUP = 16,
    KEY_PGDN = 17,
    KEY_HOME = 18,
    KEY_END = 19,
    KEY_INS = 20,
    KEY_DEL = 21,
    KEY_KP5 = 22
};

#define KEY_FN(a) (KEY_FN1 + (a)-1)

enum mod_keys
{
    MOD_SHIFT = 1,
    MOD_RSHIFT = 2,
    MOD_CTRL = 4,
    MOD_ALT = 8
};

static int get_special_code(int key)
{
    if(mod_state & MOD_ALT)        return special_codes[key][3] << 8;
    else if(mod_state & MOD_CTRL)  return special_codes[key][2] << 8;
    else if(mod_state & MOD_SHIFT) return special_codes[key][1] << 8;
    else                           return special_codes[key][0] << 8;
}

// Convert ASCII key to a scan-code + key
static int get_scancode(int i)
{
    if(i >= 'a' && i <= 'z')
        i = i - 'a' + 'A';
    switch(i)
    {
    case 0x1b: return 0x0100;
    case '!':
    case '1':  return 0x0200;
    case '@':
    case '2':  return 0x0300;
    case '#':
    case '3':  return 0x0400;
    case '$':
    case '4':  return 0x0500;
    case '%':
    case '5':  return 0x0600;
    case '^':
    case '6':  return 0x0700;
    case '&':
    case '7':  return 0x0800;
    case '*':
    case '8':  return 0x0900;
    case '(':
    case '9':  return 0x0A00;
    case ')':
    case '0':  return 0x0B00;
    case '_':
    case '-':  return 0x0C00;
    case '+':
    case '=':  return 0x0D00;
    case 0x7F:
    case 0x08: return 0x0E00;
    case 0x09: return 0x0F00;
    case 'Q':  return 0x1000;
    case 'W':  return 0x1100;
    case 'E':  return 0x1200;
    case 'R':  return 0x1300;
    case 'T':  return 0x1400;
    case 'Y':  return 0x1500;
    case 'U':  return 0x1600;
    case 'I':  return 0x1700;
    case 'O':  return 0x1800;
    case 'P':  return 0x1900;
    case '{':
    case '[':  return 0x1A00;
    case '}':
    case ']':  return 0x1B00;
    case 0x0D: return 0x1C00;
 // case 0x00: return 0x1D00; // CTRL
    case 'A':  return 0x1E00;
    case 'S':  return 0x1F00;
    case 'D':  return 0x2000;
    case 'F':  return 0x2100;
    case 'G':  return 0x2200;
    case 'H':  return 0x2300;
    case 'J':  return 0x2400;
    case 'K':  return 0x2500;
    case 'L':  return 0x2600;
    case ':':
    case ';':  return 0x2700;
    case '\'':
    case '"':  return 0x2800;
    case '`':
    case '~':  return 0x2900;
 // case 0x00: return 0x2A00; // LSHIFT
    case '\\':
    case '|':  return 0x2B00;
    case 'Z':  return 0x2C00;
    case 'X':  return 0x2D00;
    case 'C':  return 0x2E00;
    case 'V':  return 0x2F00;
    case 'B':  return 0x3000;
    case 'N':  return 0x3100;
    case 'M':  return 0x3200;
    case ',':
    case '<':  return 0x3300;
    case '.':
    case '>':  return 0x3400;
    case '/':
    case '?':  return 0x3500;
 // case 0x00: return 0x3600; // RSHIFT
 // case 0x00: return 0x3700; // ???
 // case 0x00: return 0x3800; // ALT
    case ' ':  return 0x3900;
    default:   return i & 0xFF00;
    }
}

// Adds a scan-code to the given key
static int add_scancode(int i)
{
    // Exclude ESC, ENTER and TAB.
    if(i < 0x20 && i != 0x1B && i != 0x0D && i != 0x09)
    {
        // CTRL+KEY
        mod_state |= MOD_CTRL;
        int orig = i;
        if(i == 0x1C)      i = '\\';
        else if(i == 0x1D) i = ']';
        else if(i == 0x1E) i = '6';
        else if(i == 0x1F) i = '-';
        else if(i == 0x08) orig = 0x7F;
        else               i = i + 0x20;

        return orig | get_scancode(i);
    }
    else if((i > 0x20 && i < 0x27) || (i > 0x27 && i < 0x2C) || (i == 0x3A) ||
            (i == 0x3C) || (i > 0x3D && i < 0x5B) || (i > 0x5D && i < 0x60) ||
            (i > 0x7A && i < 0x7F))
        mod_state |= MOD_SHIFT;
    // Fixes BackSpace
    if(i == 0x7F)
        i = 0x08;
    if(0 == (i & 0xFF00))
        return i | get_scancode(i);
    else
        return i;
}

// Convert key-code with ALT to scan-code
static int alt_char(int i)
{
    mod_state = MOD_ALT;
    return add_scancode(i) & 0xFF00; // No ASCII code on ALT+char
}

static int get_esc_secuence(void)
{
    // Read and process ESC sequences:
    // ESC                              ESC
    // ESC <letter>                     ALT+letter
    // ESC <number>                     ALT+number
    // ESC [ <modifiers> <letter>       Function Keys
    mod_state = 0;
    char ch = '\xFF';
    if(read(tty_fd, &ch, 1) == 0)
        return 0x011B; // ESC
    if(ch != '[' && ch != 'O')
        return alt_char(ch);

    int n1 = 0, n2 = 0;
    while(1)
    {
        char cn = '\xFF';
        if(read(tty_fd, &cn, 1) == 0)
        {
            if(n1 == 0 && n2 == 0)
                return alt_char(ch); // it is an ALT+'[' or ALT+'O'
            return 0;                // ERROR!
        }
        if(cn >= '0' && cn <= '9')
            n2 = n2 * 10 + (cn - '0');
        else if(cn == ';')
        {
            n1 = n2;
            n2 = 0;
        }
        else if(cn == '~')
        {
            if(n1 == 0 && n2 == 0)
                return 0; // ERROR!
            if(n1 == 0)
            {
                n1 = n2;
                n2 = 1;
            }
            n2--;
            if(n2 & 1) mod_state |= MOD_SHIFT;
            if(n2 & 2) mod_state |= MOD_ALT;
            if(n2 & 4) mod_state |= MOD_CTRL;
            switch(n1)
            {
            case 1:  return get_special_code(KEY_HOME); // old xterm
            case 2:  return get_special_code(KEY_INS);
            case 3:  return get_special_code(KEY_DEL);
            case 4:  return get_special_code(KEY_END); // old xterm
            case 5:  return get_special_code(KEY_PGUP);
            case 6:  return get_special_code(KEY_PGDN);
            case 11: return get_special_code(KEY_FN(1)); // F1 (old)
            case 12: return get_special_code(KEY_FN(2)); // F2 (old)
            case 13: return get_special_code(KEY_FN(3)); // F3 (old)
            case 14: return get_special_code(KEY_FN(4)); // F4 (old)
            case 15: return get_special_code(KEY_FN(5));
            case 17: return get_special_code(KEY_FN(6));
            case 18: return get_special_code(KEY_FN(7));
            case 19: return get_special_code(KEY_FN(8));
            case 20: return get_special_code(KEY_FN(9));
            case 21: return get_special_code(KEY_FN(10));
            case 23: return get_special_code(KEY_FN(11));
            case 24: return get_special_code(KEY_FN(12));
            default: return 0; // ERROR!
            }
        }
        else
        {
            if(n2)
                n2--;
            if(n2 & 1) mod_state |= MOD_SHIFT;
            if(n2 & 2) mod_state |= MOD_ALT;
            if(n2 & 4) mod_state |= MOD_CTRL;
            switch(cn)
            {
            case 'A': return get_special_code(KEY_UP);
            case 'B': return get_special_code(KEY_DOWN);
            case 'C': return get_special_code(KEY_RIGHT);
            case 'D': return get_special_code(KEY_LEFT);
            case 'E': return get_special_code(KEY_KP5);
            case 'F': return get_special_code(KEY_END);
            case 'H': return get_special_code(KEY_HOME);
            case 'I': return 0x0F09; // TAB
            case 'P': return get_special_code(KEY_FN(1)); // F1
            case 'Q': return get_special_code(KEY_FN(2)); // F2
            case 'R': return get_special_code(KEY_FN(3)); // F3
            case 'S': return get_special_code(KEY_FN(4)); // F4
            case 'Z': mod_state |= MOD_SHIFT; return 0x0F00; // shift-TAB
            default:  return 0; // ERROR!
            }
        }
    }
}

static int read_key(void)
{
    char ch = '\xFF';
    // Reads first key code
    if(read(tty_fd, &ch, 1) == 0)
        return -1; // No data

    // ESC + keys, terminal codes
    if(ch == 0x1B)
        return get_esc_secuence();

    mod_state = 0;
    // Normal key
    if((ch & 0xFF) < 0x80)
        return add_scancode(ch);

    // Unicode character, read rest of codes
    if((ch & 0xE0) == 0xC0)
    {
        char ch1 = '\xFF';
        if(read(tty_fd, &ch1, 1) == 0 || (ch1 & 0xC0) != 0x80)
            return 0; // INVALID UTF-8
        return get_dos_char(((ch & 0x1F) << 6) | (ch1 & 0x3F));
    }
    else if((ch & 0xF0) == 0xE0)
    {
        char ch1 = '\xFF', ch2 = '\xFF';
        if(read(tty_fd, &ch1, 1) == 0 || (ch1 & 0xC0) != 0x80 ||
           read(tty_fd, &ch2, 1) == 0 || (ch2 & 0xC0) != 0x80)
            return -1; // INVALID UTF-8
        return get_dos_char(((ch & 0x0F) << 12) | ((ch1 & 0x3F) << 6) | (ch2 & 0x3F));
    }
    else if((ch & 0xF8) == 0xF0)
    {
        char ch1 = '\xFF', ch2 = '\xFF', ch3 = '\xFF';
        if(read(tty_fd, &ch1, 1) == 0 || (ch1 & 0xC0) != 0x80 ||
           read(tty_fd, &ch2, 1) == 0 || (ch2 & 0xC0) != 0x80 ||
           read(tty_fd, &ch3, 1) == 0 || (ch3 & 0xC0) != 0x80)
            return -1; // INVALID UTF-8
        return get_dos_char(((ch & 0x07) << 18) | ((ch1 & 0x3F) << 12) |
                            ((ch2 & 0x3F) << 6) | (ch3 & 0x3F));
    }
    else
        return 0; // INVALID UTF-8
}

static void set_raw_term(int raw)
{
    static struct termios oldattr; // Initial terminal state
    if(raw == term_raw)
        return;

    term_raw = raw;
    if(term_raw)
    {
        struct termios newattr;
        tcgetattr(tty_fd, &oldattr);
        newattr = oldattr;
#if defined(NO_CFMAKERAW)
        newattr.c_iflag &= ~(IMAXBEL|IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
        newattr.c_oflag &= ~OPOST;
        newattr.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
        newattr.c_cflag &= ~(CSIZE|PARENB);
        newattr.c_cflag |= CS8;
#else
        cfmakeraw(&newattr);
#endif
        newattr.c_cc[VMIN] = 0;
        newattr.c_cc[VTIME] = 0;
        tcsetattr(tty_fd, TCSANOW, &newattr);
    }
    else
        tcsetattr(tty_fd, TCSANOW, &oldattr);
}

static void exit_keyboard(void)
{
    set_raw_term(0);
    close(tty_fd);
}

static void init_keyboard(void)
{
    if(tty_fd < 0)
    {
        tty_fd = open("/dev/tty", O_NOCTTY | O_RDONLY);
        if(tty_fd < 0)
            print_error("error at open TTY, %s\n", strerror(errno));
        atexit(exit_keyboard);
    }
    set_raw_term(1);
}

// Disables keyboard support - will be enabled again if needed
void suspend_keyboard(void)
{
    if(tty_fd >= 0)
        set_raw_term(0);
}

void keyb_wakeup(void)
{
    throttle_calls = 0;
}

int kbhit(void)
{
    if(queued_key == -1)
    {
        init_keyboard();
        queued_key = read_key();
        if(queued_key != -1)
        {
            update_bios_state();
            cpuTriggerIRQ(1);
        }
        else
        {
            // Used to throttle the CPU on a busy-loop waiting for keyboard
            static double last_time;

            struct timeval tv;
            if(gettimeofday(&tv, NULL) != -1)
            {
                double t1 = tv.tv_usec + tv.tv_sec * 1000000.0;
                // Arbitrary limit to 4 calls each 100Hz
                if((t1 - last_time) < 10000)
                {
                    throttle_calls++;
                    if(throttle_calls > MAX_KEYB_CALLS)
                    {
                        debug(debug_int, "keyboard sleep.\n");
                        usleep(10000);
                        throttle_calls = 0;
                    }
                }
                else
                    throttle_calls = 0;
                last_time = t1;
            }
        }
    }
    return (queued_key == -1) ? 0 : queued_key;
}

int getch(int detect_brk)
{
    int ret;
    while(queued_key == -1)
    {
        if(kbhit())
            break;
        usleep(100000);
        waiting_key = 1;
        emulator_update();
        waiting_key = 0;
    }
    if(detect_brk && ((queued_key & 0xFF) == 3))
        raise(SIGINT);
    ret = queued_key;
    queued_key = -1;
    keyb_read_buffer();
    update_bios_state();
    return ret;
}

void update_keyb(void)
{
    // See if any key is available:
    if(tty_fd >= 0 && !waiting_key && queued_key == -1)
        kbhit();
}

// Keyboard controller status
static uint8_t portB_ctl = 0;
static uint8_t keyb_command = 0;

// Handle keyboard controller port reading
uint8_t keyb_read_port(unsigned port)
{
    if(queued_key == -1)
        kbhit();
    debug(debug_int, "keyboard read_port: %02X (key=%04X)\n", port, 0xFFFFU & queued_key);
    if(port == 0x60)
        return queued_key >> 8;
    else if(port == 0x61)
        return portB_ctl; // Controller B, used for speaker output
    else if(port == 0x64)
        // bit0 == 1 if there is data available.
        return (queued_key != -1) | ((keyb_command != 0) << 3);
    else
        return 0xFF;
}

// Handle keyboard controller port writes
void keyb_write_port(unsigned port, uint8_t value)
{
    debug(debug_int, "keyboard write_port: %02X <- %02X\n", port, value);
    if(port == 0x60)
    {
        // Write to keyboard data
        if(keyb_command == 0)
        {
            queued_key = value << 8;
        }
        else
        {
            // Handle keyboard commands
            if(keyb_command == 0xD1) // Write output port
            {
                if(value & 1)
                {
                    // System reset
                    debug(debug_int, "System reset via invalid keyboard I/O!\n");
                    exit(0);
                }
                keyb_command = 0;
            }
        }
        return;
    }
    else if(port == 0x61)
        // Store speaker control bits
        portB_ctl = value & 0x03;
    else if(port == 0x64)
    {
        // Commands to keyboard controller
        keyb_command = value;
        if((keyb_command & 0xF0) == 0xF0)
        {
            int bits = keyb_command & 0x0F;
            if(bits & 1)
            {
                // System reset
                debug(debug_int, "System reset via keyboard controller!\n");
                exit(0);
            }
            keyb_command = 0;
        }
    }
}

void keyb_handle_irq(void)
{
    // The BIOS should read a key pressed here and add it to the keyboard buffer.
    int ptr = (memory[0x41C] - 0x1E) & 0x1F;
    memory[0x41E + ptr] = queued_key & 0xFF;
    memory[0x41F + ptr] = queued_key >> 8;
    memory[0x41C] = 0x1E + ((ptr + 2) & 0x1F);
}

// BIOS keyboards handler
void intr16(void)
{
    debug(debug_int, "B-16%04X: BX=%04X\n", cpuGetAX(), cpuGetBX());
    unsigned ax = cpuGetAX();
    switch(ax >> 8)
    {
    case 0:    // GET KEY
    case 0x10: // GET ENHANCED KEY:
        // TODO: implement differences between 00h / 10h
        ax = getch(0);
        cpuSetAX(ax);
        break;
    case 1:    // GET KEY AVAILABLE
    case 0x11: // CHECK FOR ENHANCED KEY AVAILABLE
        // TODO: implement differences between 01h / 11h
        ax = kbhit();
        cpuSetAX(ax);
        if(ax == 0)
            cpuSetFlag(cpuFlag_ZF);
        else
            cpuClrFlag(cpuFlag_ZF);
        break;
    case 2: // GET SHIFT FLAGS
        // Start keyboard handling and read key to fill mod_state
        kbhit();
        cpuSetAX(mod_state);
        break;
    default:
        debug(debug_int, "UNHANDLED INT 16, AX=%04x\n", cpuGetAX());
    }
}
