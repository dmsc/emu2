
#include "timer.h"
#include "dbg.h"
#include "emu.h"

#include <inttypes.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

// Emulate BIOS time
static uint32_t bios_timer = 0;
static uint16_t bios_dater = 0;
static uint64_t start_timer = 0;

// Reads BIOS timer.
// We emulate the timer as counting exactly 1573040 (0x1800B0) counts per day,
// as per IBM PC standard; this gives a frequency of (19663/1080)Hz
static int64_t time_to_bios(struct timeval tv)
{
    int64_t sec = tv.tv_sec;
    int64_t usec = tv.tv_usec;
    return sec * 19663 / 1080 + usec * 19663 / 1080000000;
}

void update_timer(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    if(start_timer == 0) {
        // Create a time_t value at the start of the day, in local time
        struct timeval td = tv;
        struct tm lt;
        localtime_r(&td.tv_sec, &lt);
        lt.tm_sec = lt.tm_min = lt.tm_hour = 0;
        td.tv_sec = mktime(&lt);
        start_timer = time_to_bios(td);
    }

    long cnt = time_to_bios(tv) - start_timer;
    bios_timer = cnt % 0x1800B0;
    bios_dater = (cnt / 0x1800B0) & 0xFF;
    put32(0x46C, bios_timer);
    memory[0x470] = bios_dater;
}

// Set BIOS timer directly
static void set_timer(unsigned x)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    start_timer = time_to_bios(tv) + x;
    update_timer();
}

// Emulate i8253 timers
// TODO: emulate timer interrupts.
static struct i8253_timer
{
    long load_time;
    uint16_t load_value;
    uint16_t rd_latch;
    uint16_t wr_latch;
    int8_t op_mode;
    int8_t rd_mode;
    int8_t wr_mode;
    int8_t latched;
} timers[3];

// Timer Read/Write states:
#define TIMER_LSB    0
#define TIMER_MSB    1
#define TIMER_WORD_L 2
#define TIMER_WORD_M 3

// Returns port timer at 1193179.97HZ
static long get_timer_clock(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    // us in microseconds
    // (don't use full seconds resolution, to avoid losing precision
    double us = (tv.tv_sec & 0xFFFFFF) * 1000000.0 + tv.tv_usec;
    // Convert to "counts"
    us = us * (105.0 / 88.0);
    // And return as long (64 bits)
    return lrint(us);
}

// Get actual value in timer
static uint16_t get_actual_timer(struct i8253_timer *t)
{
    uint64_t elapsed = get_timer_clock() - t->load_time;
    debug(debug_int, "timer elapsed: %" PRIu64 "\n", elapsed);
    switch(t->op_mode & 7)
    {
    case 2: // RATE GENERATOR
    case 3: // SQUARE WAVE GENERATOR
        if(t->load_value)
            return t->load_value - (elapsed % (t->load_value));
        else
            return -(elapsed & 0xFFFF);
    default:
        return t->load_value - elapsed;
    }
}

// Implement reading/writing to timer ports
uint8_t port_timer_read(uint16_t port)
{
    int tnum = port & 0x03;
    if(tnum == 3)
    {
        debug(debug_int, "INVALID timer port read $%02x\n", port);
        return 0xFF; // Invalid in original i8253
    }

    struct i8253_timer *t = &timers[tnum];

    // Value to read can be "realtime" or "latched":
    uint16_t tval = 0;
    if(t->latched)
    {
        tval = t->rd_latch;
        // Clear the latch except if we are reading in word mode
        if(!(t->rd_mode == TIMER_WORD_L))
            t->latched = 0;
    }
    else
        tval = get_actual_timer(t);

    // Convert internal timer value to port read
    uint8_t ret = 0;
    if(t->rd_mode == TIMER_LSB)
        ret = tval & 0xFF;
    else if(t->rd_mode == TIMER_MSB)
        ret = tval >> 8;
    else if(t->rd_mode == TIMER_WORD_L)
    {
        ret = tval & 0xFF;
        t->rd_mode = TIMER_WORD_M;
    }
    else
    {
        ret = tval >> 8;
        t->rd_mode = TIMER_WORD_L;
    }

    debug(debug_int, "timer port read $%02x = %02x (mode=%02x, r_state=%d, latch=%d)\n",
          port, ret, (unsigned)(t->op_mode), t->rd_mode, t->latched);

    return ret;
}

