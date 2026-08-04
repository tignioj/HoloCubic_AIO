#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

extern "C" {
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>
#include <lwip/udp.h>
#include <lwip/priv/tcpip_priv.h>
}

#include "common.h"
#include "screen_share_udp.h"
#include "screen_share_udp_gui.h"
#include "stream_protocol_v2.h"
#include "sys/app_controller.h"

#define SCREEN_SHARE_APP_NAME "Screen share UDP"
#define SCREEN_SHARE_CONFIG_PATH "/screen_share_udp.cfg"
#define SHARE_WIFI_ALIVE 20000UL

struct SS_Config {
    uint8_t powerFlag;
};

struct ScreenShareAppRunData {
    bool udpStarted;
    bool wifiRequested;
    bool tftSwapStatus;
    unsigned long previousWifiAliveMs;
    wifi_ps_type_t previousWifiPowerSave;
};

static SS_Config cfgData;
static ScreenShareAppRunData* runData = nullptr;

static void writeConfig(const SS_Config* config) {
    char value[16];
    snprintf(value, sizeof(value), "%u\n", config->powerFlag);
    g_flashCfg.writeFile(SCREEN_SHARE_CONFIG_PATH, value);
}

static void readConfig(SS_Config* config) {
    char info[128] = {0};
    const uint16_t size = g_flashCfg.readFile(
        SCREEN_SHARE_CONFIG_PATH, reinterpret_cast<uint8_t*>(info));
    info[min<uint16_t>(size, sizeof(info) - 1)] = 0;
    if (size == 0) {
        config->powerFlag = 0;
        writeConfig(config);
        return;
    }
    char* parameters[1] = {nullptr};
    analyseParam(info, 1, parameters);
    config->powerFlag = atol(parameters[0]);
}

