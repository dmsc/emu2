#include "dos.h"
#include "codepage.h"
#include "dbg.h"
#include "dosnames.h"
#include "emu.h"
#include "env.h"
#include "keyb.h"
#include "loader.h"
#include "timer.h"
#include "utils.h"
#include "video.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// NLS data pointers
static uint32_t nls_uppercase_table;
static uint32_t nls_terminator_table;
static uint32_t nls_collating_table;
static uint32_t nls_dbc_set_table;
static uint8_t *nls_country_info;
static uint32_t dos_sysvars;

// Disk Transfer Area, buffer for find-first-file output.
static int dosDTA;

// Allocates memory for static DOS tables, from "rom" memory
static uint32_t get_static_memory(uint16_t bytes, uint16_t align)
{
    static uint32_t current = 0xFE000; // Start allocating at F000:0000
    // Align
    if(align)
        current = (current + align - 1) & ~(align - 1);
    if(current + bytes >= 0xFF0000)
        print_error("not enough static DOS memory\n");
    current += bytes;
    return current - bytes;
}

// DOS file handles
#define max_handles (0x10000)
static FILE *handles[max_handles];
static int devinfo[max_handles];

static int guess_devinfo(FILE *f)
{
    int fn = fileno(f);
    if(isatty(fn))
        return 0x80D3;
    struct stat s;
    if(fstat(fn, &s))
        return 0x80C4;
    if(S_ISREG(s.st_mode))
        return 0x0002; // TODO: assuming C: drive
    return 0x80C0;
}

static void init_handles(void)
{
    handles[0] = stdin;
    handles[1] = stdout;
    handles[2] = stderr;
    handles[3] = 0; // AUX
    handles[4] = 0; // PRN
    // stdin,stdout,stderr: special, eof on input, is device
    for(int i = 0; i < 3; i++)
        devinfo[i] = guess_devinfo(handles[i]);
}

static int get_new_handle(void)
{
    int i;
    for(i = 5; i < max_handles; i++)
        if(!handles[i])
            return i;
    return 0;
}

static int dos_close_file(int h)
{
    FILE *f = handles[h];
    if(!f)
    {
        cpuSetFlag(cpuFlag_CF);
        cpuSetAX(6);
        return -1;
    }
    handles[h] = 0;
    devinfo[h] = 0;
    cpuClrFlag(cpuFlag_CF);
    for(int i = 0; i < max_handles; i++)
        if(handles[i] == f)
            return 0; // Still referenced, don't really close
    if(f == stdin || f == stdout || f == stderr)
        return 0; // Never close standard streams
    fclose(f);
    return 0;
}

static void create_dir(void)
{
    char *fname = dos_unix_path(cpuGetAddrDS(cpuGetDX()), 1);
    debug(debug_dos, "\tmkdir '%s' ", fname);
    if(0 != mkdir(fname, 0777))
    {
        free(fname);
        cpuSetFlag(cpuFlag_CF);
        if(errno == EACCES)
            cpuSetAX(5);
        else if(errno == ENAMETOOLONG || errno == ENOTDIR)
            cpuSetAX(3);
        else if(errno == ENOENT)
            cpuSetAX(2);
        else if(errno == EEXIST)
            cpuSetAX(5);
        else
            cpuSetAX(1);
        debug(debug_dos, "ERROR %d\n", cpuGetAX());
        return;
    }
    debug(debug_dos, "OK\n");
    free(fname);
    cpuClrFlag(cpuFlag_CF);
}

static void remove_dir(void)
{
    char *fname = dos_unix_path(cpuGetAddrDS(cpuGetDX()), 1);
    debug(debug_dos, "\trmdir '%s' ", fname);
    if(0 != rmdir(fname))
    {
        free(fname);
        cpuSetFlag(cpuFlag_CF);
        if(errno == EACCES)
            cpuSetAX(5);
        else if(errno == ENAMETOOLONG || errno == ENOTDIR)
            cpuSetAX(3);
        else if(errno == ENOENT)
            cpuSetAX(2);
        else
            cpuSetAX(1);
        debug(debug_dos, "ERROR %d\n", cpuGetAX());
        return;
    }
    debug(debug_dos, "OK\n");
    free(fname);
    cpuClrFlag(cpuFlag_CF);
}

static void dos_open_file(int create)
{
    int h = get_new_handle();
    int al = cpuGetAX() & 0xFF;
    if(!h)
    {
        cpuSetAX(4);
        cpuSetFlag(cpuFlag_CF);
        return;
    }
    int name_addr = cpuGetAddrDS(cpuGetDX());
    char *fname = dos_unix_path(name_addr, create);
    if(!memory[name_addr] || !fname)
    {
        debug(debug_dos, "\t(file not found)\n");
        cpuSetAX(2);
        cpuSetFlag(cpuFlag_CF);
        return;
    }
    const char *mode;
    if(create)
        mode = "w+b";
    else
    {
        switch(al & 7)
        {
        case 0:
            mode = "rb";
            break;
        case 1:
            mode = "r+b";
            break;
        case 2:
            mode = "r+b";
            break;
        default:
            free(fname);
            cpuSetFlag(cpuFlag_CF);
            return;
        }
    }
    debug(debug_dos, "\topen '%s', '%s', %04x ", fname, mode, h);
    if(create == 2)
    {
        // Force exclusive access. We could use mode = "x+" in glibc.
        int fd = open(fname, O_CREAT | O_EXCL | O_RDWR, 0666);
        if(fd != -1)
            handles[h] = fdopen(fd, mode);
        else
            handles[h] = 0;
    }
    else
        handles[h] = fopen(fname, mode);
    if(!handles[h])
    {
        if(errno != ENOENT)
        {
            debug(debug_dos, "%s.\n", strerror(errno));
            cpuSetAX(5);
        }
        else
        {
            debug(debug_dos, "not found.\n");
            cpuSetAX(2);
        }
        cpuSetFlag(cpuFlag_CF);
        free(fname);
        return;
    }
    // Set device info:
    if(!strcmp(fname, "/dev/null"))
        devinfo[h] = 0x80C4;
    else if(!strcmp(fname, "/dev/tty"))
        devinfo[h] = 0x80D3;
    else if(memory[name_addr + 1] == ':')
    {
        uint8_t c = memory[name_addr];
        c = (c >= 'a') ? c - 'a' : c - 'A';
        if(c > 26)
            c = dos_get_default_drive();
        devinfo[h] = 0x0000 + c;
    }
    else
        devinfo[h] = 0x0000 + dos_get_default_drive();
    debug(debug_dos, "OK.\n");
    cpuClrFlag(cpuFlag_CF);
    cpuSetAX(h);
    free(fname);
}

static int get_ex_fcb(void)
{
    return cpuGetAddrDS(cpuGetDX());
}

static int get_fcb(void)
{
    int fcb = cpuGetAddrDS(cpuGetDX());
    return memory[fcb] == 255 ? fcb + 7 : fcb;
}

static int get_fcb_handle(void)
{
    return get16(0x18 + get_fcb());
}

static void dos_show_fcb()
{
    if(!debug_active(debug_dos))
        return;

    int addr = cpuGetAddrDS(cpuGetDX());
    char *name = getstr(addr + 1, 11);
    debug(debug_dos,
          "\tFCB:"
          "[d=%02x:n=%.8s.%.3s:bn=%04x:rs=%04x:fs=%08x:h=%04x:rn=%02x:ra=%08x]\n",
          memory[addr], name, name + 8, get16(addr + 0x0C), get16(addr + 0x0E),
          get32(addr + 0x10), get16(addr + 0x18), memory[addr + 0x20],
          get32(addr + 0x21));
}

static void dos_open_file_fcb(int create)
{
    int h = get_new_handle();
    if(!h)
    {
        cpuSetAL(0xFF);
        cpuSetFlag(cpuFlag_CF);
        return;
    }
    int fcb_addr = get_fcb();
    char *fname = dos_unix_path_fcb(fcb_addr, create);
    if(!fname)
    {
        debug(debug_dos, "\t(file not found)\n");
        cpuSetAL(0xFF);
        cpuSetFlag(cpuFlag_CF);
        return;
    }
    const char *mode = create ? "w+b" : "r+b";
    debug(debug_dos, "\topen fcb '%s', '%s', %04x ", fname, mode, h);
    handles[h] = fopen(fname, mode);
    if(!handles[h])
    {
        debug(debug_dos, "%s.\n", strerror(errno));
        cpuSetAL(0xFF);
        cpuSetFlag(cpuFlag_CF);
        free(fname);
        return;
    }
    // Get file size
    fseek(handles[h], 0, SEEK_END);
    long sz = ftell(handles[h]);
    fseek(handles[h], 0, SEEK_SET);
    // Set FCB info:
    put16(fcb_addr + 0x0C, 0);   // block number
    put16(fcb_addr + 0x0E, 128); // record size
    put32(fcb_addr + 0x10, sz);  // file size
    put16(fcb_addr + 0x14, 0);   // date of last write
    put16(fcb_addr + 0x16, 0);   // time of last write
    put16(fcb_addr + 0x18, h);   // reserved - store DOS handle!
    memory[fcb_addr + 0x20] = 0; // current record

    debug(debug_dos, "OK.\n");
    cpuClrFlag(cpuFlag_CF);
    cpuSetAL(0x00);
    dos_show_fcb();
    free(fname);
}

