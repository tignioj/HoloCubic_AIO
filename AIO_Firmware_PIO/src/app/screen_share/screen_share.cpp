#include "screen_share.h"
#include "screen_share_gui.h"
#include "common.h"
#include <TJpg_Decoder.h>
#include "sys/app_controller.h"

#define SCREEN_SHARE_APP_NAME "Screen share"

#define JPEG_BUFFER_SIZE 1
#define RECV_BUFFER_SIZE 50000
#define DMA_BUFFER_SIZE 512
#define SHARE_WIFI_ALIVE 20000UL

#define HTTP_PORT 8081
#define CONNECTION_TIMEOUT 5000  // 连接超时时间
#define READ_TIMEOUT 3000        // 读取超时时间
#define SCREEN_SHARE_CONFIG_PATH "/screen_share.cfg"
WiFiServer ss_server(HTTP_PORT);
WiFiClient ss_client;

struct SS_Config
{
    uint8_t powerFlag;
};

static void write_config(SS_Config *cfg)
{
    char tmp[16];
    String w_data;
    memset(tmp, 0, 16);
    snprintf(tmp, 16, "%u\n", cfg->powerFlag);
    w_data += tmp;
    g_flashCfg.writeFile(SCREEN_SHARE_CONFIG_PATH, w_data.c_str());
}

static void read_config(SS_Config *cfg)
{
    char info[128] = {0};
    uint16_t size = g_flashCfg.readFile(SCREEN_SHARE_CONFIG_PATH, (uint8_t *)info);
    info[size] = 0;
    if (size == 0)
    {
        cfg->powerFlag = 0;
        write_config(cfg);
    }
    else
    {
        char *param[1] = {0};
        analyseParam(info, 1, param);
        cfg->powerFlag = atol(param[0]);
    }
}

struct ScreenShareAppRunData
{
    boolean tcp_start;
    boolean req_sent;
    boolean client_connected;

    uint8_t *recvBuf;
    uint8_t *mjpeg_start;
    uint8_t *mjpeg_end;
    uint8_t *last_find_pos;
    int32_t bufSaveTail;
    uint8_t *displayBufWithDma[2];
    bool dmaBufferSel;
    boolean tftSwapStatus;

    unsigned long pre_wifi_alive_millis;
    unsigned long last_data_time;
    unsigned long connection_start_time;
};

static SS_Config cfg_data;
static ScreenShareAppRunData *run_data = NULL;

bool screen_share_tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
    if (y >= tft->height())
        return 0;

    uint16_t *dmaBufferPtr;
    if (run_data->dmaBufferSel)
        dmaBufferPtr = (uint16_t *)run_data->displayBufWithDma[0];
    else
        dmaBufferPtr = (uint16_t *)run_data->displayBufWithDma[1];
    run_data->dmaBufferSel = !run_data->dmaBufferSel;
    
    tft->pushImageDMA(x, y, w, h, bitmap, dmaBufferPtr);
    
    return true;
}

static bool readJpegFromBuffer(uint8_t *const end)
{
    bool isFound = false;
    uint8_t *pfind = run_data->last_find_pos;
    
    if (NULL == run_data->mjpeg_start)
    {
        while (pfind < end - 1)  // 防止越界访问
        {
            if (*pfind == 0xFF && *(pfind + 1) == 0xD8)
            {
                run_data->mjpeg_start = pfind;
                break;
            }
            ++pfind;
        }
        run_data->last_find_pos = pfind;
    }
    else if (NULL == run_data->mjpeg_end)
    {
        while (pfind < end - 1)  // 防止越界访问
        {
            if (*pfind == 0xFF && *(pfind + 1) == 0xD9)
            {
                run_data->mjpeg_end = pfind + 1;
                isFound = true;
                break;
            }
            ++pfind;
        }
        run_data->last_find_pos = pfind;
    }
    return isFound;
}

