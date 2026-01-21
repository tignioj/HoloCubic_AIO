#include "picture_manager_gui.h"

#include "lvgl.h"

static lv_obj_t *picture_manager_gui = NULL;
static lv_obj_t *message_label = NULL;
static lv_obj_t *ssid_label = NULL;
static lv_obj_t *hostname_label = NULL;
static lv_obj_t *ip_label = NULL;
static lv_obj_t *ap_label = NULL;

static lv_style_t default_style;
static lv_style_t label_style;

void picture_manager_gui_init(void)
{ 

    lv_obj_t *act_obj = lv_scr_act(); // 获取当前活动页
    lv_obj_clean(act_obj); // 清空此前页面
    // 静态对象，如果已经被初始化了，则清空，重新初始化一次
    if(NULL != picture_manager_gui) {
        lv_obj_clean(picture_manager_gui);
        picture_manager_gui = NULL;
        ip_label = NULL;
        ap_label = NULL;
        message_label = NULL;
        ssid_label = NULL;
        hostname_label = NULL;
    }

    picture_manager_gui = lv_obj_create(NULL); // 创建一个屏幕
    lv_style_init(&default_style); // 初始化基本样式 
    lv_style_set_bg_color(&default_style, lv_color_hex(0x000000)); // 设置背景颜色为黑色
    lv_obj_add_style(picture_manager_gui, &default_style, LV_STATE_DEFAULT); // 应用样式到屏幕

    message_label = lv_label_create(picture_manager_gui); // 创建一个标签
    lv_style_set_text_color(&label_style, lv_color_white()); // 设置标签文字颜色
    lv_style_set_text_font(&label_style, &lv_font_montserrat_20); // 设置标签字体
    lv_obj_add_style(message_label, &label_style, LV_STATE_DEFAULT); // 应用样式到标签
    lv_obj_align(message_label, LV_ALIGN_CENTER, 0, -60); // 将标签居中对齐，向上偏移60像素

    ssid_label = lv_label_create(picture_manager_gui); // 创建一个标签
    lv_style_set_text_color(&label_style, lv_color_white()); // 设置标签文字颜色为白色
    lv_style_set_text_font(&label_style, &lv_font_montserrat_20); // 设置标签字体
    lv_obj_add_style(ssid_label, &label_style, LV_STATE_DEFAULT); // 应用样式到标签
    lv_obj_align(ssid_label, LV_ALIGN_CENTER, 0, -30); // 将标签居中对齐，向上偏移30像素

    ip_label = lv_label_create(picture_manager_gui); // 创建一个标签
    lv_style_set_text_color(&label_style, lv_color_white()); // 设置标签文字颜色为白色
    lv_style_set_text_font(&label_style, &lv_font_montserrat_20); // 设置标签字体
    lv_obj_add_style(ip_label, &label_style, LV_STATE_DEFAULT); // 应用样式到标签
    lv_obj_align(ip_label, LV_ALIGN_CENTER, 0, 0); // 将标签居中对齐


    hostname_label = lv_label_create(picture_manager_gui); // 创建一个标签
    lv_style_set_text_color(&label_style, lv_color_white()); // 设置标签文字颜色为白色
    lv_style_set_text_font(&label_style, &lv_font_montserrat_20);
    lv_obj_add_style(hostname_label, &label_style, LV_STATE_DEFAULT); // 应用样式到标签
    lv_obj_align(hostname_label, LV_ALIGN_CENTER, 0, 30); // 将标签居中对齐，向下偏移30像素

    ap_label = lv_label_create(picture_manager_gui); // 创建一个标签
    lv_style_set_text_color(&label_style, lv_color_white()); // 设置标签文字颜色
    lv_style_set_text_font(&label_style, &lv_font_montserrat_20); // 设置标签字体
    lv_obj_add_style(ap_label, &label_style, LV_STATE_DEFAULT); // 应用样式到标签
    lv_obj_align(ap_label, LV_ALIGN_CENTER, 0, 60); // 将标签居中对齐，向上偏移60像素

    lv_scr_load(picture_manager_gui); // 加载屏幕
}

void display_picture_manager_init() {
    // lv_obj_t *act_obj = lv_scr_act(); // 获取当前活动页
    // if (act_obj == picture_manager_gui)
    //     return;
}



void display_picture_manager_ssid_label(const char *ssid_label_text)
{
    if(NULL != ssid_label_text) lv_label_set_text_fmt(ssid_label, "%s", ssid_label_text);
}

void display_picture_manager_hostname_label(const char *hostname_label_text)
{
    if(NULL != hostname_label_text) lv_label_set_text_fmt(hostname_label, "%s", hostname_label_text);
}


void display_picture_manager_message(const char *message_label_text)
{
    if(NULL != message_label_text) lv_label_set_text_fmt(message_label, "%s", message_label_text);
}
void display_picture_manager_ap_label(const char *ap_label_text)
{
    if(NULL != ap_label_text) lv_label_set_text_fmt(ap_label, "%s", ap_label_text);
}
void display_picture_manager_ip_label(const char *ip_label_text)
{
    if(NULL != ip_label_text) lv_label_set_text_fmt(ip_label, "%s", ip_label_text);
}


void picture_manager_gui_del(void)
{
    if (NULL != picture_manager_gui)
    {
        lv_obj_clean(picture_manager_gui);
        picture_manager_gui = NULL;
        ip_label = NULL;
        ap_label = NULL;
        message_label = NULL;
        ssid_label = NULL;
        hostname_label = NULL;
    }
    
    // 手动清除样式，防止内存泄漏
    // lv_style_reset(&default_style);
}