namespace {

constexpr uint16_t kImageWidth = 240;
constexpr uint16_t kImageHeight = 240;
// AIO keeps LVGL and the rest of the application framework resident. A
// 48-packet window preserves substantially more heap than the standalone
// receiver while still covering more than half of an RGB565 frame.
constexpr uint8_t kSlotCount = 48;
constexpr uint8_t kRxFrameWindow = 3;
constexpr uint8_t kRgb332PrebufferChunks = 6;
constexpr uint8_t kRgb565PrebufferChunks = 6;
constexpr uint8_t kDmaGroupChunks = 4;
// RGB332 is expanded directly to the ST7789 12-bit RGB444 wire format. Two
// pixels occupy three bytes, so 45 FPS remains possible at the reliable
// 40 MHz SPI clock. RGB565 remains two bytes per pixel with no conversion.
constexpr size_t kRgb332DmaBytesPerChunk = StreamV2::kPayloadSize * 3 / 2;
constexpr size_t kDmaBufferBytes = kRgb332DmaBytesPerChunk * kDmaGroupChunks;
constexpr int64_t kPrebufferTimeoutUs = 20000;
constexpr int64_t kRgb332MissingChunkTimeoutUs = 20000;
constexpr int64_t kRgb565MissingChunkTimeoutUs = 40000;
// Frame-tail feedback exposes receive pressure; display completion feedback
// closes the two-frame sender pipeline. Tail feedback is bounded to 20 ms.
constexpr uint32_t kFeedbackIntervalMs = 20;
constexpr uint32_t kPowerSaveTimeoutMs = 5000;
constexpr uint32_t kSessionResetIdleMs = 250;
constexpr uint8_t kNoSlot = 0xFF;

constexpr int64_t missingChunkTimeoutUs(uint8_t mode) {
    return mode == StreamV2::kModeRgb565
        ? kRgb565MissingChunkTimeoutUs : kRgb332MissingChunkTimeoutUs;
}

constexpr uint8_t prebufferChunks(uint8_t mode) {
    return mode == StreamV2::kModeRgb565
        ? kRgb565PrebufferChunks : kRgb332PrebufferChunks;
}

struct PacketSlot {
    uint32_t sessionId;
    uint32_t frameId;
    uint8_t mode;
    uint8_t chunkIndex;
    uint16_t reserved;
    alignas(4) uint8_t payload[StreamV2::kPayloadSize];
};

struct RxFrameState {
    bool valid;
    uint32_t frameId;
    uint64_t maskLow;
    uint64_t maskHigh;
    uint8_t expectedChunks;
    uint8_t chunkCount;
};

PacketSlot* packetSlots = nullptr;
QueueHandle_t freeQueue = nullptr;
QueueHandle_t readyQueue = nullptr;
StaticQueue_t freeQueueState;
StaticQueue_t readyQueueState;
uint8_t freeQueueStorage[kSlotCount * sizeof(uint8_t)];
uint8_t readyQueueStorage[kSlotCount * sizeof(uint8_t)];

uint8_t* streamDmaBuffer = nullptr;
uint16_t rgb332ToRgb444[256];
uint8_t panelPixelFormat = 0x55;

udp_pcb* rawUdpPcb = nullptr;
ip_addr_t peerAddress;
uint16_t peerPort = 0;
volatile bool peerValid = false;
volatile bool sessionActive = false;
volatile uint32_t activeSession = 0;
volatile uint8_t activeMode = StreamV2::kModeRgb332;
volatile uint32_t sessionGeneration = 0;

volatile uint32_t feedbackSequence = 0;
volatile uint32_t latestRxFrame = StreamV2::kNoFrame;
volatile uint32_t latestCompleteFrame = StreamV2::kNoFrame;
volatile uint32_t latestDisplayedFrame = StreamV2::kNoFrame;
volatile uint32_t rxPackets = 0;
volatile uint32_t acceptedPackets = 0;
volatile uint32_t overflowPackets = 0;
volatile uint32_t invalidPackets = 0;
volatile uint32_t stalePackets = 0;
volatile uint32_t incompleteFrames = 0;
volatile uint32_t displayedFrames = 0;
volatile uint32_t lastReceiveMs = 0;
volatile bool powerSaveMode = false;
uint32_t lastFeedbackMs = 0;

RxFrameState rxFrameStates[kRxFrameWindow]{};
bool haveLatestTrackedFrame = false;
uint32_t latestTrackedFrame = 0;

uint8_t pendingSlots[kSlotCount];
uint8_t pendingCount = 0;
TaskHandle_t displayTaskHandle = nullptr;
volatile bool streamRunning = false;
volatile bool displayTaskRunning = false;

inline bool sameEndpoint(const ip_addr_t* address, uint16_t port) {
    return peerValid && port == peerPort && ip_addr_cmp(address, &peerAddress);
}

void releaseSlot(uint8_t slotIndex) {
    if (slotIndex == kNoSlot) {
        return;
    }
    if (xQueueSend(freeQueue, &slotIndex, 0) != pdTRUE) {
        Serial.println("fatal: free slot queue overflow");
    }
}

uint16_t queueDepth() {
    if (!freeQueue) {
        return 0;
    }
    const UBaseType_t freeSlots = uxQueueMessagesWaiting(freeQueue);
    return static_cast<uint16_t>(kSlotCount - min<UBaseType_t>(freeSlots, kSlotCount));
}

void resetSessionStats(uint32_t sessionId, uint8_t mode, const ip_addr_t* address, uint16_t port) {
    activeSession = sessionId;
    activeMode = mode;
    sessionActive = true;
    sessionGeneration++;
    ip_addr_copy(peerAddress, *address);
    peerPort = port;
    peerValid = true;
    feedbackSequence = 0;
    latestRxFrame = StreamV2::kNoFrame;
    latestCompleteFrame = StreamV2::kNoFrame;
    latestDisplayedFrame = StreamV2::kNoFrame;
    rxPackets = 0;
    acceptedPackets = 0;
    overflowPackets = 0;
    invalidPackets = 0;
    stalePackets = 0;
    incompleteFrames = 0;
    displayedFrames = 0;
    memset(rxFrameStates, 0, sizeof(rxFrameStates));
    haveLatestTrackedFrame = false;
}

RxFrameState* receiveStateFor(uint32_t frameId, uint8_t expectedChunks) {
    if (!haveLatestTrackedFrame) {
        latestTrackedFrame = frameId;
        haveLatestTrackedFrame = true;
    } else if (frameId != latestTrackedFrame) {
        if (StreamV2::newerFrame(frameId, latestTrackedFrame)) {
            latestTrackedFrame = frameId;
        } else if (static_cast<uint32_t>(latestTrackedFrame - frameId) >= kRxFrameWindow) {
            return nullptr;
        }
    }

    RxFrameState* freeState = nullptr;
    RxFrameState* oldestState = nullptr;
    uint32_t oldestAge = 0;
    for (RxFrameState& state : rxFrameStates) {
        if (state.valid && state.frameId == frameId) {
            return &state;
        }
        if (!state.valid) {
            freeState = &state;
            continue;
        }
        const uint32_t age = latestTrackedFrame - state.frameId;
        if (age >= kRxFrameWindow) {
            state.valid = false;
            freeState = &state;
        } else if (!oldestState || age > oldestAge) {
            oldestState = &state;
            oldestAge = age;
        }
    }
    RxFrameState* state = freeState ? freeState : oldestState;
    if (!state) {
        return nullptr;
    }
    *state = {true, frameId, 0, 0, expectedChunks, 0};
    return state;
}

bool stateHasChunk(const RxFrameState& state, uint8_t chunkIndex) {
    if (chunkIndex < 64) {
        return (state.maskLow & (1ULL << chunkIndex)) != 0;
    }
    return (state.maskHigh & (1ULL << (chunkIndex - 64))) != 0;
}

void markAcceptedChunk(RxFrameState& state, uint8_t chunkIndex) {
    if (chunkIndex < 64) {
        state.maskLow |= 1ULL << chunkIndex;
    } else {
        state.maskHigh |= 1ULL << (chunkIndex - 64);
    }
    state.chunkCount++;
    if (latestRxFrame == StreamV2::kNoFrame
        || state.frameId == latestRxFrame
        || StreamV2::newerFrame(state.frameId, latestRxFrame)) {
        latestRxFrame = state.frameId;
    }
    if (state.chunkCount == state.expectedChunks
        && (latestCompleteFrame == StreamV2::kNoFrame
            || StreamV2::newerFrame(state.frameId, latestCompleteFrame))) {
        latestCompleteFrame = state.frameId;
    }
}

void writeFeedbackField(uint8_t* packet, uint8_t index, uint32_t value) {
    StreamV2::writeU32(packet + 4 + index * 4, value);
}

void sendFeedback(
    udp_pcb* pcb,
    const ip_addr_t* address,
    uint16_t port,
    bool force = false) {
    const uint32_t now = millis();
    if (!sessionActive || (!force && now - lastFeedbackMs < kFeedbackIntervalMs)) {
        return;
    }
    lastFeedbackMs = now;

    uint8_t packet[StreamV2::kFeedbackSize];
    memcpy(packet, StreamV2::kFeedbackMagic, 4);
    writeFeedbackField(packet, 0, activeSession);
    writeFeedbackField(packet, 1, feedbackSequence++);
    writeFeedbackField(packet, 2, latestRxFrame);
    writeFeedbackField(packet, 3, latestCompleteFrame);
    writeFeedbackField(packet, 4, latestDisplayedFrame);
    writeFeedbackField(packet, 5, rxPackets);
    writeFeedbackField(packet, 6, acceptedPackets);
    writeFeedbackField(packet, 7, overflowPackets);
    writeFeedbackField(packet, 8, invalidPackets);
    writeFeedbackField(packet, 9, stalePackets);
    writeFeedbackField(packet, 10, incompleteFrames);
    writeFeedbackField(packet, 11, displayedFrames);
    StreamV2::writeU16(packet + 52, queueDepth());
    StreamV2::writeU16(packet + 54, kSlotCount);
    StreamV2::writeU32(packet + 56, ESP.getFreeHeap());
    StreamV2::writeU32(packet + 60, now);

    pbuf* response = pbuf_alloc(PBUF_TRANSPORT, sizeof(packet), PBUF_RAM);
    if (!response) {
        return;
    }
    if (pbuf_take(response, packet, sizeof(packet)) == ERR_OK) {
        udp_sendto(pcb, response, address, port);
    }
    pbuf_free(response);
}

void sendDisplayedFeedback(void*) {
    if (rawUdpPcb && peerValid) {
        sendFeedback(rawUdpPcb, &peerAddress, peerPort, true);
    }
}

void onUdpPacket(void*, udp_pcb* pcb, pbuf* packet, const ip_addr_t* address, uint16_t port) {
    auto finish = [&]() {
        // A missing frame-tail must not suppress telemetry indefinitely.
        if (peerValid && millis() - lastFeedbackMs >= 100) {
            sendFeedback(pcb, &peerAddress, peerPort);
        }
        pbuf_free(packet);
    };

    if (!streamRunning) {
        finish();
        return;
    }

    if (!packet || packet->tot_len != StreamV2::kDatagramSize) {
        invalidPackets++;
        finish();
        return;
    }

    uint8_t rawHeader[StreamV2::kHeaderSize];
    const bool contiguous = packet->len == packet->tot_len;
    if (contiguous) {
        memcpy(rawHeader, packet->payload, sizeof(rawHeader));
    } else if (pbuf_copy_partial(packet, rawHeader, sizeof(rawHeader), 0)
        != sizeof(rawHeader)) {
            invalidPackets++;
            finish();
            return;
        }
    StreamV2::DataHeader header;
    if (!StreamV2::parseDataHeader(rawHeader, header)) {
        invalidPackets++;
        finish();
        return;
    }

    const uint32_t now = millis();
    const bool newSession = !sessionActive || header.sessionId != activeSession;
    if (newSession) {
        const bool allowed = header.chunkIndex == 0
            && (!sessionActive || header.frameId == 0 || now - lastReceiveMs >= kSessionResetIdleMs);
        if (!allowed) {
            stalePackets++;
            finish();
            return;
        }
        resetSessionStats(header.sessionId, header.mode, address, port);
    } else if (!sameEndpoint(address, port) || header.mode != activeMode) {
        stalePackets++;
        finish();
        return;
    }

    rxPackets++;
    const uint8_t expectedChunks = StreamV2::chunksForMode(header.mode);
    RxFrameState* rxState = receiveStateFor(header.frameId, expectedChunks);
    if (!rxState) {
        stalePackets++;
        finish();
        return;
    }
    if (rxState->expectedChunks != expectedChunks
        || stateHasChunk(*rxState, header.chunkIndex)) {
        stalePackets++;
        finish();
        return;
    }

    uint8_t slotIndex = kNoSlot;
    if (xQueueReceive(freeQueue, &slotIndex, 0) != pdTRUE) {
        overflowPackets++;
        finish();
        return;
    }

    PacketSlot& slot = packetSlots[slotIndex];
    slot.sessionId = header.sessionId;
    slot.frameId = header.frameId;
    slot.mode = header.mode;
    slot.chunkIndex = header.chunkIndex;
    uint16_t copiedPayload = StreamV2::kPayloadSize;
    if (contiguous) {
        memcpy(
            slot.payload,
            static_cast<const uint8_t*>(packet->payload) + StreamV2::kHeaderSize,
            StreamV2::kPayloadSize);
    } else {
        copiedPayload = pbuf_copy_partial(
            packet, slot.payload, StreamV2::kPayloadSize, StreamV2::kHeaderSize);
    }
    if (copiedPayload != StreamV2::kPayloadSize) {
        invalidPackets++;
        releaseSlot(slotIndex);
        finish();
        return;
    }
    if (xQueueSend(readyQueue, &slotIndex, 0) != pdTRUE) {
        overflowPackets++;
        releaseSlot(slotIndex);
        finish();
        return;
    }
    markAcceptedChunk(*rxState, header.chunkIndex);
    acceptedPackets++;
    lastReceiveMs = now;
    powerSaveMode = false;
    if (header.chunkIndex + 1 == expectedChunks) {
        sendFeedback(pcb, &peerAddress, peerPort);
    }
    finish();
}

struct UdpInitCall {
    tcpip_api_call_data call;
    err_t result;
};

err_t initializeUdpApi(tcpip_api_call_data* rawCall) {
    UdpInitCall* call = reinterpret_cast<UdpInitCall*>(rawCall);
    rawUdpPcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (!rawUdpPcb) {
        call->result = ERR_MEM;
        return ERR_MEM;
    }
    ip_addr_t anyAddress;
    ip_addr_set_any(0, &anyAddress);
    call->result = udp_bind(rawUdpPcb, &anyAddress, StreamV2::kPort);
    if (call->result != ERR_OK) {
        udp_remove(rawUdpPcb);
        rawUdpPcb = nullptr;
        return call->result;
    }
    udp_recv(rawUdpPcb, onUdpPacket, nullptr);
    return ERR_OK;
}

bool startRawUdp() {
    UdpInitCall call{};
    const err_t apiResult = tcpip_api_call(initializeUdpApi, &call.call);
    return apiResult == ERR_OK && call.result == ERR_OK && rawUdpPcb;
}

err_t stopUdpApi(tcpip_api_call_data*) {
    if (rawUdpPcb) {
        udp_recv(rawUdpPcb, nullptr, nullptr);
        udp_remove(rawUdpPcb);
        rawUdpPcb = nullptr;
    }
    return ERR_OK;
}

void stopRawUdp() {
    if (!rawUdpPcb) {
        return;
    }
    tcpip_api_call_data call{};
    tcpip_api_call(stopUdpApi, &call);
}

void collectReady(TickType_t timeout) {
    uint8_t slotIndex;
    if (pendingCount < kSlotCount && xQueueReceive(readyQueue, &slotIndex, timeout) == pdTRUE) {
        pendingSlots[pendingCount++] = slotIndex;
    }
    while (pendingCount < kSlotCount && xQueueReceive(readyQueue, &slotIndex, 0) == pdTRUE) {
        pendingSlots[pendingCount++] = slotIndex;
    }
}

uint8_t takePendingAt(uint8_t position) {
    const uint8_t slotIndex = pendingSlots[position];
    for (uint8_t i = position + 1; i < pendingCount; ++i) {
        pendingSlots[i - 1] = pendingSlots[i];
    }
    pendingCount--;
    return slotIndex;
}

void pruneOtherSessions(uint32_t sessionId) {
    for (int i = pendingCount - 1; i >= 0; --i) {
        if (packetSlots[pendingSlots[i]].sessionId != sessionId) {
            releaseSlot(takePendingAt(static_cast<uint8_t>(i)));
        }
    }
}

int findPending(uint32_t sessionId, uint32_t frameId, uint8_t chunkIndex) {
    for (uint8_t i = 0; i < pendingCount; ++i) {
        const PacketSlot& slot = packetSlots[pendingSlots[i]];
        if (slot.sessionId == sessionId && slot.frameId == frameId
            && slot.chunkIndex == chunkIndex) {
            return i;
        }
    }
    return -1;
}

bool findOldestStart(uint32_t sessionId, uint32_t& frameId, uint8_t& mode) {
    bool found = false;
    for (uint8_t i = 0; i < pendingCount; ++i) {
        const PacketSlot& slot = packetSlots[pendingSlots[i]];
        if (slot.sessionId != sessionId || slot.chunkIndex != 0) {
            continue;
        }
        if (!found || StreamV2::newerFrame(frameId, slot.frameId)) {
            found = true;
            frameId = slot.frameId;
            mode = slot.mode;
        }
    }
    return found;
}

uint8_t consecutiveChunks(uint32_t sessionId, uint32_t frameId, uint8_t maximum) {
    uint8_t count = 0;
    while (count < maximum && findPending(sessionId, frameId, count) >= 0) {
        count++;
    }
    return count;
}

bool dropFrameSlots(uint32_t sessionId, uint32_t frameId) {
    bool dropped = false;
    for (int i = pendingCount - 1; i >= 0; --i) {
        const PacketSlot& slot = packetSlots[pendingSlots[i]];
        if (slot.sessionId == sessionId && slot.frameId == frameId) {
            releaseSlot(takePendingAt(static_cast<uint8_t>(i)));
            stalePackets++;
            dropped = true;
        }
    }
    return dropped;
}

void dropOlderFrames(uint32_t sessionId, uint32_t frameId) {
    bool dropped = false;
    for (int i = pendingCount - 1; i >= 0; --i) {
        const PacketSlot& slot = packetSlots[pendingSlots[i]];
        if (slot.sessionId == sessionId && slot.frameId != frameId
            && !StreamV2::newerFrame(slot.frameId, frameId)) {
            releaseSlot(takePendingAt(static_cast<uint8_t>(i)));
            stalePackets++;
            dropped = true;
        }
    }
    if (dropped) {
        incompleteFrames++;
    }
}

bool prepareNextFrame(uint32_t& sessionId, uint32_t& frameId, uint8_t& mode) {
    collectReady(pdMS_TO_TICKS(20));
    if (!streamRunning || !sessionActive) {
        return false;
    }
    sessionId = activeSession;
    pruneOtherSessions(sessionId);
    if (!findOldestStart(sessionId, frameId, mode)) {
        // If chunk 0 was the packet that overflowed, keeping chunks 1..N would
        // consume every slot forever and prevent the next frame from entering.
        // Give normal UDP reordering a short chance, then discard the orphaned
        // window so a future chunk 0 can re-synchronise the live stream.
        const int64_t orphanDeadline = esp_timer_get_time() + kRgb332MissingChunkTimeoutUs;
        while (streamRunning && pendingCount > 0 && esp_timer_get_time() < orphanDeadline) {
            collectReady(pdMS_TO_TICKS(1));
            pruneOtherSessions(sessionId);
            if (findOldestStart(sessionId, frameId, mode)) {
                break;
            }
        }
        if (!findOldestStart(sessionId, frameId, mode)) {
            const bool hadOrphans = pendingCount > 0;
            while (pendingCount > 0) {
                releaseSlot(takePendingAt(pendingCount - 1));
                stalePackets++;
            }
            if (hadOrphans) {
                incompleteFrames++;
            }
            return false;
        }
    }
    dropOlderFrames(sessionId, frameId);

    int64_t deadline = esp_timer_get_time() + kPrebufferTimeoutUs;
    const uint8_t requiredPrebuffer = prebufferChunks(mode);
    while (consecutiveChunks(sessionId, frameId, requiredPrebuffer) < requiredPrebuffer) {
        if (!streamRunning || activeSession != sessionId) {
            dropFrameSlots(sessionId, frameId);
            return false;
        }
        collectReady(pdMS_TO_TICKS(1));
        pruneOtherSessions(sessionId);
        if (esp_timer_get_time() >= deadline) {
            if (dropFrameSlots(sessionId, frameId)) {
                incompleteFrames++;
            }
            return false;
        }
    }
    return true;
}

void setPanelPixelFormat(uint8_t mode) {
    const uint8_t desired = mode == StreamV2::kModeRgb332 ? 0x03 : 0x55;
    if (panelPixelFormat == desired) {
        return;
    }
    tft->dmaWait();
    tft->writecommand(TFT_COLMOD);
    tft->writedata(desired);
    panelPixelFormat = desired;
    delayMicroseconds(50);
}

void convertRgb332(const uint8_t* source, uint8_t* destination) {
    for (int remaining = StreamV2::kPayloadSize; remaining > 0; remaining -= 2) {
        const uint32_t pair = StreamV2::packRgb444Pair(
            rgb332ToRgb444[source[0]], rgb332ToRgb444[source[1]]);
        destination[0] = static_cast<uint8_t>(pair >> 16);
        destination[1] = static_cast<uint8_t>(pair >> 8);
        destination[2] = static_cast<uint8_t>(pair);
        source += 2;
        destination += 3;
    }
}

void displayTask(void*) {
    displayTaskRunning = true;
    while (streamRunning) {
        uint32_t sessionId = 0;
        uint32_t frameId = 0;
        uint8_t mode = StreamV2::kModeRgb332;
        if (!prepareNextFrame(sessionId, frameId, mode)) {
            if (streamRunning && sessionActive && !powerSaveMode
                && millis() - lastReceiveMs > kPowerSaveTimeoutMs) {
                powerSaveMode = true;
                setPanelPixelFormat(StreamV2::kModeRgb565);
                tft->fillScreen(TFT_BLACK);
            }
            continue;
        }

        const uint8_t expectedChunks = StreamV2::chunksForMode(mode);
        bool complete = true;
        setPanelPixelFormat(mode);
        tft->startWrite();
        tft->setAddrWindow(0, 0, kImageWidth, kImageHeight);

        for (uint8_t groupStart = 0; groupStart < expectedChunks; groupStart += kDmaGroupChunks) {
            const uint8_t remainingChunks = expectedChunks - groupStart;
            const uint8_t groupChunks = remainingChunks < kDmaGroupChunks
                ? remainingChunks : kDmaGroupChunks;
            tft->dmaWait();
            uint8_t copiedChunks = 0;

            for (; copiedChunks < groupChunks; ++copiedChunks) {
                const uint8_t expected = groupStart + copiedChunks;
                int position = findPending(sessionId, frameId, expected);
                const int64_t deadline = esp_timer_get_time() + missingChunkTimeoutUs(mode);
                while (streamRunning && position < 0 && activeSession == sessionId
                    && esp_timer_get_time() < deadline) {
                    collectReady(pdMS_TO_TICKS(1));
                    pruneOtherSessions(sessionId);
                    position = findPending(sessionId, frameId, expected);
                    if (position < 0 && queueDepth() >= kSlotCount - 8) {
                        break;
                    }
                }
                if (position < 0 || activeSession != sessionId) {
                    complete = false;
                    break;
                }

                const uint8_t slotIndex = takePendingAt(static_cast<uint8_t>(position));
                PacketSlot& slot = packetSlots[slotIndex];
                if (mode == StreamV2::kModeRgb332) {
                    convertRgb332(
                        slot.payload,
                        streamDmaBuffer + copiedChunks * kRgb332DmaBytesPerChunk);
                } else {
                    memcpy(
                        streamDmaBuffer + copiedChunks * StreamV2::kPayloadSize,
                        slot.payload,
                        StreamV2::kPayloadSize);
                }
                releaseSlot(slotIndex);
            }
            if (!complete) {
                break;
            }
            const size_t dmaBytes = groupChunks * (
                mode == StreamV2::kModeRgb332
                    ? kRgb332DmaBytesPerChunk : StreamV2::kPayloadSize);
            tft->pushPixelsDMA(
                reinterpret_cast<uint16_t*>(streamDmaBuffer), dmaBytes / 2);
        }

        tft->dmaWait();
        tft->endWrite();

        if (activeSession != sessionId) {
            dropFrameSlots(sessionId, frameId);
            continue;
        }
        if (complete) {
            latestDisplayedFrame = frameId;
            displayedFrames++;
            tcpip_callback(sendDisplayedFeedback, nullptr);
        } else {
            incompleteFrames++;
            dropFrameSlots(sessionId, frameId);
        }
    }
    tft->dmaWait();
    displayTaskRunning = false;
    displayTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

void initializeRgb332Lut() {
    for (uint16_t value = 0; value < 256; ++value) {
        rgb332ToRgb444[value] = StreamV2::rgb332ToRgb444(value);
    }
}

void printDebugInfo() {
    static uint32_t lastPrintMs = 0;
    static uint32_t previousRx = 0;
    static uint32_t previousAccepted = 0;
    static uint32_t previousDisplayed = 0;
    static uint32_t previousSession = 0;
    const uint32_t now = millis();
    if (now - lastPrintMs < 2000) {
        return;
    }
    const uint32_t elapsed = lastPrintMs == 0 ? now : now - lastPrintMs;
    if (activeSession != previousSession) {
        previousSession = activeSession;
        previousRx = rxPackets;
        previousAccepted = acceptedPackets;
        previousDisplayed = displayedFrames;
    }
    const float scale = elapsed ? 1000.0f / elapsed : 0.0f;
    Serial.printf(
        "V2 session=%08x mode=%s rx=%.0fpps accepted=%.0fpps displayed=%.1ffps "
        "queue=%u/%u overflow=%u invalid=%u stale=%u incomplete=%u heap=%u\n",
        activeSession,
        activeMode == StreamV2::kModeRgb332 ? "RGB332" : "RGB565",
        (rxPackets - previousRx) * scale,
        (acceptedPackets - previousAccepted) * scale,
        (displayedFrames - previousDisplayed) * scale,
        queueDepth(),
        kSlotCount,
        overflowPackets,
        invalidPackets,
        stalePackets,
        incompleteFrames,
        ESP.getFreeHeap());
    previousRx = rxPackets;
    previousAccepted = acceptedPackets;
    previousDisplayed = displayedFrames;
    lastPrintMs = now;
}

}  // namespace

static bool allocateStreamResources() {
    freeQueue = xQueueCreateStatic(
        kSlotCount, sizeof(uint8_t), freeQueueStorage, &freeQueueState);
    readyQueue = xQueueCreateStatic(
        kSlotCount, sizeof(uint8_t), readyQueueStorage, &readyQueueState);
    packetSlots = static_cast<PacketSlot*>(heap_caps_calloc(
        kSlotCount, sizeof(PacketSlot), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    streamDmaBuffer = static_cast<uint8_t*>(heap_caps_malloc(
        kDmaBufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!freeQueue || !readyQueue || !packetSlots
        || !streamDmaBuffer) {
        Serial.println("ScreenShare V2: stream buffer allocation failed");
        return false;
    }
    for (uint8_t i = 0; i < kSlotCount; ++i) {
        xQueueSend(freeQueue, &i, 0);
    }

    pendingCount = 0;
    peerValid = false;
    sessionActive = false;
    activeSession = 0;
    activeMode = StreamV2::kModeRgb332;
    sessionGeneration = 0;
    feedbackSequence = 0;
    latestRxFrame = StreamV2::kNoFrame;
    latestCompleteFrame = StreamV2::kNoFrame;
    latestDisplayedFrame = StreamV2::kNoFrame;
    rxPackets = 0;
    acceptedPackets = 0;
    overflowPackets = 0;
    invalidPackets = 0;
    stalePackets = 0;
    incompleteFrames = 0;
    displayedFrames = 0;
    powerSaveMode = false;
    haveLatestTrackedFrame = false;
    memset(rxFrameStates, 0, sizeof(rxFrameStates));
    initializeRgb332Lut();
    lastReceiveMs = millis();
    panelPixelFormat = 0x55;
    return true;
}

static void releaseStreamResources() {
    if (streamDmaBuffer) {
        heap_caps_free(streamDmaBuffer);
        streamDmaBuffer = nullptr;
    }
    if (packetSlots) {
        heap_caps_free(packetSlots);
        packetSlots = nullptr;
    }
    if (freeQueue) {
        vQueueDelete(freeQueue);
        freeQueue = nullptr;
    }
    if (readyQueue) {
        vQueueDelete(readyQueue);
        readyQueue = nullptr;
    }
}

static bool startStreamReceiver() {
    if (!freeQueue || !readyQueue || !packetSlots || !streamDmaBuffer) {
        return false;
    }
    streamRunning = true;
    if (!startRawUdp()) {
        streamRunning = false;
        return false;
    }

    const BaseType_t taskResult = xTaskCreatePinnedToCore(
        displayTask,
        "display_v2",
        4096,
        nullptr,
        3,
        &displayTaskHandle,
        1);
    if (taskResult != pdPASS) {
        streamRunning = false;
        stopRawUdp();
        displayTaskHandle = nullptr;
        return false;
    }
    return true;
}

static void stopStreamReceiver() {
    streamRunning = false;
    stopRawUdp();
    for (uint8_t attempt = 0; displayTaskHandle && attempt < 100; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (displayTaskHandle) {
        vTaskDelete(displayTaskHandle);
        displayTaskRunning = false;
        displayTaskHandle = nullptr;
    }
    tft->dmaWait();
    setPanelPixelFormat(StreamV2::kModeRgb565);
}

static int screenShareInit(AppController*) {
    readConfig(&cfgData);
    setCpuFrequencyMhz(cfgData.powerFlag == 0 ? 160 : 240);

    RgbParam rgbSetting = {
        LED_MODE_HSV, 0, 128, 32, 255, 255, 32, 1, 1, 1, 150, 250, 1, 30};
    set_rgb_and_run(&rgbSetting);
    screen_share_udp_gui_init();

    runData = static_cast<ScreenShareAppRunData*>(calloc(1, sizeof(ScreenShareAppRunData)));
    if (!runData) {
        display_screen_share_udp(
            "Screen Share UDP V2", "-", "8888", "Memory allocation failed",
            LV_SCR_LOAD_ANIM_NONE);
        return 0;
    }
    runData->tftSwapStatus = tft->getSwapBytes();
    esp_wifi_get_ps(&runData->previousWifiPowerSave);
    tft->initDMA();
    tft->setSwapBytes(false);

    if (!allocateStreamResources()) {
        releaseStreamResources();
        display_screen_share_udp(
            "Screen Share UDP V2", WiFi.localIP().toString().c_str(), "8888",
            "Not enough internal RAM", LV_SCR_LOAD_ANIM_NONE);
    }
    Serial.printf("ScreenShare UDP V2 initialized, heap=%u\n", ESP.getFreeHeap());
    return 0;
}

static void screenShareProcess(AppController* system, const ImuAction* action) {
    if (action->active == RETURN) {
        system->app_exit();
        return;
    }
    if (!runData || !freeQueue) {
        return;
    }
    if (!runData->udpStarted && !runData->wifiRequested) {
        display_screen_share_udp(
            "Screen Share UDP V2", WiFi.localIP().toString().c_str(), "8888",
            "WiFi connecting...", LV_SCR_LOAD_ANIM_NONE);
        system->send_to(
            SCREEN_SHARE_APP_NAME, CTRL_NAME, APP_MESSAGE_WIFI_CONN, nullptr, nullptr);
        runData->wifiRequested = true;
    } else if (runData->udpStarted) {
        if (doDelayMillisTime(
                SHARE_WIFI_ALIVE, &runData->previousWifiAliveMs, false)) {
            system->send_to(
                SCREEN_SHARE_APP_NAME, CTRL_NAME, APP_MESSAGE_WIFI_ALIVE, nullptr, nullptr);
        }
        printDebugInfo();
    }
}

static void screenShareBackgroundTask(AppController*, const ImuAction*) {}

static int screenShareExit(void*) {
    stopStreamReceiver();
    releaseStreamResources();
    screen_share_udp_gui_del();

    if (runData) {
        tft->setSwapBytes(runData->tftSwapStatus);
        esp_wifi_set_ps(runData->previousWifiPowerSave);
        free(runData);
        runData = nullptr;
    }

    RgbParam rgbSetting = {
        LED_MODE_HSV, 1, 32, 255, 255, 255, 255, 1, 1, 1, 150, 250, 1, 30};
    set_rgb_and_run(&rgbSetting);
    return 0;
}

static void screenShareMessageHandle(
    const char*, const char*, APP_MESSAGE_TYPE type, void* message, void* extInfo) {
    switch (type) {
    case APP_MESSAGE_WIFI_CONN:
        if (!runData || runData->udpStarted || !freeQueue) {
            break;
        }
        // The receiver is a STA-side UDP service. Never start it on an AP or
        // before the configured WiFi network has supplied a valid address.
        if (WiFi.status() != WL_CONNECTED ||
            (WiFi.getMode() & WIFI_MODE_STA) != WIFI_MODE_STA ||
            WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
            runData->wifiRequested = false;
            display_screen_share_udp(
                "Screen Share UDP V2", "-", "8888",
                "WiFi connection failed", LV_SCR_LOAD_ANIM_NONE);
            break;
        }
        WiFi.setSleep(false);
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (startStreamReceiver()) {
            runData->udpStarted = true;
            display_screen_share_udp(
                "Screen Share UDP V2", WiFi.localIP().toString().c_str(), "8888",
                "Ready: RGB332 / RGB565", LV_SCR_LOAD_ANIM_NONE);
            Serial.printf(
                "UDP V2 ready at %s:%u, slots=%u, heap=%u\n",
                WiFi.localIP().toString().c_str(), StreamV2::kPort,
                kSlotCount, ESP.getFreeHeap());
        } else {
            display_screen_share_udp(
                "Screen Share UDP V2", WiFi.localIP().toString().c_str(), "8888",
                "UDP receiver start failed", LV_SCR_LOAD_ANIM_NONE);
        }
        break;
    case APP_MESSAGE_WIFI_ALIVE:
        break;
    case APP_MESSAGE_GET_PARAM: {
        const char* key = static_cast<const char*>(message);
        if (!strcmp(key, "powerFlag")) {
            snprintf(static_cast<char*>(extInfo), 32, "%u", cfgData.powerFlag);
        } else {
            snprintf(static_cast<char*>(extInfo), 32, "%s", "NULL");
        }
        break;
    }
    case APP_MESSAGE_SET_PARAM: {
        const char* key = static_cast<const char*>(message);
        if (!strcmp(key, "powerFlag")) {
            cfgData.powerFlag = atol(static_cast<const char*>(extInfo));
        }
        break;
    }
    case APP_MESSAGE_READ_CFG:
        readConfig(&cfgData);
        break;
    case APP_MESSAGE_WRITE_CFG:
        writeConfig(&cfgData);
        break;
    default:
        break;
    }
}

APP_OBJ screen_share_udp_app = {
    SCREEN_SHARE_APP_NAME,
    &app_screen_share_udp,
    "Stream Protocol V2",
    screenShareInit,
    screenShareProcess,
    screenShareBackgroundTask,
    screenShareExit,
    screenShareMessageHandle};