static void reset_connection()
{
    if (ss_client.connected())
    {
        ss_client.stop();
    }
    run_data->client_connected = false;
    run_data->mjpeg_start = NULL;
    run_data->mjpeg_end = NULL;
    run_data->last_find_pos = run_data->recvBuf;
    run_data->bufSaveTail = 0;
}

static int screen_share_init(AppController *sys)
{
    read_config(&cfg_data);

    if (0 == cfg_data.powerFlag)
    {
        setCpuFrequencyMhz(160);
    }
    else
    {
        setCpuFrequencyMhz(240);
    }

    RgbParam rgb_setting = {LED_MODE_HSV, 0, 128, 32,
                            255, 255, 32,
                            1, 1, 1,
                            150, 250, 1, 30};
    set_rgb_and_run(&rgb_setting);

    screen_share_gui_init();
    
    run_data = (ScreenShareAppRunData *)calloc(1, sizeof(ScreenShareAppRunData));
    run_data->tcp_start = 0;
    run_data->req_sent = 0;
    run_data->client_connected = false;
    
    run_data->recvBuf = (uint8_t *)malloc(RECV_BUFFER_SIZE);
    if (run_data->recvBuf == NULL) {
        Serial.println("Failed to allocate recvBuf!");
        return -1;
    }
    
    run_data->mjpeg_start = NULL;
    run_data->mjpeg_end = NULL;
    run_data->last_find_pos = run_data->recvBuf;
    run_data->bufSaveTail = 0;
    
    run_data->displayBufWithDma[0] = (uint8_t *)heap_caps_malloc(DMA_BUFFER_SIZE, MALLOC_CAP_DMA);
    run_data->displayBufWithDma[1] = (uint8_t *)heap_caps_malloc(DMA_BUFFER_SIZE, MALLOC_CAP_DMA);
    
    if (run_data->displayBufWithDma[0] == NULL || run_data->displayBufWithDma[1] == NULL) {
        Serial.println("Failed to allocate DMA buffers!");
        return -1;
    }
    
    run_data->dmaBufferSel = false;
    run_data->pre_wifi_alive_millis = 0;
    run_data->last_data_time = 0;
    run_data->connection_start_time = 0;

    tft->initDMA();

    SketchCallback callback = (SketchCallback)&screen_share_tft_output;
    TJpgDec.setCallback(callback);
    TJpgDec.setJpgScale(1);

    run_data->tftSwapStatus = tft->getSwapBytes();
    tft->setSwapBytes(true);

    Serial.print(F("Screen Share initialized!\n"));
    return 0;
}

static void stop_share_config()
{
    run_data->tcp_start = 0;
    run_data->req_sent = 0;
    reset_connection();
    
    if (ss_server) {
        ss_server.stop();
    }
}

static void check_and_accept_connection()
{
    if (!run_data->client_connected)
    {
        WiFiClient new_client = ss_server.accept();
        if (new_client)
        {
            if (ss_client.connected())
            {
                ss_client.stop();
            }
            ss_client = new_client;
            ss_client.setTimeout(100);
            run_data->client_connected = true;
            run_data->connection_start_time = GET_SYS_MILLIS();
            run_data->last_data_time = GET_SYS_MILLIS();
            
            Serial.println(F("New client connected!"));
            
            // 清空缓冲区
            run_data->mjpeg_start = NULL;
            run_data->mjpeg_end = NULL;
            run_data->last_find_pos = run_data->recvBuf;
            run_data->bufSaveTail = 0;
            
            // 发送确认消息
            ss_client.write("ok");
        }
    }
}

