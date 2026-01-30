#ifndef APP_SCREEN_SHARE_UDP_GUI_H
#define APP_SCREEN_SHARE_UDP_GUI_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "lvgl.h"
    extern const lv_img_dsc_t app_screen_share_udp;

    void screen_share_udp_gui_init(void);
    void display_share_udp_init(void);
    void display_screen_share_udp(const char *title, const char *ip,
                              const char *port, const char *info,
                              lv_scr_load_anim_t anim_type);
    void screen_share_udp_gui_del(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif