#include "app_controller.h"
#include "app_controller_gui.h"
#include "common.h"
#include "interface.h"
#include "Arduino.h"

const char *app_event_type_info[] = {"APP_MESSAGE_WIFI_CONN", "APP_MESSAGE_WIFI_AP",
                                     "APP_MESSAGE_WIFI_ALIVE", "APP_MESSAGE_WIFI_DISCONN",
                                     "APP_MESSAGE_UPDATE_TIME", "APP_MESSAGE_MQTT_DATA",
                                     "APP_MESSAGE_WIFI_CONN_FAILED", "APP_MESSAGE_WIFI_AP_FAILED",
                                     "APP_MESSAGE_GET_PARAM", "APP_MESSAGE_SET_PARAM",
                                     "APP_MESSAGE_READ_CFG", "APP_MESSAGE_WRITE_CFG",
                                     "APP_MESSAGE_NONE"};

volatile static bool isRunEventDeal = false;

// TickType_t mainFormRefreshLastTime;
// const TickType_t xDelay500ms = pdMS_TO_TICKS(500);
// mainFormRefreshLastTime = xTaskGetTickCount();
// vTaskDelayUntil(&mainFormRefreshLastTime, xDelay500ms);

void eventDealHandle(TimerHandle_t xTimer)
{
    isRunEventDeal = true;
}

AppController::AppController(const char *name)
{
    strncpy(this->name, name, APP_CONTROLLER_NAME_LEN);
    app_num = 0;
    app_exit_flag = 0;
    cur_app_index = 0;
    pre_app_index = 0;
    // appList = new APP_OBJ[APP_MAX_NUM];
    m_wifi_status = false;
    m_sta_connecting = false;
    m_preWifiReqMillis = GET_SYS_MILLIS();
    m_eventListMutex = xSemaphoreCreateMutex();
    if (NULL == m_eventListMutex)
    {
        Serial.println(F("[EVENT] Failed to create event-list mutex"));
    }

    // 定义一个事件处理定时器
    xTimerEventDeal = xTimerCreate("Event Deal",
                                   300 / portTICK_PERIOD_MS,
                                   pdTRUE, (void *)0, eventDealHandle);
    // 启动事件处理定时器
    xTimerStart(xTimerEventDeal, 0);
}

void AppController::init(void)
{
    // 把这里低功耗注释掉了，加快开机速度。也避免开机时如果启动WiFi同时收到UDP数据包而重启的问题。
    // 设置CPU主频
    // if (1 == this->sys_cfg.power_mode)
    // {
    //     setCpuFrequencyMhz(240);
    // }
    // else
    // {
    //     setCpuFrequencyMhz(160);
    // }
    // uint32_t freq = getXtalFrequencyMhz(); // In MHz
    Serial.print(F("CpuFrequencyMhz: "));
    Serial.println(getCpuFrequencyMhz());

    app_control_gui_init();
    appList[0] = new APP_OBJ();
    appList[0]->app_image = &app_loading;
    appList[0]->app_name = "Loading...";
    appTypeList[0] = APP_TYPE_REAL_TIME;
    app_control_display_scr(appList[cur_app_index]->app_image,
                            appList[cur_app_index]->app_name,
                            LV_SCR_LOAD_ANIM_NONE, true);
    // Display();
}

void AppController::Display()
{
    // appList[0].app_image = &app_loading;
    app_control_display_scr(appList[cur_app_index]->app_image,
                            appList[cur_app_index]->app_name,
                            LV_SCR_LOAD_ANIM_NONE, true);
}

AppController::~AppController()
{
    if (NULL != xTimerEventDeal)
    {
        xTimerStop(xTimerEventDeal, 0);
        xTimerDelete(xTimerEventDeal, 0);
    }
    if (NULL != m_eventListMutex)
    {
        vSemaphoreDelete(m_eventListMutex);
    }
    rgb_stop();
}

int AppController::app_is_legal(const APP_OBJ *app_obj)
{
    // APP的合法性检测
    if (NULL == app_obj)
        return 1;
    if (APP_MAX_NUM <= app_num)
        return 2;
    return 0;
}

// 将APP安装到app_controller中
int AppController::app_install(APP_OBJ *app, APP_TYPE app_type)
{
    int ret_code = app_is_legal(app);
    if (0 != ret_code)
    {
        return ret_code;
    }

    appList[app_num] = app;
    appTypeList[app_num] = app_type;
    ++app_num;
    return 0; // 安装成功
}

// 将APP的后台任务从任务队列中移除(自能通过APP退出的时候，移除自身的后台任务)
int AppController::remove_backgroud_task(void)
{
    return 0; // 安装成功
}

