#include "screen_share_udp.h"
#include "screen_share_udp_gui.h"
#include "common.h"
#include "sys/app_controller.h"
#include "scale_function.h"

#define SCREEN_SHARE_APP_NAME "Screen share UDP"


#define SHARE_WIFI_ALIVE 20000UL

#define SCREEN_SHARE_CONFIG_PATH "/screen_share_udp.cfg"  

// UDP接收任务句柄
static TaskHandle_t udpTaskHandle = NULL;
static volatile bool udpTaskRunning = false;

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
    boolean udp_start;
    boolean req_sent;
    boolean client_connected;

    boolean tftSwapStatus;

    unsigned long pre_wifi_alive_millis;
    unsigned long last_data_time;
    unsigned long connection_start_time;
};

static SS_Config cfg_data;
static ScreenShareAppRunData *run_data = NULL;

WiFiUDP udp;
#define UDP_PORT 8888
// ================= Image =================
#define IMG_W 240

#define RGB_LINE_BATCH 12  // 需要足够大，因为放大后行数可能增加

// ================= Frame Buffer =================
#define FRAME_BUF_COUNT 6

enum BufState {
    BUF_FREE,
    BUF_FILLING,
    BUF_READY,
    BUF_DISPLAYING
};

struct FrameData {
    uint16_t y_start;
    uint16_t line_count;
    // uint16_t lines[IMG_W * RGB_LINE_BATCH];
    uint16_t* lines;   // 指针
    volatile BufState state;
};

// FrameData frameBuf[FRAME_BUF_COUNT];
static FrameData* frameBuf = nullptr;

// ================= DMA =================
uint16_t* dmaBuf[2];
volatile uint8_t dmaSel = 0;

// ================= Stats =================
volatile uint32_t frameCount = 0;
volatile uint32_t dropCount = 0;
volatile uint32_t udpPackets = 0;


