#include "screen_share_udp.h"
#include "screen_share_udp_gui.h"
#include "common.h"
#include "sys/app_controller.h"
#include "scale_function.h"   // 缩放与颜色转换函数
#include <AsyncUDP.h>
static AsyncUDP asyncUdp;
#define UDP_PORT 8888

#define SCREEN_SHARE_APP_NAME "Screen share UDP"
#define SHARE_WIFI_ALIVE 20000UL
#define SCREEN_SHARE_CONFIG_PATH "/screen_share_udp.cfg"

// 绘制任务句柄
static TaskHandle_t drawTaskHandle = NULL;
static volatile bool drawTaskRunning = false;

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

#define UDP_PORT 8888
#define IMG_W 240
#define RGB_LINE_BATCH 8
#define FRAME_BUF_COUNT 8

enum BufState {
    BUF_FREE,
    BUF_FILLING,
    BUF_READY,
    BUF_DISPLAYING
};

// ================= 修改：FrameData 存储原始数据参数 =================
struct FrameData {
    uint16_t y_start;      // 目标起始行（缩放后，由drawTask计算填充）
    uint16_t line_count;   // 目标行数（缩放后，由drawTask计算填充）
    uint16_t* lines;       // 指向原始数据缓冲区（未缩放/未转换）
    uint16_t src_w;        // 原始宽度（240/180/120）
    uint16_t src_y0;       // 原始起始行号
    uint8_t  src_lines;    // 原始行数
    uint8_t  is_rgb565;    // 原始格式是否为RGB565（1:RGB565, 0:RGB332）
    volatile BufState state;
};

static FrameData* frameBuf = nullptr;

uint16_t* dmaBuf[2];
volatile uint8_t dmaSel = 0;

volatile uint32_t frameCount = 0;
volatile uint32_t dropCount = 0;
volatile uint32_t udpPackets = 0;

// ================= 修改：drawTask 集中处理缩放和颜色转换 =================
void drawTask(void* param)
{
    drawTaskRunning = true;
    while (drawTaskRunning) {
        if (!frameBuf || !run_data) {
            vTaskDelay(1);
            continue;
        }
        
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

        if (!newest) {
            vTaskDelay(1);
            continue;
        }

        newest->state = BUF_DISPLAYING;
        uint8_t nextDma = dmaSel ^ 1;

        // 等待上一次 DMA 完成
        tft->dmaWait();

        int draw_y0 = 0;
        int draw_lines = 0;

        // ----- 根据原始分辨率分别处理 -----
        if (newest->src_w == 240) {
            // 240: 无需缩放，仅可能需颜色转换
            if (newest->is_rgb565) {
                // RGB565 直接拷贝
                memcpy(dmaBuf[nextDma], newest->lines,
                       240 * newest->src_lines * 2);
            } else {
                // RGB332 → RGB565（查表加速）
                uint16_t* dst = dmaBuf[nextDma];
                uint8_t*  src = (uint8_t*)newest->lines;
                int n = 240 * newest->src_lines;
                while (n >= 4) {
                    dst[0] = rgb332_to_565_lut[src[0]];
                    dst[1] = rgb332_to_565_lut[src[1]];
                    dst[2] = rgb332_to_565_lut[src[2]];
                    dst[3] = rgb332_to_565_lut[src[3]];
                    src += 4;
                    dst += 4;
                    n   -= 4;
                }
                while (n--) {
                    *dst++ = rgb332_to_565_lut[*src++];
                }
            }
            draw_y0   = newest->src_y0;
            draw_lines = newest->src_lines;
        }
        else if (newest->src_w == 180) {
            // 180 → 240 缩放
            if (newest->is_rgb565) {
                scale_180_to_240_rgb565(
                    (uint16_t*)newest->lines,
                    dmaBuf[nextDma],
                    newest->src_lines
                );
            } else {
                scale_180_to_240_rgb332(
                    (uint8_t*)newest->lines,
                    dmaBuf[nextDma],
                    newest->src_lines
                );
            }
            // 计算目标行范围（与原逻辑一致）
            draw_y0   = (newest->src_y0 * 240 + 120) / 180;
            draw_lines = (newest->src_lines * 240 + 179) / 180;
        }
        else if (newest->src_w == 120) {
            // 120 → 240 缩放
            if (newest->is_rgb565) {
                scale_120_to_240_rgb565(
                    (uint16_t*)newest->lines,
                    dmaBuf[nextDma],
                    newest->src_lines
                );
            } else {
                scale_120_to_240_rgb332(
                    (uint8_t*)newest->lines,
                    dmaBuf[nextDma],
                    newest->src_lines
                );
            }
            draw_y0   = newest->src_y0 * 2;
            draw_lines = newest->src_lines * 2;
        }

        // ----- 检查 DMA 缓冲区容量（最多 RGB_LINE_BATCH 行）-----
        if (draw_lines > RGB_LINE_BATCH) {
            // 截断至最大行数（与原逻辑相同，避免越界）
            draw_lines = RGB_LINE_BATCH;
        }

        // ----- 推送到屏幕 -----
        tft->startWrite();
        tft->pushImageDMA(
            0,
            draw_y0,
            IMG_W,
            draw_lines,
            dmaBuf[nextDma]
        );
        tft->endWrite();

        // 更新 DMA 选择、帧计数、释放缓冲区
        dmaSel = nextDma;
        newest->state = BUF_FREE;
        frameCount++;
    }
    
    drawTaskRunning = false;
    vTaskDelete(NULL);
}