// 将APP从app_controller中卸载（删除）
int AppController::app_uninstall(const APP_OBJ *app)
{
    // todo
    return 0;
}

int AppController::app_auto_start()
{
    // APP自启动
    int index = this->getAppIdxByName(sys_cfg.auto_start_app.c_str());
    if (index < 0)
    {
        // 没找到相关的APP
        return 0;
    }
    // 进入自启动的APP
    app_exit_flag = 1; // 进入app, 如果已经在
    cur_app_index = index;
    (*(appList[cur_app_index]->app_init))(this); // 执行APP初始化
    return 0;
}

int AppController::main_process(ImuAction *act_info)
{
    if (ACTIVE_TYPE::UNKNOWN != act_info->active)
    {
        Serial.print(F("[Operate]\tact_info->active: "));
        Serial.println(active_type_info[act_info->active]);
    }

    if (isRunEventDeal)
    {
        isRunEventDeal = false;
        // 扫描事件
        this->req_event_deal();
    }

    // wifi自动关闭(在节能模式下)
    if (0 == sys_cfg.power_mode && true == m_wifi_status && doDelayMillisTime(WIFI_LIFE_CYCLE, &m_preWifiReqMillis, false))
    {
        send_to(CTRL_NAME, CTRL_NAME, APP_MESSAGE_WIFI_DISCONN, 0, NULL);
    }

    if (0 == app_exit_flag)
    {
        // 当前没有进入任何app
        lv_scr_load_anim_t anim_type = LV_SCR_LOAD_ANIM_NONE;
        if (ACTIVE_TYPE::TURN_LEFT == act_info->active)
        {
            anim_type = LV_SCR_LOAD_ANIM_MOVE_RIGHT;
            pre_app_index = cur_app_index;
            cur_app_index = (cur_app_index + 1) % app_num;
            Serial.println(String("Current App: ") + appList[cur_app_index]->app_name);
        }
        else if (ACTIVE_TYPE::TURN_RIGHT == act_info->active)
        {
            anim_type = LV_SCR_LOAD_ANIM_MOVE_LEFT;
            pre_app_index = cur_app_index;
            // 以下等效与 processId = (processId - 1 + APP_NUM) % 4;
            // +3为了不让数据溢出成负数，而导致取模逻辑错误
            cur_app_index = (cur_app_index - 1 + app_num) % app_num; // 此处的3与p_processList的长度一致
            Serial.println(String("Current App: ") + appList[cur_app_index]->app_name);
        }
        else if (ACTIVE_TYPE::GO_FORWORD == act_info->active)
        {
            app_exit_flag = 1; // 进入app
            if (NULL != appList[cur_app_index]->app_init)
            {
                (*(appList[cur_app_index]->app_init))(this); // 执行APP初始化
            }
        }

        if (ACTIVE_TYPE::GO_FORWORD != act_info->active) // && UNKNOWN != act_info->active
        {
            app_control_display_scr(appList[cur_app_index]->app_image,
                                    appList[cur_app_index]->app_name,
                                    anim_type, false);
            vTaskDelay(200 / portTICK_PERIOD_MS);
        }
    }
    else
    {
        app_control_display_scr(appList[cur_app_index]->app_image,
                                appList[cur_app_index]->app_name,
                                LV_SCR_LOAD_ANIM_NONE, false);
        // 运行APP进程 等效于把控制权交给当前APP
        (*(appList[cur_app_index]->main_process))(this, act_info);
    }
    act_info->active = ACTIVE_TYPE::UNKNOWN;
    act_info->isValid = 0;
    return 0;
}

APP_OBJ *AppController::getAppByName(const char *name)
{
    for (int pos = 0; pos < app_num; ++pos)
    {
        if (!strcmp(name, appList[pos]->app_name))
        {
            return appList[pos];
        }
    }

    return NULL;
}

int AppController::getAppIdxByName(const char *name)
{
    for (int pos = 0; pos < app_num; ++pos)
    {
        if (!strcmp(name, appList[pos]->app_name))
        {
            return pos;
        }
    }

    return -1;
}