static void dos_fcb_rand_to_block(int fcb)
{
    // Update block position from random position
    unsigned rnum = get32(0x21 + fcb);
    memory[0x20 + fcb] = rnum & 127;
    put16(0x0C, rnum / 128);
}

static int dos_read_record_fcb(int addr, int update)
{
    FILE *f = handles[get_fcb_handle()];
    if(!f)
        return 1; // no data read

    int fcb = get_fcb();
    unsigned rsize = get16(0x0E + fcb);
    unsigned pos = rsize * get32(0x21 + fcb);
    uint8_t *buf = getptr(addr, rsize);
    if(!buf || !rsize)
    {
        debug(debug_dos, "\tbuffer pointer invalid\n");
        return 2; // segment wrap in DTA
    }
    // Seek to block and read
    if(fseek(f, pos, SEEK_SET))
        return 1; // no data read
    // Read
    unsigned n = fread(buf, 1, rsize, f);
    // Update random and block positions
    if(update)
    {
        put32(0x21 + fcb, (pos + n) / rsize);
        dos_fcb_rand_to_block(fcb);
    }

    if(n == rsize)
        return 0; // read full record
    else if(!n)
        return 1; // EOF
    else
    {
        for(unsigned i = n; i < rsize; i++)
            buf[i] = 0;
        return 3; // read partial record
    }
}

int dos_write_record_fcb(int addr, int update)
{
    FILE *f = handles[get_fcb_handle()];
    if(!f)
        return 1; // no data write

    int fcb = get_fcb();
    unsigned rsize = get16(0x0E + fcb);
    unsigned pos = rsize * get32(0x21 + fcb);
    uint8_t *buf = getptr(addr, rsize);
    if(!buf || !rsize)
    {
        debug(debug_dos, "\tbuffer pointer invalid\n");
        return 2; // segment wrap in DTA
    }
    // Seek to block and read
    if(fseek(f, pos, SEEK_SET))
        return 1; // no data read
    // Write
    unsigned n = fwrite(buf, 1, rsize, f);
    // Update random and block positions
    if(update)
    {
        put32(0x21 + fcb, (pos + n) / rsize);
        dos_fcb_rand_to_block(fcb);
    }
    // Update file size
    if(pos + n > get32(fcb + 0x10))
        put32(fcb + 0x10, pos + n);

    if(n == rsize)
        return 0; // write full record
    else
        return 3; // disk full
}

// Converts Unix time_t to DOS time/date
static uint32_t get_time_date(time_t tm)
{
    struct tm lt;
    if(localtime_r(&tm, &lt))
    {
        unsigned t = (lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec / 2);
        unsigned d = ((lt.tm_year - 80) << 9) | ((lt.tm_mon + 1) << 5) | (lt.tm_mday);
        return (d << 16) | t;
    }
    else
        return (1 << 16) | 1;
}

// Converts Unix mode to DOS attributes
static int get_attributes(mode_t md)
{
    int r = 0;
    // See if it's a special file
    if(S_ISDIR(md))
        r |= 1 << 4; // DIR
    else if(!S_ISREG(md))
        r |= 1 << 2; // SYSTEM
    else
        r |= 1 << 5; // ARCHIVE
    // See if the file is writable
    if(0 == (md & (S_IWOTH | S_IWGRP | S_IWUSR)))
        r |= 1 << 0; // READ_ONLY
    return r;
}

// DOS int 21, ah=43
static void int21_43(void)
{
    unsigned al = cpuGetAX() & 0xFF;
    int dname = cpuGetAddrDS(cpuGetDX());
    if(al == 0)
    {
        char *fname = dos_unix_path(dname, 0);
        if(!fname)
        {
            debug(debug_dos, "\t(file not found)\n");
            cpuSetFlag(cpuFlag_CF);
            cpuSetAX(2);
            return;
        }
        debug(debug_dos, "\tattr '%s' = ", fname);
        // GET FILE ATTRIBUTES
        struct stat st;
        if(0 != stat(fname, &st))
        {
            cpuSetFlag(cpuFlag_CF);
            if(errno == EACCES)
                cpuSetAX(5);
            else if(errno == ENAMETOOLONG || errno == ENOTDIR)
                cpuSetAX(3);
            else if(errno == ENOENT)
                cpuSetAX(2);
            else
                cpuSetAX(1);
            free(fname);
            debug(debug_dos, "ERROR %d\n", cpuGetAX());
            return;
        }
        cpuClrFlag(cpuFlag_CF);
        cpuSetCX(get_attributes(st.st_mode));
        debug(debug_dos, "%04X\n", cpuGetCX());
        free(fname);
        return;
    }
    cpuSetFlag(cpuFlag_CF);
    cpuSetAX(1);
    return;
}

// Each DTA (Data Transfer Area) in memory can hold a find-first data
// block. We simply encode our pointer in this area and use this struct
// to hold the values.
#define NUM_FIND_FIRST_DTA 64
static struct find_first_dta
{
    // List of files to return and pointer to current.
    struct dos_file_list *find_first_list;
    struct dos_file_list *find_first_ptr;
    // Address of DTA in dos memory - 0 is unused
    unsigned dta_addr;
} find_first_dta[NUM_FIND_FIRST_DTA];

// Search the list an returns the position
static struct find_first_dta *get_find_first_dta(void)
{
    int i;
    for(i = 0; i < NUM_FIND_FIRST_DTA; i++)
        if(find_first_dta[i].dta_addr == dosDTA)
            break;
    if(i == NUM_FIND_FIRST_DTA)
    {
        for(i = 0; i < NUM_FIND_FIRST_DTA; i++)
            if(!find_first_dta[i].dta_addr)
                break;
        if(i == NUM_FIND_FIRST_DTA)
        {
            print_error("Too many find-first DTA areas opened\n");
            i = 0;
        }
        find_first_dta[i].dta_addr = dosDTA;
        find_first_dta[i].find_first_list = 0;
        find_first_dta[i].find_first_ptr = 0;
    }
    return &find_first_dta[i];
}

// Removes a DTA from the list
static void clear_find_first_dta(struct find_first_dta *p)
{
    unsigned x = find_first_dta - p;
    if(x >= NUM_FIND_FIRST_DTA)
        return;
    p->dta_addr = 0;
    p->find_first_ptr = 0;
    dos_free_file_list(p->find_first_list);
}

// DOS int 21, ah=4f
static void dos_find_next(int first)
{
    struct find_first_dta *p = get_find_first_dta();
    struct dos_file_list *d = p->find_first_ptr;
    if(!d || !p->find_first_ptr->unixname)
    {
        debug(debug_dos, "\t(end)\n");
        clear_find_first_dta(p);
        cpuSetFlag(cpuFlag_CF);
        cpuSetAX(first ? 0x02 : 0x12);
    }
    else
    {
        debug(debug_dos, "\t'%s' ('%s')\n", d->dosname, d->unixname);

        // Fills the Find First Data from a dos/unix name pair
        if(strcmp("//", d->unixname))
        {
            // Normal file/directory
            struct stat st;
            if(0 == stat(d->unixname, &st))
            {
                memory[dosDTA + 0x15] = get_attributes(st.st_mode);
                put32(dosDTA + 0x16, get_time_date(st.st_mtime));
                put32(dosDTA + 0x1A, (st.st_size > 0x7FFFFFFF) ? 0x7FFFFFFF : st.st_size);
            }
            else
            {
                memory[dosDTA + 0x15] = 0;
                put32(dosDTA + 0x16, 0x10001);
                put32(dosDTA + 0x1A, 0);
            }
        }
        else
        {
            // Fills volume label data
            memory[dosDTA + 0x15] = 8;
            put32(dosDTA + 0x16, get_time_date(time(0)));
            put32(dosDTA + 0x1A, 0);
        }
        // Fills dos file name
        putmem(dosDTA + 0x1E, d->dosname, 13);
        // Next file
        p->find_first_ptr++;
        cpuClrFlag(cpuFlag_CF);
    }
}