// ================= 修改：processUDP 只接收原始数据，不做任何缩放/转换 =================
static void onUdpPacket(AsyncUDPPacket packet)
{
    if (!frameBuf || !run_data || !run_data->client_connected) {
        return;
    }

    udpPackets++;

    const uint8_t* data = packet.data();
    uint16_t len = packet.length();

    // -------- Header 校验 --------
    if (len < 5) {
        dropCount++;
        return;
    }

    const uint8_t* header = data;
    uint16_t src_y0 = (header[2] << 8) | header[3];
    uint8_t flags = header[4];

    uint8_t resolution = (flags >> 6) & 0x03; // 0=240,1=180,2=120
    uint8_t color_mode = (flags >> 4) & 0x03; // 0=RGB565,1=RGB332
    uint8_t src_lines  = flags & 0x0F;

    if (src_lines == 0 || src_lines > RGB_LINE_BATCH) {
        dropCount++;
        return;
    }

    bool is_rgb565 = (color_mode == 0);

    // -------- 源宽度 --------
    int src_w;
    switch (resolution) {
        case 0: src_w = 240; break;
        case 1: src_w = 180; break;
        case 2: src_w = 120; break;
        default:
            dropCount++;
            return;
    }

    uint32_t bytes_per_px = is_rgb565 ? 2 : 1;
    uint32_t expect = src_w * src_lines * bytes_per_px;

    if (len < 5 + expect) {
        dropCount++;
        return;
    }

    // -------- 找空闲 buffer --------
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
        return;
    }

    // -------- 拷贝 payload --------
    memcpy(
        (uint8_t*)f->lines,
        data + 5,
        expect
    );

    // -------- 填充元数据 --------
    f->src_w     = src_w;
    f->src_y0    = src_y0;
    f->src_lines = src_lines;
    f->is_rgb565 = is_rgb565 ? 1 : 0;

    f->state = BUF_READY;
}

