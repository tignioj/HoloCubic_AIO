#include "screen_share_udp_gui.h"
#include "lvgl.h"

static lv_obj_t *share_main_scr = NULL;

static lv_obj_t *title_label;
static lv_obj_t *local_ip_label;
static lv_obj_t *local_port_label;
static lv_obj_t *info_label;

static lv_style_t default_style;
static lv_style_t label_style;

LV_FONT_DECLARE(lv_font_montserrat_24);

void screen_share_udp_gui_init(void)
{
    lv_obj_t *act_obj = lv_scr_act();
    lv_obj_clean(act_obj);

    if (share_main_scr != NULL)
        screen_share_gui_del();
    else
        share_main_scr = lv_obj_create(NULL);

    /* ================= 样式 ================= */

    lv_style_init(&default_style);
    lv_style_set_bg_color(&default_style, lv_color_hex(0x000000));

    lv_style_init(&label_style);
    lv_style_set_text_opa(&label_style, LV_OPA_COVER);
    lv_style_set_text_color(&label_style, lv_color_white());
    lv_style_set_text_font(&label_style, &lv_font_montserrat_24);

    lv_obj_add_style(share_main_scr, &default_style, 0);

    /* ================= 标题 ================= */

    title_label = lv_label_create(share_main_scr);
    lv_obj_add_style(title_label, &label_style, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);

    /* ================= 信息面板（核心） ================= */

    /* 用一个容器承载 IP / Port / Info，但不新增全局变量 */
    lv_obj_t *info_panel = lv_obj_create(share_main_scr);

    lv_coord_t panel_w = lv_obj_get_width(share_main_scr) - 10;

    lv_obj_set_width(info_panel, panel_w);
    lv_obj_align(info_panel, LV_ALIGN_BOTTOM_LEFT, 5, -20);

    /* 面板自身透明 */
    lv_obj_set_style_bg_opa(info_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(info_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(info_panel, 0, 0);
    lv_obj_set_style_pad_row(info_panel, 6, 0);

    /* 关键：纵向自动排版 */
    lv_obj_set_flex_flow(info_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        info_panel,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START
    );

    /* ================= IP ================= */

    local_ip_label = lv_label_create(info_panel);
    lv_obj_add_style(local_ip_label, &label_style, 0);
    lv_label_set_text(local_ip_label, "IP: -");

    /* ================= Port ================= */

    local_port_label = lv_label_create(info_panel);
    lv_obj_add_style(local_port_label, &label_style, 0);
    lv_label_set_text(local_port_label, "Port: -");

    /* ================= Info（自动换行） ================= */

    info_label = lv_label_create(info_panel);
    lv_obj_add_style(info_label, &label_style, 0);

    lv_obj_set_width(info_label, panel_w);
    lv_label_set_long_mode(info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(info_label, LV_TEXT_ALIGN_LEFT, 0);

    lv_label_set_text(info_label, "Status: -");

    lv_scr_load(share_main_scr);
}

void display_share_udp_init(void)
{
    /* 保持空实现 */
}

void display_screen_share_udp(const char *title,
                          const char *ip,
                          const char *port,
                          const char *info,
                          lv_scr_load_anim_t anim_type)
{
    lv_label_set_text(title_label, title);

    lv_label_set_text_fmt(local_ip_label, "IP: %s", ip);
    lv_label_set_text_fmt(local_port_label, "Port: %s", port);
    lv_label_set_text_fmt(info_label, "Status: %s", info);
}

void screen_share_udp_gui_del(void)
{
    if (share_main_scr != NULL)
    {
        lv_obj_clean(share_main_scr);

        title_label = NULL;
        local_ip_label = NULL;
        local_port_label = NULL;
        info_label = NULL;
    }
}