// ================= UDP Receiver Task =================
void udpReceiverTask(void* param)
{
    udpTaskRunning = true;
    run_data->client_connected = true;
    // 增大缓冲区以容纳放大后的数据
    // static uint8_t rxBuf[IMG_W * RGB_LINE_BATCH * 2];
        uint8_t* rxBuf = (uint8_t*)malloc(IMG_W * RGB_LINE_BATCH * 2);
    if (!rxBuf) {
        Serial.println("rxBuf alloc failed");
        vTaskDelete(NULL);
    }

    while (udpTaskRunning) {
        if (!frameBuf || !run_data) {
            vTaskDelay(1);
            continue;
        }

        int packetSize = udp.parsePacket();
        if (!udpTaskRunning) break;
        if (packetSize <= 0) {
            vTaskDelay(1);
            continue;
        }
        udpPackets++;

        // ------------------ 读 Header ------------------
        uint8_t header[5];
        if (udp.read(header, 5) != 5) {
            udp.flush();
            continue;
        }

        uint16_t frame_id = (header[0] << 8) | header[1];
        uint16_t src_y0 = (header[2] << 8) | header[3];
        uint8_t flags = header[4];

        uint8_t resolution = (flags >> 6) & 0x03; // 0=240,1=180,2=120
        uint8_t color_mode = (flags >> 4) & 0x03; // 0=RGB565,1=RGB332
        uint8_t src_lines = flags & 0x0F;

        if (src_lines == 0 || src_lines > RGB_LINE_BATCH) {
            udp.flush();
            continue;
        }

        bool is_rgb565 = (color_mode == 0);

        // ------------------ 源尺寸 ------------------
        int src_w, src_h;
        switch (resolution) {
            case 0: src_w = src_h = 240; break;
            case 1: src_w = src_h = 180; break;
            case 2: src_w = src_h = 120; break;
            default:
                udp.flush();
                continue;
        }

        // ------------------ 计算接收大小 ------------------
        uint32_t bytes_per_px = is_rgb565 ? 2 : 1;
        uint32_t expect = src_w * src_lines * bytes_per_px;

        // ------------------ 找空 buffer ------------------
        FrameData* f = nullptr;
        for (int i = 0; i < FRAME_BUF_COUNT; i++) {
            if (frameBuf[i].state == BUF_FREE) {
                f = &frameBuf[i];
                f->state = BUF_FILLING;
                break;
            }
        }

        if (!f) {
            dropCount++;
            udp.flush();
            continue;
        }

        if (udp.read(rxBuf, expect) != expect) {
            f->state = BUF_FREE;
            continue;
        }

        // =================================================
        //            分辨率统一 → 240 RGB565
        // =================================================
        
        uint16_t* dst = f->lines;
        int dst_y0 = 0;
        int dst_lines = 0;

        // =============== 240 → 240 ======================
        if (src_w == 240) {
            dst_y0 = src_y0;
            dst_lines = src_lines;
            
            if (is_rgb565) {
                memcpy(dst, rxBuf, 240 * src_lines * 2);
            } else {
                uint32_t px = 240 * src_lines;
                for (uint32_t i = 0; i < px; i++) {
                    dst[i] = rgb332_to_rgb565(rxBuf[i]);
                }
            }
        }
        // =============== 180 → 240 ======================
        else if (src_w == 180) {
            // 计算目标行范围
            dst_y0 = (src_y0 * 240 + 120) / 180;  // 四舍五入
            dst_lines = (src_lines * 240 + 179) / 180; // 向上取整
            

            
            // 检查缓冲区是否足够
            if (dst_lines > RGB_LINE_BATCH) {
                f->state = BUF_FREE;
                udp.flush();
                continue;
            }
            
            scale_180_to_240_table(rxBuf, dst, is_rgb565, src_lines);
        }
        // =============== 120 → 240 ======================
        else if (src_w == 120) {
            // 计算目标行范围
            dst_y0 = src_y0 * 2;
            dst_lines = src_lines * 2;
            // 确保不超出240边界
            if (dst_y0 + dst_lines > 240) {
                dst_lines = 240 - dst_y0;
            }

        // 检查缓冲区是否足够
        // 检查缓冲区是否足够 - 使用动态计算
        if (dst_lines > RGB_LINE_BATCH) {
            // 如果超出，调整接收到的行数
            int max_src_lines = RGB_LINE_BATCH / 2;
            if (src_lines > max_src_lines) {
                src_lines = max_src_lines;
                dst_lines = max_src_lines * 2;
            }
        }
        
        scale_120_to_240(rxBuf, dst, is_rgb565, src_lines);
        }

        // ------------------ 提交 ------------------
        f->y_start = dst_y0;
        f->line_count = dst_lines;
        f->state = BUF_READY;
    }

    Serial.println("释放UDP线程任务的rxBuf以及停止udp服务");
    free(rxBuf);
    udp.stop();              // 关闭 socket
    run_data->client_connected = false;
    udpTaskRunning = false;
    vTaskDelete(NULL);
}

// ================= Draw Frame =================
void drawFrame() {
    FrameData* newest = nullptr;
    int newestIdx = -1;

    // 找 READY 的缓冲区
    for (int i = 0; i < FRAME_BUF_COUNT; i++) {
        if (frameBuf[i].state == BUF_READY) {
            newest = &frameBuf[i];
            newestIdx = i;
            break;
        }
    }

    if (!newest) return;

    // 其余 READY 的直接丢弃（防堆积）
    // for (int i = 0; i < FRAME_BUF_COUNT; i++) {
    //     if (i != newestIdx && frameBuf[i].state == BUF_READY) {
    //         frameBuf[i].state = BUF_FREE;
    //     }
    // }

    newest->state = BUF_DISPLAYING;

    uint8_t nextDma = dmaSel ^ 1;

    // 等待 DMA 完成
    tft->dmaWait();

    memcpy(
        dmaBuf[nextDma],
        newest->lines,
        IMG_W * newest->line_count * 2
    );

    tft->startWrite();
    tft->pushImageDMA(
        0,
        newest->y_start,
        IMG_W,
        newest->line_count,
        dmaBuf[nextDma]
    );
    tft->endWrite();

    dmaSel = nextDma;
    newest->state = BUF_FREE;
    frameCount++;
}

