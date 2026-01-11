#include "myexample_gui.h"

#include "lvgl.h"

lv_obj_t *myexample_gui = NULL;

static lv_style_t default_style1;
static lv_style_t label_style1;

void myexample_gui_init(void)
{ 
    lv_style_init(&default_style1);
}

/*
 * 其他函数请根据需要添加
 */

void display_myexample(const char *file_name, lv_scr_load_anim_t anim_type)
{
    lv_obj_t *act_obj = lv_scr_act(); // 获取当前活动页
    if (act_obj == myexample_gui)
        return;

    myexample_gui_del(); // 清空对象
    lv_obj_clean(act_obj); // 清空此前页面

    // 创建屏幕
    myexample_gui = lv_obj_create(NULL);
    
    // 创建标签对象
    lv_obj_t *label = lv_label_create(myexample_gui);
    
    // 设置标签文本
    lv_label_set_text(label, "hello myexample");
    
    // 设置标签位置为屏幕中心
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    
    // 加载屏幕
    //lv_scr_load_anim(myexample_gui, anim_type, 500, 0, true); // 千万别调用这行，退出app会卡重启
    lv_scr_load(myexample_gui);
}

void myexample_gui_del(void)
{
    if (NULL != myexample_gui)
    {
        lv_obj_clean(myexample_gui);
        myexample_gui = NULL;
    }
    
    // 手动清除样式，防止内存泄漏
    // lv_style_reset(&default_style);
}