void port_timer_write(uint16_t port, uint8_t val)
{
    int tnum = port & 0x03;
    if(tnum == 3)
    {
        // PORT 43h: timer control word
        tnum = (val >> 6);
        if(tnum == 3)
        {
            // Invalid in original i8253
            debug(debug_int, "INVALID timer port read $%02x\n", port);
            return;
        }
        struct i8253_timer *t = &timers[tnum];
        int rl = (val >> 4) & 3;
        if(rl == 0)
        {
            // Latch value to read
            t->rd_latch = get_actual_timer(t);
            t->latched = 1;
            debug(debug_int,
                  "timer port write $%02x = %02x (latching timer %d, value=%04x)\n", port,
                  val, tnum, t->rd_latch);
            return;
        }
        // TODO: don't support BCD mode
        t->op_mode = (val >> 1) & 7;
        if(rl == 1)
        {
            t->rd_mode = TIMER_LSB;
            t->wr_mode = TIMER_LSB;
        }
        else if(rl == 2)
        {
            t->rd_mode = TIMER_MSB;
            t->wr_mode = TIMER_MSB;
        }
        else
        {
            t->rd_mode = TIMER_WORD_L;
            t->wr_mode = TIMER_WORD_L;
        }
        debug(debug_int,
              "timer port write $%02x = %02x (setup timer %d, RL=%d, MODE=%d, BCD=%d)\n",
              port, val, tnum, rl, t->op_mode, val & 1);
    }
    else
    {
        // Write to timer value
        struct i8253_timer *t = &timers[tnum];

        if(t->wr_mode == TIMER_WORD_L)
        {
            // NOTE: data-sheet specifies invalid data could be read if
            //       the timer is between updates, we simply don't update
            //       after the full word is written.
            t->wr_latch = val;
            t->wr_mode = TIMER_WORD_M;
            debug(debug_int, "timer port write $%02x = %02x (timer %d, latched %02x)\n",
                  port, val, tnum, val);
            return;
        }

        t->load_time = get_timer_clock();

        if(t->wr_mode == TIMER_LSB)
            t->load_value = (t->load_value & 0xFF00) | val;
        else if(t->wr_mode == TIMER_MSB)
            t->load_value = (t->load_value & 0x00FF) | (val << 8);
        else
        {
            t->load_value = t->wr_latch | (val << 8);
            t->rd_mode = TIMER_WORD_L;
        }
        debug(debug_int, "timer port write $%02x = %02x (timer %d, counter=%04x)\n", port,
              val, tnum, t->load_value);
    }
}

uint32_t get_bios_timer(void)
{
    return bios_timer;
}

static uint16_t bcd(uint16_t v, int digits)
{
    uint16_t bcd = 0;
    for(int i = 0; i < digits; i++)
    {
        bcd |= (v % 10) << (i * 4);
        v /= 10;
    }
    return bcd;
}

// BIOS TIMER
void intr1A(void)
{
    debug(debug_int, "B-1A%04X: BX=%04X\n", cpuGetAX(), cpuGetBX());
    unsigned ax = cpuGetAX();
    switch(ax >> 8)
    {
    case 0: // TIME
    {
        update_timer();
        cpuSetDX(bios_timer & 0xFFFF);
        cpuSetCX((bios_timer >> 16) & 0xFFFF);
        cpuSetAX(bios_dater);
        debug(debug_int, "GET TIME: %02x:%04x:%04x\n", cpuGetAX(), cpuGetCX(),
              cpuGetDX());
        break;
    }
    case 1: // SET SYSTEM TIME
    {
        unsigned t = cpuGetDX() + (cpuGetCX() << 16);
        set_timer(t);
        debug(debug_int, "SET TIME: %02x:%04x:%04x\n", cpuGetAX(), cpuGetCX(),
              cpuGetDX());
        break;
    }
    case 2: // GET RTC TIME
    {
        time_t tm = time(0);
        struct tm lt;
        if(localtime_r(&tm, &lt))
        {
            cpuSetDX(bcd(lt.tm_sec, 2) << 8);
            cpuSetCX((bcd(lt.tm_hour, 2) << 8) | bcd(lt.tm_min, 2));
        }
        break;
    }
    case 4: // GET RTC DATE
    {
        time_t tm = time(0);
        struct tm lt;
        if(localtime_r(&tm, &lt))
        {
            cpuSetDX((bcd(lt.tm_mon + 1, 2) << 8) | bcd(lt.tm_mday, 2));
            cpuSetCX(bcd(lt.tm_year + 1900, 4));
        }
        break;
    }
    default:
        debug(debug_int, "UNHANDLED INT 1A, AX=%04x\n", ax);
        break;
    }
}