// 通信中心（消息转发）
int AppController::send_to(const char *from, const char *to,
                           APP_MESSAGE_TYPE type, void *message,
                           void *ext_info)
{
    APP_OBJ *fromApp = getAppByName(from); // 来自谁 有可能为空
    APP_OBJ *toApp = getAppByName(to);     // 发送给谁 有可能为空
    if (type <= APP_MESSAGE_MQTT_DATA)
    {
        if (NULL == m_eventListMutex || pdTRUE != xSemaphoreTake(m_eventListMutex, portMAX_DELAY))
        {
            return 1;
        }
        // 更新事件的请求者
        if (eventList.size() >= EVENT_LIST_MAX_LENGTH)
        {
            xSemaphoreGive(m_eventListMutex);
            return 1;
        }
        // 发给控制器的消息(目前都是wifi事件)
        EVENT_OBJ new_event = {fromApp, type, message, CONN_ERR_TIMEOUT + 1, 0,
                               GET_SYS_MILLIS()};
        eventList.push_back(new_event);
        Serial.print("[EVENT]\tAdd -> " + String(app_event_type_info[type]));
        Serial.print(F("\tEventList Size: "));
        Serial.println(eventList.size());
        xSemaphoreGive(m_eventListMutex);
    }
    else
    {
        // 各个APP之间通信的消息
        if (NULL != toApp)
        {
            Serial.print("[Massage]\tFrom " + String(fromApp->app_name) + "\tTo " + String(toApp->app_name) + "\n");
            if (NULL != toApp->message_handle)
            {
                toApp->message_handle(from, to, type, message, ext_info);
            }
        }
        else if (!strcmp(to, CTRL_NAME))
        {
            Serial.print("[Massage]\tFrom " + String(fromApp->app_name) + "\tTo " + CTRL_NAME + "\n");
            deal_config(type, (const char *)message, (char *)ext_info);
        }
    }
    return 0;
}

int AppController::req_event_deal(void)
{
    // 先在锁内取出一个到期事件，WiFi操作和APP回调在锁外执行。
    // 这样TimeSync等独立任务可以安全地调用send_to，也不会长时间占用互斥锁。
    for (;;)
    {
        if (NULL == m_eventListMutex || pdTRUE != xSemaphoreTake(m_eventListMutex, portMAX_DELAY))
        {
            return 1;
        }

        std::list<EVENT_OBJ>::iterator event = eventList.begin();
        const unsigned long now = GET_SYS_MILLIS();
        while (event != eventList.end() && (long)((*event).nextRunTime - now) > 0)
        {
            ++event;
        }
        if (event == eventList.end())
        {
            xSemaphoreGive(m_eventListMutex);
            break;
        }

        EVENT_OBJ currentEvent = *event;
        eventList.erase(event);
        xSemaphoreGive(m_eventListMutex);

        bool ret = wifi_event(currentEvent.type);
        if (false == ret)
        {
            currentEvent.retryCount += 1;
            if (currentEvent.retryCount >= currentEvent.retryMaxNum)
            {
                APP_MESSAGE_TYPE failureType = APP_MESSAGE_NONE;
                if (APP_MESSAGE_WIFI_CONN == currentEvent.type)
                {
                    failureType = APP_MESSAGE_WIFI_CONN_FAILED;
                    m_sta_connecting = false;
                }
                else if (APP_MESSAGE_WIFI_AP == currentEvent.type)
                {
                    failureType = APP_MESSAGE_WIFI_AP_FAILED;
                }

                Serial.println("[EVENT]\tFailed -> " + String(app_event_type_info[currentEvent.type]));
                if (APP_MESSAGE_NONE != failureType && NULL != currentEvent.from &&
                    NULL != currentEvent.from->message_handle)
                {
                    currentEvent.from->message_handle(CTRL_NAME, currentEvent.from->app_name,
                                                      failureType, currentEvent.info, NULL);
                }
            }
            else
            {
                currentEvent.nextRunTime = GET_SYS_MILLIS() + EVENT_RETRY_INTERVAL;
                if (pdTRUE == xSemaphoreTake(m_eventListMutex, portMAX_DELAY))
                {
                    eventList.push_back(currentEvent);
                    xSemaphoreGive(m_eventListMutex);
                }
            }
            continue;
        }
        // 事件回调
        if (NULL != currentEvent.from && NULL != currentEvent.from->message_handle)
        {
            currentEvent.from->message_handle(CTRL_NAME, currentEvent.from->app_name,
                                              currentEvent.type, currentEvent.info, NULL);
        }
        Serial.println("[EVENT]\tComplete -> " + String(app_event_type_info[currentEvent.type]));
    }
    return 0;
}

/**
 *  wifi事件的处理
 *  事件处理成功返回true 否则false
 * */
