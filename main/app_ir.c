#include <string.h>
#include "app_ir.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "esp_check.h"

#define TAG "ir"

#define IR_RESOLUTION_HZ    1000000U   /* 1 RMT tick = 1 us */
#define IR_RX_GPIO          CONFIG_IR_MONITOR_IR_RX_GPIO
#define IR_BUF_SYMBOLS      512
#define IR_RX_MIN_PULSE_NS  1000U      /* glitches < 1 us are ignored */
#define IR_RX_TIMEOUT_NS    50000000U  /* idle gap > 50 ms ends the frame */

#define NEC_LEAD_PULSE_US   9000U
#define NEC_LEAD_SPACE_US   4500U
#define NEC_REPEAT_SPACE_US 2250U
#define NEC_BIT_PULSE_US    560U
#define NEC_BIT0_SPACE_US   560U
#define NEC_BIT1_SPACE_US   1690U

typedef struct {
    uint16_t dur; /* duration in us */
    uint8_t level;
} ir_seg_t;

static rmt_channel_handle_t s_rx_ch;
static rmt_receive_config_t s_rx_cfg;
static rmt_symbol_word_t s_rx_buf[IR_BUF_SYMBOLS];
static QueueHandle_t s_rx_queue;
static SemaphoreHandle_t s_mutex;
static ir_frame_t s_frame;
static bool s_frame_new;
static uint32_t s_seq;
static ir_seg_t s_segs[IR_RAW_MAX_SEGS];

static inline bool dur_in_range(uint32_t v, uint32_t nom, uint32_t margin)
{
    return (v >= nom - margin) && (v <= nom + margin);
}

static bool ir_rx_done_cb(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *edata, void *udata)
{
    BaseType_t woken = pdFALSE;
    QueueHandle_t q = (QueueHandle_t)udata;
    (void)ch;
    xQueueSendFromISR(q, edata, &woken);
    return woken == pdTRUE;
}

static void ir_analyze(const rmt_symbol_word_t *sym, size_t num, ir_frame_t *f)
{
    /* flatten RMT symbol pairs into an alternating level/duration list */
    int n = 0;
    for (size_t i = 0; i < num && n < IR_RAW_MAX_SEGS; i++) {
        if (sym[i].duration0 > 0) {
            s_segs[n].level = sym[i].level0;
            s_segs[n].dur = sym[i].duration0;
            n++;
        }
        if (n < IR_RAW_MAX_SEGS && sym[i].duration1 > 0) {
            s_segs[n].level = sym[i].level1;
            s_segs[n].dur = sym[i].duration1;
            n++;
        }
    }

    /* trim leading idle and the trailing idle-to-timeout tail */
    int start = 0;
    int end = n;
    if (end - start >= 2 && s_segs[start].dur > 15000) {
        start++;
    }
    if (end - start >= 2 && s_segs[end - 1].dur > 10000) {
        end--;
    }

    /* raw signal features */
    f->seg_count = (uint32_t)(end - start);
    uint64_t total = 0;
    for (int i = start; i < end; i++) {
        total += s_segs[i].dur;
    }
    f->total_us = (uint32_t)total;

    f->leader_pulse_us = 0;
    f->leader_space_us = 0;
    f->last_gap_us = 0;
    if (end - start >= 1) {
        f->leader_pulse_us = s_segs[start].dur;
    }
    if (end - start >= 2) {
        f->leader_space_us = s_segs[start + 1].dur;
    }
    if (end - start >= 1) {
        f->last_gap_us = s_segs[end - 1].dur;
    }

    f->pulse_count = 0;
    f->min_pulse_us = 0;
    f->max_pulse_us = 0;
    for (int i = start; i < end; i += 2) {
        f->pulse_count++;
        if (i == start) {
            continue; /* exclude the leader from min/max */
        }
        if (f->min_pulse_us == 0 || s_segs[i].dur < f->min_pulse_us) {
            f->min_pulse_us = s_segs[i].dur;
        }
        if (s_segs[i].dur > f->max_pulse_us) {
            f->max_pulse_us = s_segs[i].dur;
        }
    }

    /* NEC decode: scan for the 9 ms leader (polarity agnostic) */
    f->nec_ok = false;
    f->nec_repeat = false;
    f->nec_chksum_ok = false;
    f->nec_ext_addr = false;
    f->nec_bits = 0;
    f->nec_addr = 0;
    f->nec_cmd = 0;
    f->nec_raw = 0;

    for (int i = start; i + 1 < end; i++) {
        if (s_segs[i + 1].level == s_segs[i].level) {
            continue;
        }
        if (!dur_in_range(s_segs[i].dur, NEC_LEAD_PULSE_US, 1200)) {
            continue;
        }

        /* repeat code: 9ms + 2.25ms + 560us */
        if (dur_in_range(s_segs[i + 1].dur, NEC_REPEAT_SPACE_US, 500)) {
            if (i + 2 < end && dur_in_range(s_segs[i + 2].dur, NEC_BIT_PULSE_US, 300)) {
                f->nec_ok = true;
                f->nec_repeat = true;
            }
            return;
        }
        if (!dur_in_range(s_segs[i + 1].dur, NEC_LEAD_SPACE_US, 700)) {
            continue;
        }

        /* 32 data bits, LSB first: (pulse, space) pairs */
        uint32_t raw = 0;
        bool good = true;
        for (int b = 0; b < 32; b++) {
            int p = i + 2 + b * 2;
            if (p + 1 >= end || !dur_in_range(s_segs[p].dur, NEC_BIT_PULSE_US, 300)) {
                good = false;
                break;
            }
            if (dur_in_range(s_segs[p + 1].dur, NEC_BIT0_SPACE_US, 350)) {
                /* logic 0 */
            } else if (dur_in_range(s_segs[p + 1].dur, NEC_BIT1_SPACE_US, 450)) {
                raw |= (1UL << b);
            } else {
                good = false;
                break;
            }
        }
        if (good) {
            uint8_t a = raw & 0xFF;
            uint8_t ai = (raw >> 8) & 0xFF;
            uint8_t c = (raw >> 16) & 0xFF;
            uint8_t ci = (raw >> 24) & 0xFF;
            f->nec_ok = true;
            f->nec_bits = 32;
            f->nec_raw = raw;
            if (ai == (uint8_t)~a && ci == (uint8_t)~c) {
                f->nec_chksum_ok = true;
                f->nec_addr = a;
                f->nec_cmd = c;
            } else if (ci == (uint8_t)~c) {
                /* extended NEC with 16-bit address */
                f->nec_ext_addr = true;
                f->nec_addr = raw & 0xFFFF;
                f->nec_cmd = c;
            } else {
                f->nec_addr = a;
                f->nec_cmd = c;
            }
        }
        return;
    }
}