// DOS int 21, ah=4e
static void dos_find_first(void)
{
    struct find_first_dta *p = get_find_first_dta();
    // Gets all directory entries
    if(p->find_first_list)
        dos_free_file_list(p->find_first_list);

    // Check if we want the volume label
    if(cpuGetCX() & 8)
    {
        p->find_first_list = calloc(2, sizeof(struct dos_file_list));
        p->find_first_list[0].unixname = strdup("//");
        memcpy(p->find_first_list[0].dosname, "DISK LABEL", 11);
        p->find_first_list[1].unixname = 0;
    }
    else
        p->find_first_list = dos_find_first_file(cpuGetAddrDS(cpuGetDX()));

    p->find_first_ptr = p->find_first_list;
    return dos_find_next(1);
}

static void dos_find_next_fcb(void)
{
    struct find_first_dta *p = get_find_first_dta();
    struct dos_file_list *d = p->find_first_ptr;

    if(!d || !p->find_first_ptr->unixname)
    {
        debug(debug_dos, "\t(end)\n");
        clear_find_first_dta(p);
        cpuSetAL(0xFF);
    }
    else
    {
        debug(debug_dos, "\t'%s' ('%s')\n", d->dosname, d->unixname);
        // Fills output FCB at DTA - use extended or normal depending on input
        int ofcb = memory[get_ex_fcb()] == 0xFF ? dosDTA + 7 : dosDTA;
        int pos = 1;
        for(uint8_t *c = d->dosname; *c; c++)
        {
            if(*c != '.')
                memory[ofcb + pos++] = *c;
            else
                while(pos < 9)
                    memory[ofcb + pos++] = ' ';
        }
        while(pos < 12)
            memory[ofcb + pos++] = ' ';
        // Fill drive letter
        memory[ofcb] = memory[get_fcb()];
        // Get file info
        if(strcmp("//", d->unixname))
        {
            // Normal file/directory
            struct stat st;
            if(0 == stat(d->unixname, &st))
            {
                memory[ofcb + 0x0C] = get_attributes(st.st_mode);
                put32(ofcb + 0x17, get_time_date(st.st_mtime));
                put32(ofcb + 0x1D, (st.st_size > 0x7FFFFFFF) ? 0x7FFFFFFF : st.st_size);
            }
            else
            {
                memory[ofcb + 0x0C] = 0;
                put32(ofcb + 0x17, 0x10001);
                put32(ofcb + 0x1D, 0);
            }
        }
        else
        {
            memory[ofcb + 0x0C] = 8;
            put32(ofcb + 0x17, get_time_date(time(0)));
            put32(ofcb + 0x1D, 0);
        }
        p->find_first_ptr++;
        cpuSetAL(0x00);
    }
}

static void dos_find_first_fcb(void)
{
    struct find_first_dta *p = get_find_first_dta();
    // Gets all directory entries
    if(p->find_first_list)
        dos_free_file_list(p->find_first_list);

    int efcb = get_ex_fcb();
    if(memory[efcb] == 0xFF && memory[efcb + 6] == 0x08)
    {
        p->find_first_list = calloc(2, sizeof(struct dos_file_list));
        p->find_first_list[0].unixname = strdup("//");
        memcpy(p->find_first_list[0].dosname, "DISK LABEL", 11);
        p->find_first_list[1].unixname = 0;
    }
    else
        p->find_first_list = dos_find_first_file_fcb(get_fcb());
    p->find_first_ptr = p->find_first_list;
    return dos_find_next_fcb();
}

// DOS int 21, ah=57
static void int21_57(void)
{
    unsigned al = cpuGetAX() & 0xFF;
    FILE *f = handles[cpuGetBX()];
    if(!f)
    {
        cpuSetFlag(cpuFlag_CF);
        cpuSetAX(6); // invalid handle
        return;
    }
    if(al == 0)
    {
        // GET FILE LAST WRITTEN TIME
        struct stat st;
        if(0 != fstat(fileno(f), &st))
        {
            cpuSetFlag(cpuFlag_CF);
            cpuSetAX(1);
            return;
        }
        cpuClrFlag(cpuFlag_CF);
        uint32_t td = get_time_date(st.st_mtime);
        cpuSetCX(td & 0xFFFF);
        cpuSetDX(td >> 16);
        return;
    }
    else if(al == 1)
    {
        // SET FILE LAST WRITTEN TIME
        cpuClrFlag(cpuFlag_CF);
        return;
    }
    cpuSetFlag(cpuFlag_CF);
    cpuSetAX(1);
    return;
}

static void dos_get_drive_info(uint8_t drive)
{
    if(!drive)
        drive = dos_get_default_drive();
    else
        drive--;
    cpuSetAL(32);     // 16k clusters
    cpuSetCX(512);    // 512 bytes/sector
    cpuSetDX(0xFFFF); // total 1GB
    cpuSetBX(0x0000); // media ID byte, offset
    cpuSetDS(0x0000); // and segment
    cpuClrFlag(cpuFlag_CF);
}

// Writes a character to standard output.
static void dos_putchar(uint8_t ch)
{
    if(devinfo[1] == 0x80D3 && video_active())
        video_putch(ch);
    else if(!handles[1])
        putchar(ch);
    else
        fputc(ch, handles[1]);
}

static void int21_9(void)
{
    int i = cpuGetAddrDS(cpuGetDX());

    for(; memory[i] != 0x24 && i < 0x100000; i++)
        dos_putchar(memory[i]);

    cpuSetAL(0x24);
}

static int return_code;
// Runs the emulator again with given parameters
static int run_emulator(char *file, const char *prgname, char *cmdline, char *env)
{
    pid_t pid = fork();
    if(pid == -1)
        print_error("fork error, %s\n", strerror(errno));
    else if(pid != 0)
    {
        int ret, status;
        while((ret = waitpid(pid, &status, 0)) == -1)
        {
            if(errno != EINTR)
            {
                print_error("error waiting child, %s\n", strerror(errno));
                break;
            }
        }
        return_code = (WEXITSTATUS(status) & 0xFF);
        if(!WIFEXITED(status))
            return_code |= 0x100;
        if(return_code)
            debug(debug_dos, "child exited with code %04x\n", return_code);
        return return_code > 0xFF;
    }
    else
    {
        // Set program name
        setenv(ENV_PROGNAME, prgname, 1);
        // default drive
        char drv[2] = {0, 0};
        drv[0] = dos_get_default_drive() + 'A';
        setenv(ENV_DEF_DRIVE, drv, 1);
        // and CWD
        setenv(ENV_CWD, (const char *)dos_get_cwd(0), 1);
        // pass open file descriptors to child process
        for(unsigned i = 0; i < 3; i++)
            if(handles[i])
            {
                int f1 = fileno(handles[i]);
                int f2 = (f1 < 3) ? dup(f1) : f1;
                dup2(f2, i);
                close(f2);
                if(f1 >= 3)
                    close(f1);
            }
        // Accumulate args:
        char *args[64];
        int i;
        const char *exe_path = get_program_exe_path();
        if(!exe_path)
            print_error("can't get emulator path.\n");
        args[0] = prog_name;
        args[1] = file;
        args[2] = cmdline;
        args[3] = "--";
        for(i = 4; i < 63 && *env; i++)
        {
            int l = strlen(env);
            args[i] = env;
            env += l + 1;
        }
        for(; i < 64; i++)
            args[i] = 0;
        if(execv(exe_path, args) == -1)
        {
            raise(SIGABRT);
            exit(1);
        }
    }
    return 0;
}

// DOS exit
void int20()
{
    exit(0);
}

static void char_input(int brk)
{
    static uint16_t last_key;
    fflush(handles[1] ? handles[1] : stdout);

    if(last_key == 0)
    {
        if(devinfo[0] != 0x80D3 && handles[0])
            last_key = getc(handles[0]);
        else
            last_key = getch(brk);
    }
    debug(debug_dos, "\tgetch = %02x '%c'\n", last_key, (char)last_key);
    cpuSetAL(last_key);
    if((last_key & 0xFF) == 0)
        last_key = last_key >> 8;
    else
        last_key = 0;
}

static int line_input(FILE *f, uint8_t *buf, int max)
{
    if(video_active())
    {
        int len = 0;
        while(len < max - 1)
        {
            char key = getch(1);
            if(key == '\r')
            {
                video_putch('\r');
                video_putch('\n');
                buf[len] = '\r';
                buf[len+1] = '\n';
                len += 2;
                break;
            }
            else if(key == 8)
            {
                if(len)
                {
                    len--;
                    video_putch(key);
                    video_putch(' ');
                    video_putch(key);
                }
            }
            else if(len < max && video_get_col() < 79)
            {
                video_putch(key);
                buf[len] = key;
                len++;
            }
        }
        return len;
    }
    else
    {
        int i, cr = 0;
        for(i = 0; i < max; i++)
        {
            int c = fgetc(f);
            if(c < 0)
                break;
            if(c == '\n' && !cr && i < max)
            {
                cr = 1;
                buf[i] = '\r';
                i++;
            }
            else if(c == '\r')
                cr = 1;
            buf[i] = c;
            if(c == '\n')
            {
                i++;
                break;
            }
        }
        return i;
    }
}