// ================= Debug Info（保持不变）=================
void printDebugInfo() {
    static uint32_t lastPrint = 0;
    uint32_t now = millis();
    
    if (now - lastPrint > 5000) {
        lastPrint = now;
        
        Serial.printf("内存: %u, ", ESP.getFreeHeap());
        Serial.printf("UDP包/5秒: %u, ", udpPackets);
        Serial.printf("丢包数: %u, ", dropCount);
        Serial.printf("显示帧数: %u, ", frameCount);
        
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

static void initdata() {
    frameBuf = (FrameData*)calloc(FRAME_BUF_COUNT, sizeof(FrameData));
    if (!frameBuf) {
        Serial.println("frameBuf alloc failed");
        while (1);
    }

    for (int i = 0; i < FRAME_BUF_COUNT; i++) {
        // 缓冲区大小仍为 240×8×2 = 3840 字节，足够存放最大原始包（240×8×2）
        frameBuf[i].lines = (uint16_t*)heap_caps_malloc(
            1460, // 这个大小主要是MTU单元大小
            MALLOC_CAP_8BIT
        );
        if (!frameBuf[i].lines) {
            Serial.println("frame line alloc failed");
            while (1);
        }
        frameBuf[i].state = BUF_FREE;
        // 新增字段已由 calloc 清零，无需额外初始化
    }
    init_scale_maps();

    // 分配 DMA 缓冲区（240×8×2，与原一致）
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
}

// ================= 以下函数（init、process、exit、message_handle）保持不变 =================
static int screen_share_init(AppController *sys)
{
    read_config(&cfg_data);
    if (0 == cfg_data.powerFlag) setCpuFrequencyMhz(160);
    else setCpuFrequencyMhz(240);

    RgbParam rgb_setting = {LED_MODE_HSV, 0, 128, 32, 255, 255, 32, 1, 1, 1, 150, 250, 1, 30};
    set_rgb_and_run(&rgb_setting);

    screen_share_udp_gui_init();
    
    run_data = (ScreenShareAppRunData *)calloc(1, sizeof(ScreenShareAppRunData));
    run_data->udp_start = 0;
    run_data->req_sent = 0;
    run_data->client_connected = false;

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
        
        printDebugInfo();
    }
}

static void screen_background_task(AppController *sys, const ImuAction *act_info)
{
    // 后台任务
}

static int screen_exit_callback(void *param)
{
    if (drawTaskHandle) {
        drawTaskRunning = false;
        for (int i = 0; i < 50; i++) {
            if (eTaskGetState(drawTaskHandle) == eDeleted) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        drawTaskHandle = NULL;
    }
    
    asyncUdp.close();
    run_data->client_connected = false;

    if (dmaBuf[0]) { heap_caps_free(dmaBuf[0]); dmaBuf[0] = nullptr; }
    if (dmaBuf[1]) { heap_caps_free(dmaBuf[1]); dmaBuf[1] = nullptr; }

    stop_share_config();
    screen_share_udp_gui_del();





    tft->setSwapBytes(run_data->tftSwapStatus);

    RgbParam rgb_setting = {LED_MODE_HSV, 1, 32, 255, 255, 255, 255, 1, 1, 1, 150, 250, 1, 30};
    set_rgb_and_run(&rgb_setting);

    if (NULL != run_data) { free(run_data); run_data = NULL; }
    if (frameBuf) {
        for (int i = 0; i < FRAME_BUF_COUNT; i++) {
            heap_caps_free(frameBuf[i].lines);
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
            LV_SCR_LOAD_ANIM_NONE
        );

        run_data->udp_start = 1;
        run_data->client_connected = true;

        if (asyncUdp.listen(UDP_PORT)) {
            asyncUdp.onPacket(onUdpPacket);
            Serial.printf("AsyncUDP listening on port %d\n", UDP_PORT);
        } else {
            Serial.println("AsyncUDP listen failed!");
        }

        // 创建绘制任务（核心1）
        xTaskCreatePinnedToCore(
            drawTask,
            "draw_task",
            4096,
            nullptr,
            2,
            &drawTaskHandle,
            1
        );

        Serial.printf("Server started on port %d\n", UDP_PORT);
    }
    break;
    case APP_MESSAGE_WIFI_ALIVE: break;
    case APP_MESSAGE_GET_PARAM:
    {
        char *param_key = (char *)message;
        if (!strcmp(param_key, "powerFlag"))
            snprintf((char *)ext_info, 32, "%u", cfg_data.powerFlag);
        else
            snprintf((char *)ext_info, 32, "%s", "NULL");
    }
    break;
    case APP_MESSAGE_SET_PARAM:
    {
        char *param_key = (char *)message;
        char *param_val = (char *)ext_info;
        if (!strcmp(param_key, "powerFlag"))
            cfg_data.powerFlag = atol(param_val);
    }
    break;
    case APP_MESSAGE_READ_CFG: read_config(&cfg_data); break;
    case APP_MESSAGE_WRITE_CFG: write_config(&cfg_data); break;
    default: break;
    }
}

APP_OBJ screen_share_udp_app = {SCREEN_SHARE_APP_NAME, &app_screen_share_udp, "",
                                screen_share_init, screen_share_process, screen_background_task,
                                screen_exit_callback, screen_message_handle};