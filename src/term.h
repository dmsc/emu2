#pragma once

void b8_to_term();
void init_video2();
int video_ready(void);
void init_bi();
void int10();
void int10_t();
void int10_c();

void xx_addch(uint16_t x);
uint16_t xx_inch(void);
void xx_putchar(uint16_t x, int easy);
int xx_refresh(void);
int easy_switch(int ch, int easy);

void b_putchar(int page, uint16_t ch, int easy);
uint16_t b_get_cursor(uint8_t page);
uint8_t b_get_page(void);

extern int flag_s, flag_c, flag_C;


#pragma pack(1)
typedef struct bios_info    // bios-data (256 bytes)
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

//extern bios_info *bi;