static void int21_debug(void)
{
    static const char *func_names[] =
    {
        "terminate", "getchar", "putchar", "getc(aux)", "putc(aux)", // 0-4
        "putc(prn)", "console i/o", "getch", "getch", "puts", // 5-9
        "gets", "eof(stdin)", "flush(stdin)+", "disk reset", "set drive", // 0A-0E
        "open fcb", "close fcb", "find first fcb", "find next fcb", "del fcb", // 0F-13
        "read fcb", "write fcb", "creat fcb", "rename fcb", "n/a", // 14-18
        "get drive", "set DTA", "stat def drive", "stat drive", "n/a", // 19-1D
        "n/a", "get def DPB", "n/a", "read fcb", "write fcb", // 1E-22
        "size fcb", "set record fcb", "set int vect", "create PSP", "read blk fcb", // 23-27
        "write blk fcb", "parse filename", "get date", "set date", "get time", // 28-2C
        "set time", "set verify", "get DTA", "version", "go TSR", // 2D-31
        "get DPB", "g/set brk check", "InDOS addr", "get int vect", "get free", // 32-36
        "get/set switch", "country info", "mkdir", "rmdir", "chdir", // 37-3B
        "creat", "open", "close", "read", "write", // 3C-40
        "unlink", "lseek", "get/set attr", "g/set devinfo", "dup", // 41-45
        "dup2", "get CWD", "mem alloc", "mem free", "mem resize", // 46-4A
        "exec", "exit", "get errorlevel", "find first", "find next", // 4B-4F
        "set PSP", "get PSP", "get sysvars", "trans BPB to DPB", "get verify", // 50-54
        "create PSP", "rename", "g/set file dates", "g/set alloc type", "ext error", // 55-59
        "create tmpfile", "creat new file", "flock", "(server fn)", "(net fn)", // 5A-5E
        "(net redir)", "truename", "n/a", "get PSP", "intl char info", // 5F-63
        "(internal)", "get ext country info"
    };
    unsigned ax = cpuGetAX();
    const char *fn;
    if((ax >> 8) < (sizeof(func_names) / sizeof(func_names[0])))
        fn = func_names[ax >> 8];
    else
        fn = "(unknown)";

    debug(debug_dos, "D-21%04X: %-15s BX=%04X CX:%04X DX:%04X DI=%04X DS:%04X ES:%04X\n",
          ax, fn, cpuGetBX(), cpuGetCX(), cpuGetDX(), cpuGetDI(), cpuGetDS(), cpuGetES());
}