static void ir_task(void *arg)
{
    rmt_rx_done_event_data_t ev;
    for (;;) {
        if (xQueueReceive(s_rx_queue, &ev, portMAX_DELAY) != pdPASS) {
            continue;
        }

        ir_frame_t fr;
        memset(&fr, 0, sizeof(fr));
        size_t num = ev.num_symbols;
        if (num > IR_BUF_SYMBOLS) {
            num = IR_BUF_SYMBOLS;
        }
        ir_analyze(ev.received_symbols, num, &fr);
        fr.seq = ++s_seq;
        fr.valid = true;

        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            s_frame = fr;
            s_frame_new = true;
            xSemaphoreGive(s_mutex);
        }

        rmt_receive(s_rx_ch, s_rx_buf, sizeof(s_rx_buf), &s_rx_cfg);
    }
}

esp_err_t ir_capture_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    s_rx_queue = xQueueCreate(4, sizeof(rmt_rx_done_event_data_t));
    if (!s_rx_queue) {
        return ESP_ERR_NO_MEM;
    }

    rmt_rx_channel_config_t ch_cfg = {
        .gpio_num = IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = 256,
        .intr_priority = 0,
        .flags = {
            .invert_in = false,
            .with_dma = true,
            .allow_pd = false,
        },
    };
    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&ch_cfg, &s_rx_ch), TAG, "create RMT RX channel");

    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = ir_rx_done_cb,
    };
    ESP_RETURN_ON_ERROR(rmt_rx_register_event_callbacks(s_rx_ch, &cbs, s_rx_queue),
                        TAG, "register RMT RX callback");

    s_rx_cfg.signal_range_min_ns = IR_RX_MIN_PULSE_NS;
    s_rx_cfg.signal_range_max_ns = IR_RX_TIMEOUT_NS;
    s_rx_cfg.flags.en_partial_rx = 0;

    ESP_RETURN_ON_ERROR(rmt_enable(s_rx_ch), TAG, "enable RMT RX");
    ESP_RETURN_ON_ERROR(rmt_receive(s_rx_ch, s_rx_buf, sizeof(s_rx_buf), &s_rx_cfg),
                        TAG, "start RMT receive");

    if (xTaskCreate(ir_task, "ir_task", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool ir_get_frame(ir_frame_t *out)
{
    bool got = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (s_frame_new) {
            *out = s_frame;
            s_frame_new = false;
            got = true;
        }
        xSemaphoreGive(s_mutex);
    }
    return got;
}