static void initdata() {


    frameBuf = (FrameData*)calloc(FRAME_BUF_COUNT, sizeof(FrameData));
    if (!frameBuf) {
        Serial.println("frameBuf alloc failed");
        while (1);
    }

    for (int i = 0; i < FRAME_BUF_COUNT; i++) {
        frameBuf[i].lines = (uint16_t*)heap_caps_malloc(
            IMG_W * RGB_LINE_BATCH * 2,
            MALLOC_CAP_8BIT   // ❗不要 DMA，这不是 DMA buffer
        );
        if (!frameBuf[i].lines) {
            Serial.println("frame line alloc failed");
            while (1);
        }
        frameBuf[i].state = BUF_FREE;
    }
    init_scale_maps();

    // 分配 DMA 缓冲区
    dmaBuf[0] = (uint16_t*)heap_caps_malloc(
        IMG_W * RGB_LINE_BATCH * 2,
        MALLOC_CAP_DMA
    );
    dmaBuf[1] = (uint16_t*)heap_caps_malloc(
        IMG_W * RGB_LINE_BATCH * 2,
        MALLOC_CAP_DMA
    );

    if (!dmaBuf[0] || !dmaBuf[1]) {
        Serial.println("DMA alloc failed");
        while (1);
    }

    // 创建 UDP 接收任务
    // xTaskCreatePinnedToCore(
    //     udpReceiverTask,
    //     "udp_rx",
    //     8192,
    //     nullptr,
    //     2,
    //     nullptr,
    //     0
    // );
}