// DOS int 21
void int21()
{
    // Check CP/M call, INT 21h from address 0x000C0
    if(cpuGetAddress(cpuGetStack(2), cpuGetStack(0)) == 0xC2)
    {
        debug(debug_dos, "CP/M CALL: ");
        int old_ax = cpuGetAX();
        int ip = cpuGetStack(10), cs = cpuGetStack(8), flags = cpuGetStack(4);
        // Fix-up registers
        cpuSetAX((cpuGetCX() << 8) | (old_ax & 0xFF));
        cpuSetSP(cpuGetSP() + 6);
        // Fix-up return address, interchanges segment/ip:
        int stack = cpuGetAddress(cpuGetSS(), cpuGetSP());
        put16(stack, ip);
        put16(stack + 2, cs);
        put16(stack + 4, flags);
        // Call ourselves
        int21();
        // Restore AH
        cpuSetAX((old_ax & 0xFF00) | (cpuGetAX() & 0xFF));
        return;
    }
    debug(debug_int, "D-21%04X: BX=%04X\n", cpuGetAX(), cpuGetBX());
    if(debug_active(debug_dos))
        int21_debug();

    // Process interrupt
    unsigned ax = cpuGetAX(), ah = ax >> 8;

    // Store SS:SP into PSP, used at return from child process
    // According to DOSBOX, only set for certain functions:
    if(ah != 0x50 && ah != 0x51 && ah != 0x62 && ah != 0x64 && ah < 0x6c)
    {
        put16(cpuGetAddress(get_current_PSP(), 0x2E), cpuGetSP());
        put16(cpuGetAddress(get_current_PSP(), 0x30), cpuGetSS());
    }

    switch(ah)
    {
    case 0: // TERMINATE PROGRAM
        exit(0);
    case 1: // CHARACTER INPUT WITH ECHO
        char_input(1);
        dos_putchar(cpuGetAX() & 0xFF);
        break;
    case 2: // PUTCH
        dos_putchar(cpuGetDX() & 0xFF);
        cpuSetAX(0x0200 | (cpuGetDX() & 0xFF)); // from intlist.
        break;
    case 0x6: // CONSOLE OUTPUT
        if((cpuGetDX() & 0xFF) == 0xFF)
            char_input(1);
        else
        {
            dos_putchar(cpuGetDX() & 0xFF);
            cpuSetAL(cpuGetDX());
        }
        break;
    case 0x7: // DIRECT CHARACTER INPUT WITHOUT ECHO
        char_input(0);
        break;
    case 0x8: // CHARACTER INPUT WITHOUT ECHO
        char_input(1);
        break;
    case 0x9: // WRITE STRING
        int21_9();
        break;
    case 0xA: // BUFFERED INPUT
    {
        FILE *f = handles[0] ? handles[0] : stdin;
        int addr = cpuGetAddrDS(cpuGetDX());
        unsigned len = memory[addr], i = 2;
        while(i < len && addr + i < 0x100000)
        {
            int c = getc(f);
            if(c == '\n' || c == EOF)
                c = '\r';
            memory[addr + i] = (char)c;
            if(c == '\r')
                break;
            i++;
        }
        memory[addr + 1] = i - 2;
        break;
    }
    case 0xB: // STDIN STATUS
        if(devinfo[0] == 0x80D3)
            cpuSetAX(kbhit() ? 0x0BFF : 0x0B00);
        else
            cpuSetAX(0x0B00);
        break;
    case 0xC: // FLUSH AND READ STDIN
        tcflush(STDIN_FILENO, TCIFLUSH);
        fflush(stdin);
        switch(ax & 0xFF)
        {
        case 0x01:
        case 0x06:
        case 0x07:
        case 0x08:
        case 0x0A:
            cpuSetAX(ax << 8);
            int21();
            return;
        }
        break;
    case 0x0E: // SELECT DEFAULT DRIVE
        dos_set_default_drive(cpuGetDX() & 0xFF);
        // Number of drives = 3, 'A:', 'B:' and 'C:'
        cpuSetAX(0x0E03);
        break;
    case 0x0F: // OPEN FILE USING FCB
        dos_open_file_fcb(0);
        break;
    case 0x10: // CLOSE FILE USING FCB
        dos_show_fcb();
        // Set full AX because dos_close_file clobbers AX
        cpuSetAX(dos_close_file(get_fcb_handle()) ? 0x10FF : 0x1000);
        break;
    case 0x11: // FIND FIRST FILE USING FCB
        dos_find_first_fcb();
        break;
    case 0x12: // FIND NEXT FILE USING FCB
        dos_find_next_fcb();
        break;
    case 0x13: // DELETE FILE USING FCB
    {
        dos_show_fcb();
        /* TODO: Limited support. No wild cards */
        int fcb_addr = get_fcb();
        char *fname = dos_unix_path_fcb(fcb_addr, 0);
        if(!fname)
        {
            debug(debug_dos, "\t(file not found)\n");
            cpuSetAL(0xFF);
            break;
        }
        debug(debug_dos, "\tdelete fcb '%s'\n", fname);
        int e = unlink(fname);
        free(fname);
        if(e)
        {
            debug(debug_dos, "\tcould not delete file (%d).\n", errno);
            cpuSetAL(0xFF);
        }
        else
        {
            memory[fcb_addr + 0x1] = 0xE5; // Marker for file deleted
            cpuSetAL(0x00);
        }
        break;
    }
    case 0x14: // SEQUENTIAL READ USING FCB
        dos_show_fcb();
        cpuSetAL(dos_read_record_fcb(dosDTA, 1));
        break;
    case 0x15: // SEQUENTIAL WRITE USING FCB
        dos_show_fcb();
        cpuSetAL(dos_write_record_fcb(dosDTA, 1));
        break;
    case 0x16: // CREATE FILE USING FCB
        dos_open_file_fcb(1);
        break;
    case 0x19: // GET DEFAULT DRIVE
        debug(debug_dos, "\tget default drive = '%c'\n", dos_get_default_drive() + 'A');
        cpuSetAL(dos_get_default_drive());
        break;
    case 0x1A: // SET DTA
        dosDTA = 0xFFFFF & (cpuGetDS() * 16 + cpuGetDX());
        break;
    case 0x1B: // GET DEFAULT DRIVE INFO
        dos_get_drive_info(0);
        break;
    case 0x1C: // GET DRIVE INFO
        dos_get_drive_info(cpuGetDX() & 0xFF);
        break;
    case 0x21: // RANDOM READ USING FCB
        dos_show_fcb();
        cpuSetAL(dos_read_record_fcb(dosDTA, 0));
        break;
    case 0x22: // RANDOM WRITE USING FCB
        dos_show_fcb();
        cpuSetAL(dos_write_record_fcb(dosDTA, 0));
        break;
    case 0x25: // set interrupt vector
        put16(4 * (ax & 0xFF), cpuGetDX());
        put16(4 * (ax & 0xFF) + 2, cpuGetDS());
        break;
    case 0x26: // Create PSP (duplicate current PSP)
    {
        uint8_t *new_psp = getptr(cpuGetAddress(cpuGetDX(), 0), 0x100);
        uint8_t *orig_psp = getptr(cpuGetAddress(get_current_PSP(), 0), 0x100);
        if(!new_psp || !orig_psp)
        {
            debug(debug_dos, "\tinvalid new PSP segment %04x.\n", cpuGetDX());
            break;
        }
        // Copy PSP to the new segment, 0x80 is what DOS does - this excludes command line
        memcpy(new_psp, orig_psp, 0x80);
        break;
    }
    case 0x27: // BLOCK READ FROM FCB
    case 0x28: // BLOCK WRITE TO FCB
    {
        dos_show_fcb();
        int fcb = get_fcb();
        unsigned count = cpuGetCX();
        unsigned rsize = get16(0x0E + fcb);
        unsigned e = 0;
        int target = dosDTA;

        while(!e && count)
        {
            if(0x27 == (ax >> 8))
                e = dos_read_record_fcb(target, 1);
            else
                e = dos_write_record_fcb(target, 1);

            if(e == 0 || e == 3)
            {
                target += rsize;
                count--;
            }
        }
        cpuSetCX(cpuGetCX() - count);
        cpuSetAL(e);
        dos_show_fcb();
        break;
    }
    case 0x29: // PARSE FILENAME TO FCB
    {
        // TODO: length could be more than 64 bytes!
        char *fname = getstr(cpuGetAddrDS(cpuGetSI()), 64);
        char *orig = fname;
        uint8_t *dst = getptr(cpuGetAddrES(cpuGetDI()), 37);
        if(!dst)
        {
            debug(debug_dos, "\tinvalid destination\n");
            cpuSetAL(0xFF);
            break;
        }
        debug(debug_dos, "\t'%s' -> ", fname);
        if(ax & 1)
            // Skip separator
            if(*fname && strchr(":;.,=+", *fname))
                fname++;
        // Skip initial spaces
        while(*fname && strchr(" \t", *fname))
            fname++;
        // Check drive:
        int ret = 0;
        dst[0] = 0;
        if(*fname && fname[1] == ':')
        {
            char d = *fname;
            if(d >= 'A' && d <= 'Z')
                dst[0] = d - 'A' + 1;
            else if(d >= 'a' && d <= 'z')
                dst[0] = d - 'a' + 1;
            else
                ret = 0xFF;
            fname += 2;
        }
        int i = 1;
        while(i < 12)
        {
            char c = *fname;
            if(c == '.' && i <= 9)
            {
                for(; i < 9; i++)
                    dst[i] = ' ';
                fname++;
            }
            else if(!c || strchr(":.;,=+ \t/\"[]<>|\x0D\x10", c))
            {
                for(; i < 12; i++)
                    dst[i] = ' ';
                break;
            }
            else if(c == '*' && i < 9)
            {
                for(; i < 9; i++)
                    dst[i] = '?';
                fname++;
                ret = 1;
            }
            else if(c == '*')
            {
                for(; i < 12; i++)
                    dst[i] = '?';
                fname++;
                ret = 1;
                break;
            }
            else
            {
                if(c >= 'a' && c <= 'z')
                    dst[i] = c - 'a' + 'A';
                else
                    dst[i] = c;
                i++;
                fname++;
            }
        }
        // Update resulting pointer
        int si = cpuGetSI() + (fname - orig);
        while(si > 0xFFFF)
        {
            si = si - 0x10;
            cpuSetDS(cpuGetDS() + 1);
        }
        cpuSetSI((si));
        cpuSetAL(ret);
        debug(debug_dos, "%c:'%.11s'\n", dst[0] ? dst[0] + '@' : '*', dst + 1);
        break;
    }
    case 0x2A: // GET SYSTEM DATE
    {
        time_t tm = time(0);
        struct tm lt;
        if(localtime_r(&tm, &lt))
        {
            cpuSetAL(lt.tm_wday);
            cpuSetCX(lt.tm_year + 1900);
            cpuSetDX(((lt.tm_mon + 1) << 8) | (lt.tm_mday));
        }
        break;
    }
    case 0x2B: // SET SYSTEM DATE
        cpuSetAL(0xFF); // Invalid date - don't support setting date.
        break;
    case 0x2C: // GET SYSTEM TIME
    {
        // Use BIOS time: 1573040 ticks per day
        uint32_t bios_timer = get_bios_timer() * 1080;
        uint32_t bsec = bios_timer / 19663;
        uint32_t bsub = bios_timer % 19663;

        uint8_t thor = bsec / 3600;
        uint8_t tmin = (bsec / 60) % 60;
        uint8_t tsec = bsec % 60;
        uint8_t msec = bsub * 100 / 19663;

        cpuSetCX((thor << 8) | tmin);
        cpuSetDX((tsec << 8) | msec);
        break;
    }
    case 0x2D: // SET SYSTEM TIME
        cpuSetAL(0xFF); // Invalid time - don't support setting time.
        break;
    case 0x2F: // GET DTA
        cpuSetES((dosDTA & 0xFFF00) >> 4);
        cpuSetBX((dosDTA & 0x000FF));
        break;
    case 0x30: // DOS version: 3.30
        cpuSetAX(0x1E03);
        cpuSetBX(0x0000);
        break;
    case 0x33: // BREAK SETTINGS
        if(ax == 0x3300)
            cpuSetDX((cpuGetDX() & 0xFF00) | 1);
        else if(ax == 0x3301)
            cpuSetDX((cpuGetDX() & 0xFF00) | 1); // Ignore new state
        break;
    case 0x35: // get interrupt vector
        cpuSetBX(get16(4 * (ax & 0xFF)));
        cpuSetES(get16(4 * (ax & 0xFF) + 2));
        break;
    case 0x36: // get free space
        cpuSetAX(32);     // 16k clusters
        cpuSetBX(0xFFFF); // all free, 1GB
        cpuSetCX(512);    // 512 bytes/sector
        cpuSetDX(0xFFFF); // total 1GB
        break;
    case 0x37: // get/set switch character
        cpuSetDX('/');
        break;
    case 0x38: // GET COUNTRY INFO
        putmem(cpuGetAddrDS(cpuGetDX()), nls_country_info, 34);
        break;
    case 0x39: // MKDIR
        create_dir();
        break;
    case 0x3A: // RMDIR
        remove_dir();
        break;
    case 0x3B: // CHDIR
    {
        if(dos_change_dir(cpuGetAddrDS(cpuGetDX())))
        {
            cpuSetAX(2);
            cpuSetFlag(cpuFlag_CF);
        }
        else
            cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x3C: // CREATE FILE
        dos_open_file(1);
        break;
    case 0x3D: // OPEN EXISTING FILE
        dos_open_file(0);
        break;
    case 0x3E: // CLOSE FILE
        dos_close_file(cpuGetBX());
        break;
    case 0x3F: // READ
    {
        FILE *f = handles[cpuGetBX()];
        if(!f)
        {
            cpuSetFlag(cpuFlag_CF);
            cpuSetAX(6); // invalid handle
            break;
        }
        uint8_t *buf = getptr(cpuGetAddrDS(cpuGetDX()), cpuGetCX());
        if(!buf)
        {
            debug(debug_dos, "\tbuffer pointer invalid\n");
            cpuSetAX(5);
            cpuSetFlag(cpuFlag_CF);
            break;
        }
        // If read from "CON", reads up to the first "CR":
        if(devinfo[cpuGetBX()] == 0x80D3)
        {
            suspend_keyboard();
            cpuSetAX(line_input(f, buf, cpuGetCX()));
        }
        else
        {
            unsigned n = fread(buf, 1, cpuGetCX(), f);
            cpuSetAX(n);
        }
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x40: // WRITE
    {
        FILE *f = handles[cpuGetBX()];
        if(!f)
        {
            cpuSetFlag(cpuFlag_CF);
            cpuSetAX(6); // invalid handle
            return;
        }
        unsigned len = cpuGetCX();
        uint8_t *buf = getptr(cpuGetAddrDS(cpuGetDX()), len);
        if(!buf)
        {
            debug(debug_dos, "\tbuffer pointer invalid\n");
            cpuSetAX(5);
            cpuSetFlag(cpuFlag_CF);
            break;
        }
        if(devinfo[cpuGetBX()] == 0x80D3 && video_active())
        {
            for(unsigned i = 0; i < len; i++)
                video_putch(buf[i]);
            cpuSetAX(len);
        }
        else
        {
            unsigned n = fwrite(buf, 1, len, f);
            cpuSetAX(n);
        }
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x41: // UNLINK
    {
        char *fname = dos_unix_path(cpuGetAddrDS(cpuGetDX()), 0);
        if(!fname)
        {
            debug(debug_dos, "\t(file not found)\n");
            cpuSetFlag(cpuFlag_CF);
            cpuSetAX(0x02);
            break;
        }
        debug(debug_dos, "\tunlink '%s'\n", fname);
        int e = unlink(fname);
        free(fname);
        if(e)
        {
            cpuSetFlag(cpuFlag_CF);
            if(errno == ENOTDIR)
                cpuSetAX(0x03);
            else if(errno == ENOENT)
                cpuSetAX(0x02);
            else
                cpuSetAX(0x05);
        }
        else
            cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x42: // LSEEK
    {
        FILE *f = handles[cpuGetBX()];
        long pos = cpuGetDX();
        if(cpuGetCX() >= 0x8000)
            pos = pos + (((long)cpuGetCX() - 0x10000) << 16);
        else
            pos = pos + (cpuGetCX() << 16);

        debug(debug_dos, "\tlseek-%02x pos = %ld\n", ax & 0xFF, pos);
        if(!f)
        {
            cpuSetFlag(cpuFlag_CF);
            cpuSetAX(6); // invalid handle
            break;
        }
        switch(ax & 0xFF)
        {
        case 0:
            fseek(f, pos, SEEK_SET);
            break;
        case 1:
            fseek(f, pos, SEEK_CUR);
            break;
        case 2:
            fseek(f, pos, SEEK_END);
            break;
        default:
            cpuSetFlag(cpuFlag_CF);
            cpuSetAX(1);
            return;
        }
        pos = ftell(f);
        cpuSetAX(pos & 0xFFFF);
        cpuSetDX((pos >> 16) & 0xFFFF);
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x43: // DOS 2+ file attributes
        int21_43();
        break;
    case 0x44:
    {
        int h = cpuGetBX();
        int al = ax & 0xFF;
        if((al < 4 || al == 6 || al == 7 || al == 10 || al == 12 || al == 16) &&
           !handles[h])
        {
            // Show error if it is a file handle.
            debug(debug_dos, "\t(invalid file handle)\n");
            cpuSetFlag(cpuFlag_CF);
            cpuSetAX(6); // invalid handle
            break;
        }
        cpuClrFlag(cpuFlag_CF);
        switch(al)
        {
        case 0x00: // GET DEV INFO
            debug(debug_dos, "\t= %04x\n", devinfo[h]);
            cpuSetDX(devinfo[h]);
            break;
        case 0x01: // SET DEV INFO
        case 0x02: // IOCTL CHAR DEV READ
        case 0x03: // IOCTL CHAR DEV WRITE
        case 0x04: // IOCTL BLOCK DEV READ
        case 0x05: // IOCTL BLOCK DEV |WRITE
            cpuSetAX(5);
            cpuSetFlag(cpuFlag_CF);
            break;
        case 0x06: // GET INPUT STATUS
            if(devinfo[h] == 0x80D3)
                cpuSetAX(kbhit() ? 0x44FF : 0x4400);
            else
                cpuSetAX(feof(handles[h]) ? 0x4400 : 0x44FF);
            break;
        case 0x07: // GET OUTPUT STATUS
            cpuSetAX(0x44FF);
            break;
        case 0x08: // CHECK DRIVE REMOVABLE
            h = h & 0xFF;
            h = h ? h - 1 : dos_get_default_drive();
            if(h < 2)
                cpuSetAX(0x0000);
            else
                cpuSetAX(0x0001);
            break;
        case 0x09: // CHECK DRIVE REMOTE
            cpuSetDX(0x0100);
            break;
        case 0x0A: // CHECK HANDLE REMOTE
            cpuSetDX(0);
            break;
        case 0x0B: // SET SHARING RETRY COUNT
        case 0x0C: // GENERIC CHAR DEV REQUEST
        case 0x0D: // GENERIC BLOCK DEV REQUEST
        case 0x0F: // SET LOGICAL DRIVE MAP
        case 0x10: // QUERY CHAR DEV CAPABILITY
        case 0x11: // QUERY BLOCK DEV CAPABILITY
            cpuSetAX(1);
            cpuSetFlag(cpuFlag_CF);
            break;
        case 0x0E: // GET LOGICAL DRIVE MAP
            cpuSetAX(0x4400);
        }
        break;
    }
    case 0x45:
    {
        if(!handles[cpuGetBX()])
        {
            // Show error if it is a file handle.
            debug(debug_dos, "\t(invalid file handle)\n");
            cpuSetFlag(cpuFlag_CF);
            cpuSetAX(6); // invalid handle
            break;
        }
        int h = get_new_handle();
        if(!h)
        {
            cpuSetAX(4);
            cpuSetFlag(cpuFlag_CF);
            break;
        }
        debug(debug_dos, "\t%04x -> %04x\n", cpuGetBX(), h);
        handles[h] = handles[cpuGetBX()];
        devinfo[h] = devinfo[cpuGetBX()];
        cpuSetAX(h);
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x46:
    {
        if(!handles[cpuGetBX()])
        {
            // Show error if it is a file handle.
            debug(debug_dos, "\t(invalid file handle)\n");
            cpuSetFlag(cpuFlag_CF);
            cpuSetAX(6); // invalid handle
            break;
        }
        if(handles[cpuGetCX()])
            dos_close_file(cpuGetCX());
        handles[cpuGetCX()] = handles[cpuGetBX()];
        devinfo[cpuGetCX()] = devinfo[cpuGetBX()];
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x47: // GET CWD
    {
        // Note: ignore drive letter in DL
        const uint8_t *path = dos_get_cwd(cpuGetDX() & 0xFF);
        debug(debug_dos, "\tcwd '%c' = '%s'\n", '@' + (cpuGetDX() & 0xFF), path);
        putmem(cpuGetAddrDS(cpuGetSI()), path, 64);
        cpuSetAX(0x0100);
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x48: // ALLOC MEMORY BLOCK
    {
        int seg, max = 0;
        seg = mem_alloc_segment(cpuGetBX(), &max);
        if(seg)
        {
            debug(debug_dos, "\tallocated at %04x.\n", seg);
            cpuSetAX(seg);
            cpuClrFlag(cpuFlag_CF);
        }
        else
        {
            debug(debug_dos, "\tnot enough memory, max=$%04x paragraphs\n", max);
            cpuSetAX(0x8);
            cpuSetBX(max);
            cpuSetFlag(cpuFlag_CF);
        }
        break;
    }
    case 0x49: // FREE MEMORY BLOCK
    {
        mem_free_segment(cpuGetES());
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x4A: // RESIZE MEMORY BLOCK
    {
        int sz;
        sz = mem_resize_segment(cpuGetES(), cpuGetBX());
        if(sz == cpuGetBX())
            cpuClrFlag(cpuFlag_CF);
        else
        {
            cpuSetAX(0x8);
            cpuSetBX(sz);
            cpuSetFlag(cpuFlag_CF);
            debug(debug_dos, "\tmax memory available: $%04x\n", sz);
        }
        break;
    }
    case 0x4B: // EXEC
    {
        char *fname = dos_unix_path(cpuGetAddrDS(cpuGetDX()), 0);
        if(!fname)
        {
            debug(debug_dos, "\texec error, file not found\n");
            cpuSetFlag(cpuFlag_CF);
            cpuSetAX(2);
            break;
        }
        // Flags:   0 = Load and Go, 1 = LOAD, 3 = Overlay
        //        128 = LoadHi
        if((ax & 0xFF) == 3)
        {
            debug(debug_dos, "\tload overlay '%s'\n", fname);
            int pb = cpuGetAddrES(cpuGetBX());
            uint16_t load_seg = get16(pb);
            uint16_t reloc_seg = get16(pb + 2);
            FILE *f = fopen(fname, "rb");
            if(!f || dos_read_overlay(f, load_seg, reloc_seg))
            {
                debug(debug_dos, "\tERROR\n");
                cpuSetFlag(cpuFlag_CF);
                cpuSetAX(11);
            }
            else
                cpuClrFlag(cpuFlag_CF);
        }
        else if((ax & 0xFF) == 0)
        {
            debug(debug_dos, "\texec: '%s'\n", fname);
            // Get executable file name:
            char *prgname = getstr(cpuGetAddrDS(cpuGetDX()), 64);
            // Read command line parameters:
            int pb = cpuGetAddrES(cpuGetBX());
            int cmd_addr = cpuGetAddress(get16(pb + 4), get16(pb + 2));
            int clen = memory[cmd_addr];
            char *cmdline = getstr(cmd_addr + 1, clen);
            debug(debug_dos, "\texec command line: '%s %.*s'\n", prgname, clen, cmdline);
            char *env = "\0\0";
            if(get16(pb) != 0)
            {
                // Sanitize env
                int eaddr = cpuGetAddress(get16(pb), 0);
                while(memory[eaddr] != 0 && eaddr < 0xFFFFF)
                {
                    while(memory[eaddr] != 0 && eaddr < 0xFFFFF)
                        eaddr++;
                    eaddr++;
                }
                if(eaddr < 0xFFFFF)
                    env = (char *)(memory + cpuGetAddress(get16(pb), 0));
            }
            if(run_emulator(fname, prgname, cmdline, env))
            {
                cpuSetAX(5); // access denied
                cpuSetFlag(cpuFlag_CF);
            }
            else
                cpuClrFlag(cpuFlag_CF);
        }
        else
        {
            debug(debug_dos, "\texec '%s': type %02xh not supported.\n", fname,
                  ax & 0xFF);
            cpuSetFlag(cpuFlag_CF);
            cpuSetAX(1);
        }
        free(fname);
        break;
    }
    case 0x4C: // EXIT
        // Detect if our PSP is last one
        debug(debug_dos, "\texit PSP:'%04x', PARENT:%04x.\n", get_current_PSP(),
              get16(cpuGetAddress(get_current_PSP(), 22)));
        if(0xFFFE == get16(cpuGetAddress(get_current_PSP(), 22)))
            exit(ax & 0xFF);
        else
        {
            // Exit to parent
            // TODO: we must close all child file descriptors and dealocate
            //       child memory.
            return_code = cpuGetAX() & 0xFF;
            // Patch INT 22h, 23h and 24h addresses to the ones saved in new PSP
            put16(0x88, get16(cpuGetAddress(get_current_PSP(), 10)));
            put16(0x8A, get16(cpuGetAddress(get_current_PSP(), 12)));
            put16(0x8C, get16(cpuGetAddress(get_current_PSP(), 14)));
            put16(0x8E, get16(cpuGetAddress(get_current_PSP(), 16)));
            put16(0x90, get16(cpuGetAddress(get_current_PSP(), 18)));
            put16(0x92, get16(cpuGetAddress(get_current_PSP(), 20)));
            // Set PSP to parent
            set_current_PSP(get16(cpuGetAddress(get_current_PSP(), 22)));
            // Get last stack
            cpuSetSS(get16(cpuGetAddress(get_current_PSP(), 0x30)));
            cpuSetSP(get16(cpuGetAddress(get_current_PSP(), 0x2E)));
            int stack = cpuGetAddress(cpuGetSS(), cpuGetSP());
            // Fixup interrupt return
            put16(stack, get16(0x22 * 4));
            put16(stack + 2, get16(0x22 * 4 + 2));
            put16(stack + 4, 0xf202);
            // And exit!
        }
        break;
    case 0x4D: // GET RETURN CODE (ERRORLEVEL)
        cpuSetAX(return_code);
        return_code = 0;
        cpuClrFlag(cpuFlag_CF);
        break;
    case 0x4E: // FIND FIRST MATCHING FILE
        dos_find_first();
        break;
    case 0x4F: // FIND NEXT MATCHING FILE
        dos_find_next(0);
        break;
    case 0x50: // SET CURRENT PSP
        set_current_PSP(cpuGetBX());
        break;
    case 0x51: // GET CURRENT PSP
        cpuSetBX(get_current_PSP());
        break;
    case 0x52: // GET SYSVARS
        cpuSetES(dos_sysvars >> 4);
        cpuSetBX((dos_sysvars & 0xF) + 24);
        break;
    case 0x55: // Create CHILD PSP
    {
        uint8_t *new_psp = getptr(cpuGetAddress(cpuGetDX(), 0), 0x100);
        uint8_t *orig_psp = getptr(cpuGetAddress(get_current_PSP(), 0), 0x100);
        if(!new_psp || !orig_psp)
        {
            debug(debug_dos, "\tinvalid new PSP segment %04x.\n", cpuGetDX());
            break;
        }
        // Copy PSP to the new segment, 0x80 is what DOS does - this excludes command line
        memcpy(new_psp, orig_psp, 0x80);
        // Set parent PSP to the current one
        new_psp[22] = get_current_PSP() & 0xFF;
        new_psp[23] = get_current_PSP() >> 8;
        set_current_PSP(cpuGetDX());
        break;
    }
    case 0x56: // RENAME
    {
        char *fname1 = dos_unix_path(cpuGetAddrDS(cpuGetDX()), 0);
        if(!fname1)
        {
            debug(debug_dos, "\t(file not found)\n");
            cpuSetAX(0x02);
            cpuSetFlag(cpuFlag_CF);
        }
        char *fname2 = dos_unix_path(cpuGetAddrES(cpuGetDI()), 1);
        debug(debug_dos, "\t'%s' -> '%s'\n", fname1, fname2);
        int e = rename(fname1, fname2);
        free(fname2);
        free(fname1);
        if(e)
        {
            cpuSetFlag(cpuFlag_CF);
            if(errno == ENOTDIR)
                cpuSetAX(0x03);
            else if(errno == ENOENT)
                cpuSetAX(0x02);
            else
                cpuSetAX(0x05);
        }
        else
            cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x57: // DATE/TIME
        int21_57();
        break;
    case 0x58:
    {
        uint8_t al = ax & 0xFF;
        if(0 == al)
            cpuSetAX(mem_get_alloc_strategy());
        else if(1 == al)
            mem_set_alloc_strategy(cpuGetBX());
        else if(3 == al)
        {
            cpuSetFlag(cpuFlag_CF);
            cpuSetAX(1);
        }
    }
    case 0x5B: // CREATE NEW FILE
        dos_open_file(2);
        break;
    case 0x62: // GET PSP SEGMENT
        cpuSetBX(get_current_PSP());
        break;
    case 0x65: // GET NLS DATA
    {
        uint32_t addr = cpuGetAddrES(cpuGetDI());
        cpuClrFlag(cpuFlag_CF);
        switch(ax & 0xFF)
        {
        case 1:
        {
            static const uint8_t data[] = {1, 38, 0, 1, 0, 181, 1};
            putmem(addr, data, 7);
            putmem(addr + 7, nls_country_info, 34);
            cpuSetCX(41);
            break;
        }
        case 2:
            memory[addr] = 2;
            put16(addr + 1, nls_uppercase_table & 0xF);
            put16(addr + 3, nls_uppercase_table >> 4);
            cpuSetCX(5);
            break;
        case 4:
            memory[addr] = 4;
            put16(addr + 1, nls_uppercase_table & 0xF);
            put16(addr + 3, nls_uppercase_table >> 4);
            cpuSetCX(5);
            break;
        case 5:
            memory[addr] = 5;
            put16(addr + 1, nls_terminator_table & 0xF);
            put16(addr + 3, nls_terminator_table >> 4);
            cpuSetCX(5);
            break;
        case 6:
            memory[addr] = 6;
            put16(addr + 1, nls_collating_table & 0xF);
            put16(addr + 3, nls_collating_table >> 4);
            cpuSetCX(5);
            break;
        case 7:
            memory[addr] = 7;
            put16(addr + 1, nls_dbc_set_table & 0xF);
            put16(addr + 3, nls_dbc_set_table >> 4);
            cpuSetCX(5);
            break;
        default:
            cpuSetAX(1);
            cpuSetFlag(cpuFlag_CF);
            break;
        }
        break;
    case 0x66: // GET GLOBAL CODE PAGE
        cpuSetBX(437);
        cpuSetDX(437);
        cpuClrFlag(cpuFlag_CF);
        break;
    case 0x67: // SET HANDLE COUNT
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    default:
        debug(debug_dos, "UNHANDLED INT 21, AX=%04x\n", cpuGetAX());
        debug(debug_int, "UNHANDLED INT 21, AX=%04x\n", cpuGetAX());
        cpuSetFlag(cpuFlag_CF);
        cpuSetAX(ax & 0xFF00);
    }
}

// DOS int 22 - TERMINATE ADDRESS
void int22()
{
    debug(debug_dos, "D-22: TERMINATE HANDLER CALLED\n");
    // If we reached here, we must terminate now
    exit(return_code & 0xFF);
}

static char *addstr(char *dst, const char *src, int limit)
{
    while(limit > 0 && *src)
    {
        *dst = *src;
        dst++;
        src++;
        limit--;
    }
    return dst;
}

// Initializes all NLS data for INT 21/38 and INT 21/65
static void init_nls_data(void)
{
    static const uint8_t uppercase_table[128] = {
        0x80,0x9A,0x45,0x41,0x8E,0x41,0x8F,0x80,0x45,0x45,0x45,0x49,0x49,0x49,0x8E,0x8F,
        0x90,0x92,0x92,0x4F,0x99,0x4F,0x55,0x55,0x59,0x99,0x9A,0x9B,0x9C,0x9D,0x9E,0x9F,
        0x41,0x49,0x4F,0x55,0xA5,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF,
        0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,
        0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,
        0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,
        0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,
        0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF,
    };
    static const uint8_t collating_table[256] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
        0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,
        0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,
        0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,
        0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x5b,0x5c,0x5d,0x5e,0x5f,
        0x60,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,
        0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x7b,0x7c,0x7d,0x7e,0x7f,
        0x43,0x55,0x45,0x41,0x41,0x41,0x41,0x43,0x45,0x45,0x45,0x49,0x49,0x49,0x41,0x41,
        0x45,0x41,0x41,0x4f,0x4f,0x4f,0x55,0x55,0x59,0x4f,0x55,0x24,0x24,0x24,0x24,0x24,
        0x41,0x49,0x4f,0x55,0x4e,0x4e,0xa6,0xa7,0x3f,0xa9,0xaa,0xab,0xac,0x21,0x22,0x22,
        0xb0,0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf,
        0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf,
        0xd0,0xd1,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xdb,0xdc,0xdd,0xde,0xdf,
        0xe0,0x53,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xeb,0xec,0xed,0xee,0xef,
        0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff
    };
    static const uint8_t terminator_table[24] = {
        0x16, 0x00, // size of table = 22 bytes
        0x01,       // ???
        0x00,       // lowest char in filename
        0xFF,       // highest char in filename
        0x00,       // ???
        0x00,       // first excluded char
        0x20,       // last excluded char (all from 00-20 excluded)
        0x02,       // ???
        0x0E,       // number of terminator characters
        0x2E, 0x22, 0x2F, 0x5C, 0x5B, 0x5D, 0x3A,
        0x7C, 0x3C, 0x3E, 0x2B, 0x3D, 0x3B, 0x2C};
    static const uint8_t fn_uppercase[16] = {
        0x3C, 0x80,       //     CMP    AL,80
        0x72, 0x0B,       //     JB     xit
        0x53,             //     PUSH   BX
        0x30, 0xFF,       //     XOR    BH,BH
        0x88, 0xC3,       //     MOV    BL,AL
        0x2E,             //     CS:
        0x8A, 0x87, 2, 0, //     MOV    AL,[BX+0002] ; offset of uppercase table
        0x5B,             //     POP    BX
        0xCB              // xit: RETF
    };
    static uint8_t country_info[34] = {
        1, 0,            // Date format
        '$', 0, 0, 0, 0, // Currency symbol string
        ',', 0,          // Thousands separator
        '.', 0,          // Decimal separator
        '-', 0,          // Date separator
        ':', 0,          // Time separator
        0,               // Currency format
        2,               // Digits after decimal in currency
        0,               // Time format
        0, 0, 0, 0,      // Uppercase function address - patched in code
        ',', 0,          // Data list separator
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    // Uppercase table
    nls_uppercase_table = get_static_memory(128 + 2 + 16, 0);
    put16(nls_uppercase_table, 128); // Length
    putmem(nls_uppercase_table + 2, uppercase_table, 128);
    // Uppercase function
    uint16_t fn_ucase_seg = nls_uppercase_table >> 4;
    uint16_t fn_ucase_off = (nls_uppercase_table & 0xF) + 128 + 2;
    putmem(nls_uppercase_table + 128 + 2, fn_uppercase, 16);

    // Country info
    country_info[18] = fn_ucase_off & 0xFF;
    country_info[19] = fn_ucase_off >> 8;
    country_info[20] = fn_ucase_seg & 0xFF;
    country_info[21] = fn_ucase_seg >> 8;
    nls_country_info = country_info;

    // Terminators table
    nls_terminator_table = get_static_memory(24, 0);
    putmem(nls_terminator_table, terminator_table, 24);

    // Collating table
    nls_collating_table = get_static_memory(256 + 2, 0);
    put16(nls_collating_table, 256); // Length
    putmem(nls_collating_table + 2, collating_table, 256);

    // Double-byte-chars table
    nls_dbc_set_table = get_static_memory(4, 0);
    put16(nls_dbc_set_table, 0); // Length
    put16(nls_dbc_set_table, 0); // one entry at least.
}

void init_dos(int argc, char **argv)
{
    char args[256], environ[4096];
    memset(args, 0, 256);
    memset(environ, 0, sizeof(environ));

    init_handles();
    init_codepage();
    init_nls_data();

    // Init INTERRUPT handlers - point to our own handlers
    for(int i = 0; i < 256; i++)
    {
        memory[i * 4 + 0] = i;
        memory[i * 4 + 1] = 0;
        memory[i * 4 + 2] = 0;
        memory[i * 4 + 3] = 0;
    }

    // Patch an INT 21 at address 0x000C0, this is for the CP/M emulation code
    memory[0x000C0] = 0xCD;
    memory[0x000C1] = 0x21;

    // Init memory handling - available start address at 0x800,
    // ending address at 0xA0000.
    // We limit here memory to less than 512K to fix some old programs
    // that check memori using "JLE" instead of "JBE".
    if(getenv(ENV_LOWMEM))
        mcb_init(0x80, 0x7FFF);
    else
        mcb_init(0x80, 0xA000);

    // Init SYSVARS
    dos_sysvars = get_static_memory(128, 0);
    put16(dos_sysvars + 22, 0x0080); // First MCB

    // Setup default drive
    if(getenv(ENV_DEF_DRIVE))
    {
        char c = getenv(ENV_DEF_DRIVE)[0];
        c = (c >= 'a') ? c - 'a' : c - 'A';
        if(c >= 0 && c <= 25)
        {
            dos_set_default_drive(c);
            debug(debug_dos, "set default drive = '%c'\n", c + 'A');
        }
    }

    // Setup CWD
    if(getenv(ENV_CWD))
    {
        char path[64];
        memset(path, 0, 64);
        strncpy(path, getenv(ENV_CWD), 63);
        dos_change_cwd(path);
    }
    else
    {
        // No CWD given, translate from base path of default drive
        char *cwd = dos_real_path(dos_get_default_drive(), ".");
        if(cwd)
        {
            dos_change_cwd(cwd);
            free(cwd);
        }
        else
            debug(debug_dos, "\tWARNING: working directory outside default drive\n");
    }

    // Concat rest of arguments
    int i;
    char *p = args;
    for(i = 1; i < argc && strcmp(argv[i], "--"); i++)
    {
        if(i != 1)
            p = addstr(p, " ", 127 - (p - args));
        p = addstr(p, argv[i], 127 - (p - args));
    }
    *p = 0;

    // Copy environment
    int have_path = 0;
    p = environ;
    for(i++; i < argc; i++)
    {
        if(!strncmp("PATH=", argv[i], 5) || !strcmp("PATH", argv[i]))
            have_path = 1;
        p = addstr(p, argv[i], environ + sizeof(environ) - 2 - p);
        *p = 0;
        p++;
    }
    if(!have_path)
    {
        // Adds a PATH variable.
        p = addstr(p, "PATH=C:\\", environ + sizeof(environ) - 2 - p);
        *p = 0;
        p++;
    }

    const char *progname = getenv(ENV_PROGNAME);
    if(!progname)
    {
        progname = dos_real_path(dos_get_default_drive(), argv[0]);
        if(!progname)
            progname = argv[0];
    }

    // Create main PSP
    int psp_mcb = create_PSP(args, environ, p - environ + 1, progname);

    // Load program
    const char *name = argv[0];
    FILE *f = fopen(name, "rb");
    if(!f)
        print_error("can't open '%s': %s\n", name, strerror(errno));
    if(!dos_load_exe(f, psp_mcb))
        print_error("error loading EXE/COM file.\n");
    fclose(f);

    // Init DTA
    dosDTA = get_current_PSP() * 16 + 0x80;

    // Init DOS flags
    cpuSetStartupFlag(cpuFlag_IF);
    cpuClrStartupFlag(cpuFlag_DF);
    cpuClrStartupFlag(cpuFlag_TF);
}

void int28(void)
{
    usleep(1); // TODO: process messages?
}
