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
/***
 * 
 object_del 和 lv_obj_clean 的区别

 这两段代码不一样，它们的功能和后果有重要区别：
    lv_obj_del(picture_manager_gui);
    picture_manager_gui = NULL;

    功能：完全删除对象及其所有子对象，释放内存
    效果：picture_manager_gui 对象不复存在
    用途：当不再需要整个对象时使用

    lv_obj_clean(picture_manager_gui);
    picture_manager_gui = NULL;  // 这里有问题！

功能：只删除对象的所有子对象，但保留对象本身
问题：将指针设为 NULL 后，你丢失了对原对象的引用，但对象本身还在内存中（内存泄漏）

正确用法：
lv_obj_clean(picture_manager_gui);
// picture_manager_gui 指针不变，对象本身仍然有效
// 之后可以继续往这个对象里添加新的子对象

最后强调：lv_obj_clean之后，千万不要设置picture_manager_gui = NULL;，因为这样会导致内存泄漏。
只有lv_obj_del才需要将指针设为NULL，以防止悬挂指针问题。
 */

// clean 53792 53780 53780 53760 53760 53756 53748 53740 53724
// del 53724 53572 53612 53604 53628 53584 53740 53768
void picture_manager_gui_init(void)
{ 

    // 下面这两行代码是为了清掉进入app前的LOGO残留，如果你不执行，那么再退出APP之后，原图标会留在那里。
    lv_obj_t *act_obj = lv_scr_act(); // 获取当前活动页
    lv_obj_clean(act_obj); // 清空此前页面


    // 静态对象，如果已经被初始化了，则清空，重新初始化一次
    // if(picture_manager_gui != NULL) picture_manager_gui_del();
    // picture_manager_gui = lv_obj_create(NULL); // 创建一个屏幕
    
    if(picture_manager_gui != NULL) picture_manager_gui_clean();
    else picture_manager_gui = lv_obj_create(NULL); // 创建一个屏幕

    // 初始化样式
    lv_style_init(&default_style); // 初始化基本样式 
    lv_style_set_bg_color(&default_style, lv_color_hex(0x000000)); // 设置背景颜色为黑色

    // 初始化标签样式
    lv_style_set_text_color(&label_style, lv_color_white()); // 设置标签文字颜色
    lv_style_set_text_font(&label_style, &lv_font_montserrat_20); // 设置标签字体

    lv_obj_add_style(picture_manager_gui, &default_style, LV_STATE_DEFAULT); // 应用样式到屏幕


    message_label = lv_label_create(picture_manager_gui); // 创建一个标签
    lv_obj_add_style(message_label, &label_style, LV_STATE_DEFAULT); // 应用样式到标签
    lv_obj_align(message_label, LV_ALIGN_CENTER, 0, -60); // 将标签居中对齐，向上偏移60像素

    ssid_label = lv_label_create(picture_manager_gui); // 创建一个标签
    lv_obj_add_style(ssid_label, &label_style, LV_STATE_DEFAULT); // 应用样式到标签
    lv_obj_align(ssid_label, LV_ALIGN_CENTER, 0, -30); // 将标签居中对齐，向上偏移30像素

    ip_label = lv_label_create(picture_manager_gui); // 创建一个标签
    lv_obj_add_style(ip_label, &label_style, LV_STATE_DEFAULT); // 应用样式到标签
    lv_obj_align(ip_label, LV_ALIGN_CENTER, 0, 0); // 将标签居中对齐


    hostname_label = lv_label_create(picture_manager_gui); // 创建一个标签
    lv_obj_add_style(hostname_label, &label_style, LV_STATE_DEFAULT); // 应用样式到标签
    lv_obj_align(hostname_label, LV_ALIGN_CENTER, 0, 30); // 将标签居中对齐，向下偏移30像素

    ap_label = lv_label_create(picture_manager_gui); // 创建一个标签
    lv_obj_add_style(ap_label, &label_style, LV_STATE_DEFAULT); // 应用样式到标签
    lv_obj_align(ap_label, LV_ALIGN_CENTER, 0, 60); // 将标签居中对齐，向上偏移60像素

    lv_scr_load(picture_manager_gui); // 加载屏幕
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
void picture_manager_gui_clean(){
    if (NULL != picture_manager_gui)
    {
        lv_obj_clean(picture_manager_gui); // 清空此前页面
        ip_label = NULL;
        ap_label = NULL;
        message_label = NULL;
        ssid_label = NULL;
        hostname_label = NULL;
    }
}
// 注意：在app_exit()中调用del会导致lv_obj_t *act_obj = lv_scr_act() 为空值
void picture_manager_gui_del(void)
{
    if (NULL != picture_manager_gui)
    {
        // 删除整个对象及其子对象， del之后必须设置null，防止指针悬挂；
        // 而clean之后不能设置null，因为clean只是清空子对象，父对象还在。如果设置null会导致内存泄漏。
        lv_obj_del(picture_manager_gui);  // 不知道为什么执行del，在app->exit()会报错，所以这个函数基本作废
        picture_manager_gui = NULL;
        ip_label = NULL;
        ap_label = NULL;
        message_label = NULL;
        ssid_label = NULL;
        hostname_label = NULL;
    }
    
    // 手动清除样式，防止内存泄漏
    // 不要执行这段代码，退出app的时候会白屏一下
    lv_style_reset(&default_style);
    lv_style_reset(&label_style);
}