static void handle_client_data()
{
    if (!run_data->client_connected || !ss_client.connected())
    {
        if (run_data->client_connected)
        {
            Serial.println(F("Client disconnected!"));
            reset_connection();
        }
        return;
    }

    // 检查连接超时
    if (GET_SYS_MILLIS() - run_data->last_data_time > READ_TIMEOUT)
    {
        Serial.println(F("Connection timeout!"));
        reset_connection();
        return;
    }

    // 检查是否有数据可读
    int available = ss_client.available();
    if (available > 0)
    {
        run_data->last_data_time = GET_SYS_MILLIS();
        
        // 确保不会读取超过缓冲区容量
        int to_read = min(available, (int)(RECV_BUFFER_SIZE - run_data->bufSaveTail));
        if (to_read <= 0)
        {
            // 缓冲区已满，重置
            run_data->bufSaveTail = 0;
            run_data->last_find_pos = run_data->recvBuf;
            run_data->mjpeg_start = NULL;
            run_data->mjpeg_end = NULL;
            ss_client.write("ok");
            return;
        }

        // 读取数据
        int32_t read_count = ss_client.read(&run_data->recvBuf[run_data->bufSaveTail], to_read);
        if (read_count > 0)
        {
            run_data->bufSaveTail += read_count;
            
            unsigned long deal_time = GET_SYS_MILLIS();
            bool get_mjpeg_ret = readJpegFromBuffer(run_data->recvBuf + run_data->bufSaveTail);

            if (get_mjpeg_ret)
            {
                // 找到完整的JPEG帧
                ss_client.write("ok");
                tft->startWrite();
                
                uint32_t frame_size = run_data->mjpeg_end - run_data->mjpeg_start + 1;
                JRESULT jpg_ret = TJpgDec.drawJpg(0, 0, run_data->mjpeg_start, frame_size);
                tft->endWrite();
                
                // 移动剩余数据到缓冲区开头
                uint32_t left_frame_size = &run_data->recvBuf[run_data->bufSaveTail] - run_data->mjpeg_end;
                if (left_frame_size > 0 && run_data->mjpeg_end + 1 >= run_data->recvBuf)
                {
                    memmove(run_data->recvBuf, run_data->mjpeg_end + 1, left_frame_size);
                }
                
                Serial.printf("Frame size: %d ", frame_size);
                Serial.print("Processing speed: ");
                Serial.print(1000.0 / (GET_SYS_MILLIS() - deal_time), 2);
                Serial.print(" Fps\n");

                run_data->last_find_pos = run_data->recvBuf;
                run_data->bufSaveTail = left_frame_size;
                run_data->mjpeg_start = NULL;
                run_data->mjpeg_end = NULL;
            }
            else if (run_data->bufSaveTail >= RECV_BUFFER_SIZE - 1000)  // 留一些余量
            {
                // 缓冲区快满了但没找到完整帧，重置
                run_data->last_find_pos = run_data->recvBuf;
                run_data->bufSaveTail = 0;
                run_data->mjpeg_start = NULL;
                run_data->mjpeg_end = NULL;
                ss_client.write("ok");
            }
        }
    }
    else if (available == 0)
    {
        // 没有数据，小延迟防止过度消耗CPU
        delay(1);
    }
}

static void screen_share_process(AppController *sys, const ImuAction *action)
{
    if (RETURN == action->active)
    {
        sys->app_exit();
        return;
    }

    if (0 == run_data->tcp_start && 0 == run_data->req_sent)
    {
        display_screen_share(
            "Screen Share",
            WiFi.localIP().toString().c_str(),
            "8081",
            "Wait connect ....",
            LV_SCR_LOAD_ANIM_NONE);
        
        sys->send_to(SCREEN_SHARE_APP_NAME, CTRL_NAME,
                     APP_MESSAGE_WIFI_CONN, NULL, NULL);
        run_data->req_sent = 1;
    }
    else if (1 == run_data->tcp_start)
    {
        if (doDelayMillisTime(SHARE_WIFI_ALIVE, &run_data->pre_wifi_alive_millis, false))
        {
            sys->send_to(SCREEN_SHARE_APP_NAME, CTRL_NAME,
                         APP_MESSAGE_WIFI_ALIVE, NULL, NULL);
        }

        check_and_accept_connection();
        handle_client_data();

        // 更新显示状态
        static unsigned long last_display_update = 0;
        if (doDelayMillisTime(1000, &last_display_update, false))
        {
            if (run_data->client_connected && ss_client.connected())
            {
                Serial.println("client connected.");
                // display_screen_share(
                //     "Screen Share",
                //     WiFi.localIP().toString().c_str(),
                //     "8081",
                //     "Connected",
                //     LV_SCR_LOAD_ANIM_NONE);
            }
            else
            {
                display_screen_share(
                    "Screen Share",
                    WiFi.localIP().toString().c_str(),
                    "8081",
                    "Wait connect ....",
                    LV_SCR_LOAD_ANIM_NONE);
            }
        }
    }
}

