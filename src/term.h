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