static int screen_share_init(AppController *sys)
{
    read_config(&cfg_data);

    if (0 == cfg_data.powerFlag)
    {
        setCpuFrequencyMhz(160);
        // setCpuFrequencyMhz(240);
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

    screen_share_udp_gui_init();
    
    run_data = (ScreenShareAppRunData *)calloc(1, sizeof(ScreenShareAppRunData));
    run_data->udp_start = 0;
    run_data->req_sent = 0;

    initdata();
    
    run_data->pre_wifi_alive_millis = 0;
    run_data->last_data_time = 0;
    run_data->connection_start_time = 0;

    tft->initDMA();

    run_data->tftSwapStatus = tft->getSwapBytes();
    tft->setSwapBytes(true);

    Serial.print(F("Screen Share initialized!\n"));
    return 0;
}

static void stop_share_config()
{
    run_data->udp_start = 0;
    run_data->req_sent = 0;
    
}

// ================= Debug Info =================
void printDebugInfo() {
    static uint32_t lastPrint = 0;
    uint32_t now = millis();
    
    if (now - lastPrint > 5000) {
        lastPrint = now;
        
        Serial.printf("内存: %u, ", ESP.getFreeHeap());
        Serial.printf("UDP包/5秒: %u, ", udpPackets);
        Serial.printf("丢包数: %u, ", dropCount);
        Serial.printf("显示帧数: %u, ", frameCount);
        
        // 显示缓冲区状态
        Serial.print("缓冲区状态: ");
        for (int i = 0; i < FRAME_BUF_COUNT; i++) {
            switch(frameBuf[i].state) {
                case BUF_FREE: Serial.print("F"); break;
                case BUF_FILLING: Serial.print("I"); break;
                case BUF_READY: Serial.print("R"); break;
                case BUF_DISPLAYING: Serial.print("D"); break;
            }
        }
        Serial.println();
        
        udpPackets = 0;
    }
}

static void screen_share_process(AppController *sys, const ImuAction *action)
{
    if (RETURN == action->active)
    {
        sys->app_exit();
        return;
    }

    if (0 == run_data->udp_start && 0 == run_data->req_sent)
    {
        display_screen_share_udp(
            "Screen Share UDP",
            WiFi.localIP().toString().c_str(),
            "8888",
            "WiFi connecting...",
            LV_SCR_LOAD_ANIM_NONE);
        
        sys->send_to(SCREEN_SHARE_APP_NAME, CTRL_NAME,
                     APP_MESSAGE_WIFI_CONN, NULL, NULL);
        run_data->req_sent = 1;
    }
    else if (1 == run_data->udp_start)
    {
        if (doDelayMillisTime(SHARE_WIFI_ALIVE, &run_data->pre_wifi_alive_millis, false))
        {
            sys->send_to(SCREEN_SHARE_APP_NAME, CTRL_NAME,
                         APP_MESSAGE_WIFI_ALIVE, NULL, NULL);
        }
        drawFrame();
        printDebugInfo();
        // 更新显示状态
        // static unsigned long last_display_update = 0;
        // if (doDelayMillisTime(5000, &last_display_update, false))
        // {
        //     if (run_data->client_connected)
        //     {
        //         // Serial.println("client connected.");
        //         // display_screen_share(
        //         //     "Screen Share",
        //         //     WiFi.localIP().toString().c_str(),
        //         //     "8081",
        //         //     "Connected",
        //         //     LV_SCR_LOAD_ANIM_NONE);
        //     }
        //     else
        //     {
        //         display_screen_share(
        //             "Screen Share UDP",
        //             WiFi.localIP().toString().c_str(),
        //             "8888",
        //             "Wait connect ....",
        //             LV_SCR_LOAD_ANIM_NONE);
        //     }
        // }
    }
}

static void screen_background_task(AppController *sys, const ImuAction *act_info)
{
    // 后台任务
}

static int screen_exit_callback(void *param)
{

    // 通知 UDP 任务退出
    if (udpTaskHandle) {
        udpTaskRunning = false;

        // 等任务自己 vTaskDelete
            for (int i = 0; i < 50; i++) {
                if (eTaskGetState(udpTaskHandle) == eDeleted) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }

        udpTaskHandle = NULL;

    }
    udp.stop();

    // 释放DMA缓存空间
    if (dmaBuf[0]) {
    heap_caps_free(dmaBuf[0]);
    dmaBuf[0] = nullptr;
    }
    if (dmaBuf[1]) {
        heap_caps_free(dmaBuf[1]);
        dmaBuf[1] = nullptr;
    }

    stop_share_config();
    screen_share_udp_gui_del();

    tft->setSwapBytes(run_data->tftSwapStatus);

    RgbParam rgb_setting = {LED_MODE_HSV,
                            1, 32, 255,
                            255, 255, 255,
                            1, 1, 1,
                            150, 250, 1, 30};
    set_rgb_and_run(&rgb_setting);



    // 清空数据
    if (NULL != run_data)
    {
        free(run_data);
        run_data = NULL;
    }
    if (frameBuf) {
        for (int i = 0; i < FRAME_BUF_COUNT; i++) {
            free(frameBuf[i].lines);
        }
        free(frameBuf);
        frameBuf = nullptr;
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
        display_screen_share_udp(
            "Screen Share UDP",
            WiFi.localIP().toString().c_str(),
            "8888",
            "WiFi Connect succ.",
            LV_SCR_LOAD_ANIM_NONE);
        run_data->udp_start = 1;
        
        // 确保服务器正确启动
        udp.begin(UDP_PORT);


        // 创建 UDP 接收任务
        xTaskCreatePinnedToCore(
            udpReceiverTask,
            "udp_rx",
            8192,
            nullptr,
            2,
            &udpTaskHandle,
            0
        );



        Serial.printf("Server started on port %d\n", UDP_PORT);
    }
    break;
    case APP_MESSAGE_WIFI_ALIVE:
    {
        // wifi心跳
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

APP_OBJ screen_share_udp_app = {SCREEN_SHARE_APP_NAME, &app_screen_share_udp, "",
                            screen_share_init, screen_share_process, screen_background_task,
                            screen_exit_callback, screen_message_handle};