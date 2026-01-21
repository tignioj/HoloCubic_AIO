#ifndef APP_PICTURE_MANAGER_GUI_H
#define APP_PICTURE_MANAGER_GUI_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "lvgl.h"
#define ANIEND                      \
    while (lv_anim_count_running()) \
        lv_task_handler(); //等待动画完成

    void picture_manager_gui_init(void);
    void display_picture_manager_message(const char *message_label_text);
    void display_picture_manager_ssid_label(const char *ssid_label_text);
    void display_picture_manager_hostname_label(const char *hostname_label_text);
    void display_picture_manager_ap_label(const char *ap_label_text);
    void display_picture_manager_ip_label(const char *ip_label_text);
    void picture_manager_gui_del(void);

#ifdef __cplusplus
} /* extern "C" */
#endif


#ifdef __cplusplus
extern "C"
{
#endif

#include "lvgl.h"
    extern const lv_img_dsc_t app_picture_manager;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif