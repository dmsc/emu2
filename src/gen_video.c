#include <stdint.h>
#include <stdio.h>
#include "term.h"
#include "video.h"
#include "dbg.h"

void int10(void)
{
    if(flag_c) int10_t();
    else if(flag_C) int10_c();
    else if(flag_s) int10_t();
    else int10_v();
}
int video_active(void)
{
    if(flag_c || flag_C || flag_s) 
        return 1;
    else return video_active_v();
}
void video_putch(char ch)
{
    if(!flag_c && !flag_C && !flag_s)
    {
        video_putch_v(ch);
        return;
    }
    init_bi();
    int page=b_get_page();
    if(video_ready()){
        if(flag_C)
        {
            xx_putchar(0x700|(ch&0xff), 0);
            xx_refresh();
        }
        else if(flag_c)
        {
            b_putchar(page, 0x0700|ch, 0);
            b8_to_term();
        }
        if(flag_s)
        {
            b_putchar(page, 0x0700|ch, 0);
            putchar(ch);
        }
    }
    else
    {
        b_putchar(page, 0x0700|ch, 0);
        putchar(ch);
    }
}
int video_get_col(void)
{
    if(!flag_c && !flag_C & !flag_s)
        return video_get_col_v();
    init_bi();
    int cursor=b_get_cursor(b_get_page());
    return cursor&0xff;
}
void check_screen(void)
{
    //if(flag_s == 0)
    if(flag_s) return;
    else if(flag_C) return;
    else if(flag_c) b8_to_term();
    else check_screen_v();
}


/* crtc emulation copied from "video.c"
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