static void screen_background_task(AppController *sys, const ImuAction *act_info)
{
    // 后台任务
}

static int screen_exit_callback(void *param)
{
    stop_share_config();
    screen_share_gui_del();
    
    if (NULL != run_data->recvBuf)
    {
        free(run_data->recvBuf);
        run_data->recvBuf = NULL;
    }

    if (NULL != run_data->displayBufWithDma[0])
    {
        free(run_data->displayBufWithDma[0]);
        run_data->displayBufWithDma[0] = NULL;
    }
    if (NULL != run_data->displayBufWithDma[1])
    {
        free(run_data->displayBufWithDma[1]);
        run_data->displayBufWithDma[1] = NULL;
    }

    tft->setSwapBytes(run_data->tftSwapStatus);

    RgbParam rgb_setting = {LED_MODE_HSV,
                            1, 32, 255,
                            255, 255, 255,
                            1, 1, 1,
                            150, 250, 1, 30};
    set_rgb_and_run(&rgb_setting);

    if (NULL != run_data)
    {
        free(run_data);
        run_data = NULL;
    }
    return 0;
}

static void screen_message_handle(const char *from, const char *to,
                                  APP_MESSAGE_TYPE type, void *message,
                                  void *ext_info)
{
    switch (type)
    {
    case APP_MESSAGE_WIFI_CONN:
    {
        Serial.print(F("WiFi connected\n"));
        display_screen_share(
            "Screen Share",
            WiFi.localIP().toString().c_str(),
            "8081",
            "Connect succ",
            LV_SCR_LOAD_ANIM_NONE);
        run_data->tcp_start = 1;
        
        // 确保服务器正确启动
        if (!ss_server) {
            ss_server = WiFiServer(HTTP_PORT);
        }
        ss_server.begin();
        ss_server.setNoDelay(true);
        
        Serial.printf("Server started on port %d\n", HTTP_PORT);
    }
    break;
    case APP_MESSAGE_WIFI_ALIVE:
    {
        // wifi心跳
    }
    break;
    case APP_MESSAGE_WIFI_CONN_FAILED:
    {
        if (NULL != run_data)
        {
            run_data->req_sent = 0;
            display_screen_share(
                "Screen Share", "-", "8081", "WiFi failed, retrying",
                LV_SCR_LOAD_ANIM_NONE);
        }
    }
    break;
    case APP_MESSAGE_GET_PARAM:
    {
        char *param_key = (char *)message;
        if (!strcmp(param_key, "powerFlag"))
        {
            snprintf((char *)ext_info, 32, "%u", cfg_data.powerFlag);
        }
        else
        {
            snprintf((char *)ext_info, 32, "%s", "NULL");
        }
    }
    break;
    case APP_MESSAGE_SET_PARAM:
    {
        char *param_key = (char *)message;
        char *param_val = (char *)ext_info;
        if (!strcmp(param_key, "powerFlag"))
        {
            cfg_data.powerFlag = atol(param_val);
        }
    }
    break;
    case APP_MESSAGE_READ_CFG:
    {
        read_config(&cfg_data);
    }
    break;
    case APP_MESSAGE_WRITE_CFG:
    {
        write_config(&cfg_data);
    }
    break;
    default:
        break;
    }
}

APP_OBJ screen_share_app = {SCREEN_SHARE_APP_NAME, &app_screen, "",
                            screen_share_init, screen_share_process, screen_background_task,
                            screen_exit_callback, screen_message_handle};
