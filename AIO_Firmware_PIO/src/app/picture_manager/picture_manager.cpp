#include "picture_manager.h"
#include "picture_manager_gui.h"
#include "sys/app_controller.h"
#include "common.h"
#include "picture_manager_server.h"

#define PICTURE_MANAGER_APP_NAME "PicManager"

// 动态数据，APP的生命周期结束也需要释放它
struct PictureManagerAppRunData
{
    boolean req_sent;
    boolean tcp_start;
    unsigned long apAlivePreMillis;       // 上一回更新的时间
};

// 常驻数据，可以不随APP的生命周期而释放或删除
struct PictureManagerAppForeverData
{
};

// 保存APP运行时的参数信息，理论上关闭APP时推荐在 xxx_exit_callback 中释放掉
static PictureManagerAppRunData *run_data = NULL;

// 当然你也可以添加恒定在内存中的少量变量（退出时不用释放，实现第二次启动时可以读取）
// 考虑到所有的APP公用内存，尽量减少 forever_data 的数据占用
static PictureManagerAppForeverData forever_data;

static int picture_manager_init(AppController *sys)
{
    // 初始化运行时的参数
    picture_manager_gui_init();
    // 初始化运行时参数
    run_data = (PictureManagerAppRunData *)calloc(1, sizeof(PictureManagerAppRunData));
    run_data->req_sent = false;
    run_data->tcp_start = false;
    // 使用 forever_data 中的变量，任何函数都可以用

    // 如果有需要持久化配置文件 可以调用此函数将数据存在flash中
    // 配置文件名最好以APP名为开头 以".cfg"结尾，以免多个APP读取混乱
    // char info[128] = {0};
    // uint16_t size = g_flashCfg.readFile("/picture_manager.cfg", (uint8_t *)info);
    // 解析数据
    // 将配置数据保存在文件中（持久化）
    // g_flashCfg.writeFile("/picture_manager.cfg", "value1=100\nvalue2=200");
    
    return 0;
}

static void picture_manager_process(AppController *sys,
                            const ImuAction *act_info)
{
    if (RETURN == act_info->active)
    {
        sys->app_exit(); // 退出APP
        return;
    }
    if(!run_data->req_sent) {
        // 发送请求。如果是wifi相关的消息，当请求完成后自动会调用 picture_manager_message_handle 函数
        sys->send_to(PICTURE_MANAGER_APP_NAME, CTRL_NAME,
                    APP_MESSAGE_WIFI_CONN,NULL, NULL);
        sys->send_to(PICTURE_MANAGER_APP_NAME, CTRL_NAME,
                    APP_MESSAGE_WIFI_AP, NULL, NULL);
        
        display_picture_manager_message("WiFi request send.");
        run_data->req_sent = true;
        delay(300);
    } 
    if(run_data->req_sent && !(run_data->tcp_start)) {
        display_picture_manager_message("waiting for WiFi");
    }
    if(run_data->req_sent && run_data->tcp_start) {
        picture_manager_server_handle();
        if (doDelayMillisTime(20000UL, &run_data->apAlivePreMillis, false))
        {
            display_picture_manager_message("Server Running");
            // 发送wifi维持的心跳
            sys->send_to(PICTURE_MANAGER_APP_NAME, CTRL_NAME,
                         APP_MESSAGE_WIFI_ALIVE, NULL, NULL);
        }
    }
    // 程序需要时可以适当加延时
    
}

static void picture_manager_background_task(AppController *sys,
                                    const ImuAction *act_info)
{
    // 本函数为后台任务，主控制器会间隔一分钟调用此函数
    // 本函数尽量只调用"常驻数据",其他变量可能会因为生命周期的缘故已经释放

    // 发送请求。如果是wifi相关的消息，当请求完成后自动会调用 picture_manager_message_handle 函数
    // sys->send_to(PICTURE_MANAGER_APP_NAME, CTRL_NAME,
    //              APP_MESSAGE_WIFI_CONN, (void *)run_data->val1, NULL);

    // 也可以移除自身的后台任务，放在本APP可控的地方最合适
    // sys->remove_backgroud_task();

    // 程序需要时可以适当加延时
    // delay(300);
}

static int picture_manager_exit_callback(void *param)
{

    stop_picture_manager_server();
    picture_manager_gui_clean();
    
    // 这里不能调用lv_obj_del函数，否则会再sys->exit()代码中会报错！原因未知！

    // 释放资源
    if (NULL != run_data)
    {
        free(run_data);
        run_data = NULL;
    }
    return 0;
}
static void start_server() {
    display_picture_manager_message("Starting server...");
    start_picture_manager_server();
    display_picture_manager_message("Server started.");
    run_data->tcp_start = true;
}

static void picture_manager_message_handle(const char *from, const char *to,
                                   APP_MESSAGE_TYPE type, void *message,
                                   void *ext_info)
{
    // 目前主要是wifi开关类事件（用于功耗控制）
    switch (type)
    {
    case APP_MESSAGE_WIFI_CONN:
    {
        // todo
        start_server();
        Serial.println("图片服务器启动");
        display_picture_manager_ssid_label(WiFi.SSID().c_str());
        String ip_str = WiFi.localIP().toString() + ":81";
        display_picture_manager_ip_label(ip_str.c_str());
    }
    break;
    case APP_MESSAGE_WIFI_AP:
    {
        // todo
        Serial.println("热点已经启动");
        start_server();
        display_picture_manager_hostname_label(WiFi.getHostname());
        String ap_label_text = WiFi.softAPIP().toString() + ":81";
        display_picture_manager_ap_label(ap_label_text.c_str());
    }
    break;
    case APP_MESSAGE_WIFI_ALIVE:
    {
        // wifi心跳维持的响应 可以不做任何处理
    }
    break;
    case APP_MESSAGE_GET_PARAM:
    {
        char *param_key = (char *)message;
    }
    break;
    case APP_MESSAGE_SET_PARAM:
    {
        char *param_key = (char *)message;
        char *param_val = (char *)ext_info;
    }
    break;
    default:
        break;
    }
}

APP_OBJ picture_manager_app = {PICTURE_MANAGER_APP_NAME, &app_picture_manager, "Author tignioj\nVersion 0.0.1\n",
                       picture_manager_init, picture_manager_process, picture_manager_background_task,
                       picture_manager_exit_callback, picture_manager_message_handle};