bool AppController::wifi_event(APP_MESSAGE_TYPE type)
{
    switch (type)
    {
    case APP_MESSAGE_WIFI_CONN:
    {
        const bool staConnected =
            ((WiFi.getMode() & WIFI_MODE_STA) == WIFI_MODE_STA) &&
            (WiFi.status() == WL_CONNECTED) &&
            (WiFi.localIP() != IPAddress(0, 0, 0, 0));

        if (staConnected)
        {
            m_wifi_status = true;
            m_sta_connecting = false;
            m_preWifiReqMillis = GET_SYS_MILLIS();
            return true;
        }

        if ((WiFi.getMode() & WIFI_MODE_STA) != WIFI_MODE_STA)
        {
            m_sta_connecting = false;
        }
        if (!m_sta_connecting)
        {
            m_sta_connecting = g_network.start_conn_wifi(sys_cfg.ssid_0.c_str(),
                                                         sys_cfg.password_0.c_str());
        }

        m_wifi_status = (WiFi.getMode() != WIFI_MODE_NULL);
        m_preWifiReqMillis = GET_SYS_MILLIS();
        return false;
    }
    break;
    case APP_MESSAGE_WIFI_AP:
    {
        // 更新请求
        if (!g_network.open_ap(AP_SSID))
        {
            return false;
        }
        m_wifi_status = (WiFi.getMode() != WIFI_MODE_NULL);
        m_preWifiReqMillis = GET_SYS_MILLIS();
    }
    break;
    case APP_MESSAGE_WIFI_ALIVE:
    {
        // wifi开关的心跳 持续收到心跳 wifi才不会被关闭
        if (WiFi.getMode() == WIFI_MODE_NULL)
        {
            m_wifi_status = false;
            return false;
        }
        m_wifi_status = true;
        // 更新请求
        m_preWifiReqMillis = GET_SYS_MILLIS();
    }
    break;
    case APP_MESSAGE_WIFI_DISCONN:
    {
        if (!g_network.close_wifi())
        {
            m_wifi_status = (WiFi.getMode() != WIFI_MODE_NULL);
            return false;
        }
        m_wifi_status = false;
        m_sta_connecting = false;
        // m_preWifiReqMillis = GET_SYS_MILLIS() - WIFI_LIFE_CYCLE;
    }
    break;
    case APP_MESSAGE_UPDATE_TIME:
    {
    }
    break;
    case APP_MESSAGE_MQTT_DATA:
    {
        Serial.println("APP_MESSAGE_MQTT_DATA");
        if (app_exit_flag == 1 && cur_app_index != getAppIdxByName("Heartbeat")) // 在其他app中
        {
            app_exit_flag = 0;
            (*(appList[cur_app_index]->exit_callback))(NULL); // 退出当前app
        }
        if (app_exit_flag == 0)
        {
            app_exit_flag = 1; // 进入app, 如果已经在
            cur_app_index = getAppIdxByName("Heartbeat");
            (*(getAppByName("Heartbeat")->app_init))(this); // 执行APP初始化
        }
    }
    break;
    default:
        break;
    }

    return true;
}

void AppController::app_exit()
{
    app_exit_flag = 0; // 退出APP

    // 清空该对象的所有请求
    if (NULL != m_eventListMutex && pdTRUE == xSemaphoreTake(m_eventListMutex, portMAX_DELAY))
    {
        for (std::list<EVENT_OBJ>::iterator event = eventList.begin(); event != eventList.end();)
        {
            if (appList[cur_app_index] == (*event).from)
            {
                event = eventList.erase(event);
            }
            else
            {
                ++event;
            }
        }
        xSemaphoreGive(m_eventListMutex);
    }

    if (NULL != appList[cur_app_index]->exit_callback)
    {
        // 执行APP退出回调
        (*(appList[cur_app_index]->exit_callback))(NULL);
    }
    app_control_display_scr(appList[cur_app_index]->app_image,
                            appList[cur_app_index]->app_name,
                            LV_SCR_LOAD_ANIM_NONE, true);

    // 恢复RGB灯  HSV色彩模式
    RgbConfig *cfg = &rgb_cfg;
    RgbParam rgb_setting = {LED_MODE_HSV,
                            cfg->min_value_0, cfg->min_value_1, cfg->min_value_2,
                            cfg->max_value_0, cfg->max_value_1, cfg->max_value_2,
                            cfg->step_0, cfg->step_1, cfg->step_2,
                            cfg->min_brightness, cfg->max_brightness,
                            cfg->brightness_step, cfg->time, 

                            cfg->brightness_night_mode_specified, 
                            cfg->brightness_night_mode_start, 
                            cfg->brightness_night_mode_end};
    set_rgb_and_run(&rgb_setting);

    // 设置CPU主频
    if (1 == this->sys_cfg.power_mode)
    {
        setCpuFrequencyMhz(240);
    }
    else
    {
        setCpuFrequencyMhz(160);
    }
    Serial.print(F("CpuFrequencyMhz: "));
    Serial.println(getCpuFrequencyMhz());
}
