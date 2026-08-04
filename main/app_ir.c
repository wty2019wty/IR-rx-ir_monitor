#include <string.h>
#include <sys/stat.h>
#include "app_ir.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_spiffs.h"
#include "esp_timer.h"

#define TAG "ir"

#define IR_RESOLUTION_HZ    500000U    /* 1 RMT tick = 2 us; slower clock allows a longer 50ms idle timeout */
#define IR_RX_GPIO          CONFIG_IR_MONITOR_IR_RX_GPIO
#define IR_TX_GPIO          CONFIG_IR_MONITOR_IR_TX_GPIO
#define IR_BUF_SYMBOLS      256        /* Without DMA, use smaller buffer */
#define IR_RX_MIN_PULSE_NS  1000U      /* glitches < 1 us are ignored */
#define IR_RX_TIMEOUT_NS    50000000U  /* idle gap > 50 ms ends the frame */

#define NEC_LEAD_PULSE_US   9000U
#define NEC_LEAD_SPACE_US   4500U
#define NEC_REPEAT_SPACE_US 2250U
#define NEC_BIT_PULSE_US    560U
#define NEC_BIT0_SPACE_US   560U
#define NEC_BIT1_SPACE_US   1690U

#define IR_CARRIER_FREQ_HZ  CONFIG_IR_MONITOR_CARRIER_FREQ_HZ
#define IR_CARRIER_DUTY     CONFIG_IR_MONITOR_CARRIER_DUTY

#define IR_RECORDING_DIR    "/spiffs"
#define IR_MAX_SYMBOLS_PER_RECORDING 2048

typedef struct {
    uint32_t dur; /* duration in us */
    uint8_t level;
} ir_seg_t;

/* RX variables */
static rmt_channel_handle_t s_rx_ch;
static rmt_receive_config_t s_rx_cfg;
static rmt_symbol_word_t s_rx_buf[IR_BUF_SYMBOLS];
static SemaphoreHandle_t s_mutex;
static ir_frame_t s_frame;
static bool s_frame_new;
static uint32_t s_seq;
static ir_seg_t s_segs[IR_RAW_MAX_SEGS];

/* TX variables */
static rmt_channel_handle_t s_tx_ch = NULL;
static rmt_transmit_config_t s_tx_cfg;
static rmt_symbol_word_t *s_tx_buf = NULL;

/* Last frame buffer for "save on pause" feature */
static rmt_symbol_word_t s_last_frame_buf[IR_BUF_SYMBOLS];
static uint32_t s_last_frame_symbols = 0;

/* Frozen snapshot: captured when pause is entered, never overwritten until next freeze */
static rmt_symbol_word_t s_frozen_frame_buf[IR_BUF_SYMBOLS];
static uint32_t s_frozen_frame_symbols = 0;

/* Playback variables */
static bool s_playing = false;
static float s_playback_progress = 0.0f;
static TaskHandle_t s_playback_task_handle = NULL;

/* Storage variables */
static bool s_storage_initialized = false;
static uint32_t s_saved_recording_count = 0;

static inline bool dur_in_range(uint32_t v, uint32_t nom, uint32_t margin)
{
    return (v >= nom - margin) && (v <= nom + margin);
}

/* Use simple flag + copy instead of FreeRTOS queue to avoid ISR compatibility issues */
static volatile bool s_rx_done = false;
static rmt_rx_done_event_data_t s_rx_data;

static bool ir_rx_done_cb(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *edata, void *udata)
{
    (void)ch;
    (void)udata;
    /* Copy data directly instead of using queue */
    s_rx_data = *edata;
    s_rx_done = true;
    return true; /* request context switch so ir_task wakes immediately */
}

/* Helper function to get protocol string */
const char *ir_protocol_str(uint8_t protocol)
{
    switch (protocol) {
        case 1: return "NEC";
        case 2: return "NEC RPT";
        case 3: return "NEC EXT";
        default: return "UNKNOWN";
    }
}

/* Save recording to SPIFFS file */
static esp_err_t ir_save_recording_to_file(uint32_t index, const ir_recording_header_t *header, const rmt_symbol_word_t *symbols)
{
    char filepath[64];
    snprintf(filepath, sizeof(filepath), "%s/ir_%03lu.bin", IR_RECORDING_DIR, (unsigned long)index);

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        return ESP_FAIL;
    }

    /* Write header */
    size_t written = fwrite(header, sizeof(ir_recording_header_t), 1, f);
    if (written != 1) {
        ESP_LOGE(TAG, "Failed to write header");
        fclose(f);
        return ESP_FAIL;
    }

    /* Write symbols */
    written = fwrite(symbols, sizeof(rmt_symbol_word_t), header->symbol_count, f);
    if (written != header->symbol_count) {
        ESP_LOGE(TAG, "Failed to write symbols");
        fclose(f);
        return ESP_FAIL;
    }

    fclose(f);
    ESP_LOGI(TAG, "Saved recording %lu to %s (%lu symbols)", (unsigned long)index, filepath, (unsigned long)header->symbol_count);
    return ESP_OK;
}

/* Load recording from SPIFFS file */
static esp_err_t ir_load_recording_from_file(uint32_t index, ir_recording_header_t *header, rmt_symbol_word_t *symbols, uint32_t max_symbols)
{
    char filepath[64];
    snprintf(filepath, sizeof(filepath), "%s/ir_%03lu.bin", IR_RECORDING_DIR, (unsigned long)index);

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", filepath);
        return ESP_FAIL;
    }

    /* Read header */
    size_t read_count = fread(header, sizeof(ir_recording_header_t), 1, f);
    if (read_count != 1) {
        ESP_LOGE(TAG, "Failed to read header");
        fclose(f);
        return ESP_FAIL;
    }

    /* Validate magic */
    if (header->magic != IR_RECORDING_MAGIC) {
        ESP_LOGE(TAG, "Invalid recording file magic");
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    /* Read symbols */
    uint32_t symbols_to_read = header->symbol_count;
    if (symbols_to_read > max_symbols) {
        symbols_to_read = max_symbols;
    }

    read_count = fread(symbols, sizeof(rmt_symbol_word_t), symbols_to_read, f);
    if (read_count != symbols_to_read) {
        ESP_LOGE(TAG, "Failed to read symbols");
        fclose(f);
        return ESP_FAIL;
    }

    fclose(f);
    return ESP_OK;
}

/* Delete recording file */
static esp_err_t ir_delete_recording_file(uint32_t index)
{
    char filepath[64];
    snprintf(filepath, sizeof(filepath), "%s/ir_%03lu.bin", IR_RECORDING_DIR, (unsigned long)index);

    /* Check if file exists first */
    struct stat st;
    if (stat(filepath, &st) != 0) {
        return ESP_ERR_NOT_FOUND;  /* File doesn't exist, not an error */
    }

    if (remove(filepath) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", filepath);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleted recording %lu", (unsigned long)index);
    return ESP_OK;
}

/* Count saved recordings in SPIFFS */
static uint32_t ir_count_saved_recordings(void)
{
    uint32_t count = 0;
    char filepath[64];

    for (uint32_t i = 0; i < IR_MAX_RECORDINGS; i++) {
        snprintf(filepath, sizeof(filepath), "%s/ir_%03lu.bin", IR_RECORDING_DIR, (unsigned long)i);
        struct stat st;
        if (stat(filepath, &st) == 0) {
            count++;
        }
        /* Yield every call to prevent watchdog timeout — SPIFFS stat is slow */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return count;
}

/* Find next free recording index. Returns ESP_ERR_NO_SPACE if all slots are full. */
static esp_err_t ir_find_free_index(uint32_t *out_index)
{
    char filepath[64];
    struct stat st;

    for (uint32_t i = 0; i < IR_MAX_RECORDINGS; i++) {
        snprintf(filepath, sizeof(filepath), "%s/ir_%03lu.bin", IR_RECORDING_DIR, (unsigned long)i);
        if (stat(filepath, &st) != 0) {
            *out_index = i;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGE(TAG, "No free recording slot (max %d)", IR_MAX_RECORDINGS);
    return ESP_ERR_NO_MEM;
}

static void ir_analyze(const rmt_symbol_word_t *sym, size_t num, ir_frame_t *f)
{
    /* flatten RMT symbol pairs into an alternating level/duration list */
    int n = 0;
    for (size_t i = 0; i < num && n < IR_RAW_MAX_SEGS; i++) {
        if (sym[i].duration0 > 0) {
            s_segs[n].level = sym[i].level0;
            s_segs[n].dur = (uint32_t)sym[i].duration0 * 2;
            n++;
        }
        if (n < IR_RAW_MAX_SEGS && sym[i].duration1 > 0) {
            s_segs[n].level = sym[i].level1;
            s_segs[n].dur = (uint32_t)sym[i].duration1 * 2;
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
    (void)arg;
    for (;;) {
        /* Poll for RX done flag; callback returns true so we wake on event */
        if (!s_rx_done) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        /* Read symbol data BEFORE clearing flag to avoid race with ISR */
        rmt_rx_done_event_data_t ev = s_rx_data;
        s_rx_done = false;

        /* Process received data */
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
            /* Always save last frame raw data for "save on pause" feature */
            if (num > 0) {
                memcpy(s_last_frame_buf, ev.received_symbols, num * sizeof(rmt_symbol_word_t));
                s_last_frame_symbols = num;
            }
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

    /* Allocate TX buffer */
    s_tx_buf = heap_caps_malloc(IR_MAX_SYMBOLS_PER_RECORDING * sizeof(rmt_symbol_word_t), MALLOC_CAP_DEFAULT);
    if (!s_tx_buf) {
        ESP_LOGE(TAG, "Failed to allocate TX buffer");
        return ESP_ERR_NO_MEM;
    }

    /* Configure RMT RX channel with DMA */
    rmt_rx_channel_config_t rx_ch_cfg = {
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
    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&rx_ch_cfg, &s_rx_ch), TAG, "create RMT RX channel");

    /* Register callback without using FreeRTOS queue */
    rmt_rx_event_callbacks_t rx_cbs = {
        .on_recv_done = ir_rx_done_cb,
    };
    ESP_RETURN_ON_ERROR(rmt_rx_register_event_callbacks(s_rx_ch, &rx_cbs, NULL),
                        TAG, "register RMT RX callback");

    s_rx_cfg.signal_range_min_ns = IR_RX_MIN_PULSE_NS;
    s_rx_cfg.signal_range_max_ns = IR_RX_TIMEOUT_NS;
    s_rx_cfg.flags.en_partial_rx = 0;

    /* Configure RMT TX channel with DMA */
    rmt_tx_channel_config_t tx_ch_cfg = {
        .gpio_num = IR_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = 256,
        .trans_queue_depth = 4,
        .flags = {
            .invert_out = false,
            .with_dma = true,
            .allow_pd = false,
        },
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_ch_cfg, &s_tx_ch), TAG, "create RMT TX channel");

    /* Apply carrier to TX channel */
    rmt_carrier_config_t carrier_cfg = {
        .duty_cycle = IR_CARRIER_DUTY / 100.0f,
        .frequency_hz = IR_CARRIER_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(rmt_apply_carrier(s_tx_ch, &carrier_cfg), TAG, "apply carrier");

    s_tx_cfg.loop_count = 0; /* no loop */

    /* Enable TX channel only - RX will be started later by ir_start_receive() */
    ESP_RETURN_ON_ERROR(rmt_enable(s_tx_ch), TAG, "enable RMT TX");

    /* Create IR task */
    if (xTaskCreate(ir_task, "ir_task", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "IR capture initialized (RX: GPIO%d, TX: GPIO%d)", IR_RX_GPIO, IR_TX_GPIO);
    return ESP_OK;
}

esp_err_t ir_start_receive(void)
{
    /* Enable RX channel */
    ESP_RETURN_ON_ERROR(rmt_enable(s_rx_ch), TAG, "enable RMT RX");

    /* Start receiving */
    ESP_RETURN_ON_ERROR(rmt_receive(s_rx_ch, s_rx_buf, sizeof(s_rx_buf), &s_rx_cfg),
                        TAG, "start RMT receive");

    ESP_LOGI(TAG, "IR receive started");
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

/* Storage initialization */
esp_err_t ir_storage_init(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS storage");

    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }

    /* Count existing recordings */
    s_saved_recording_count = ir_count_saved_recordings();
    ESP_LOGI(TAG, "Found %lu saved recordings", (unsigned long)s_saved_recording_count);

    s_storage_initialized = true;
    return ESP_OK;
}

uint32_t ir_get_saved_recording_count(void)
{
    return s_saved_recording_count;
}

esp_err_t ir_get_recording_info(uint32_t index, ir_recording_info_t *info)
{
    if (!s_storage_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (index >= IR_MAX_RECORDINGS) {
        return ESP_ERR_INVALID_ARG;
    }

    char filepath[64];
    snprintf(filepath, sizeof(filepath), "%s/ir_%03lu.bin", IR_RECORDING_DIR, (unsigned long)index);

    ir_recording_header_t header;
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        info->valid = false;
        return ESP_ERR_NOT_FOUND;
    }

    size_t read_count = fread(&header, sizeof(header), 1, f);
    fclose(f);

    if (read_count != 1 || header.magic != IR_RECORDING_MAGIC) {
        info->valid = false;
        return ESP_ERR_INVALID_ARG;
    }

    info->index = index;
    info->symbol_count = header.symbol_count;
    info->total_duration_us = header.total_duration_us;
    info->protocol = header.protocol;
    info->nec_addr = header.nec_addr;
    info->nec_cmd = header.nec_cmd;
    info->valid = true;

    return ESP_OK;
}

esp_err_t ir_delete_recording(uint32_t index)
{
    if (!s_storage_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (index >= IR_MAX_RECORDINGS) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ir_delete_recording_file(index);
    if (ret == ESP_OK) {
        if (s_saved_recording_count > 0) {
            s_saved_recording_count--;
        }
    }

    return ret;
}

esp_err_t ir_delete_all_recordings(void)
{
    if (!s_storage_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    for (uint32_t i = 0; i < IR_MAX_RECORDINGS; i++) {
        ir_delete_recording_file(i);
    }

    s_saved_recording_count = 0;
    ESP_LOGI(TAG, "Deleted all recordings");
    return ESP_OK;
}

/* Playback task */
static void ir_playback_task(void *arg)
{
    uint32_t index = (uint32_t)(uintptr_t)arg;
    s_playing = true;
    s_playback_progress = 0.0f;

    ESP_LOGI(TAG, "Starting playback of recording %lu", (unsigned long)index);

    /* Load recording */
    ir_recording_header_t header;
    esp_err_t ret = ir_load_recording_from_file(index, &header, s_tx_buf, IR_MAX_SYMBOLS_PER_RECORDING);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load recording");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Loaded recording with %lu symbols", (unsigned long)header.symbol_count);

    /* Durations in the file are already in RMT ticks at the same 500kHz
     * (2us/tick) resolution used by the TX channel, so no time conversion
     * is needed.
     * VS1838B outputs active-low baseband, so recorded bursts are level 0.
     * RMT TX only emits the carrier during high level, so invert the levels
     * to make bursts (carrier on) level 1 and spaces level 0.
     */
    for (uint32_t i = 0; i < header.symbol_count; i++) {
        s_tx_buf[i].level0 = !s_tx_buf[i].level0;
        s_tx_buf[i].level1 = !s_tx_buf[i].level1;
    }

    /* Create copy encoder for raw symbols */
    rmt_encoder_handle_t copy_encoder = NULL;
    rmt_copy_encoder_config_t copy_cfg = {};
    ret = rmt_new_copy_encoder(&copy_cfg, &copy_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create copy encoder: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    /* Transmit */
    ret = rmt_transmit(s_tx_ch, copy_encoder, s_tx_buf, header.symbol_count * sizeof(rmt_symbol_word_t), &s_tx_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start transmission: %s", esp_err_to_name(ret));
    } else {
        /* Wait for transmission to complete */
        ret = rmt_tx_wait_all_done(s_tx_ch, pdMS_TO_TICKS(5000));
        if (ret == ESP_OK) {
            s_playback_progress = 1.0f;
        } else {
            ESP_LOGE(TAG, "Transmission timeout");
        }
    }

    /* Cleanup encoder (always executed on any exit path) */
    rmt_del_encoder(copy_encoder);

cleanup:
    ESP_LOGI(TAG, "Playback completed");
    s_playing = false;
    s_playback_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t ir_playback_start(uint32_t index)
{
    if (!s_storage_initialized) {
        ESP_LOGE(TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_playing) {
        ESP_LOGW(TAG, "Already playing");
        return ESP_ERR_INVALID_STATE;
    }

    if (index >= IR_MAX_RECORDINGS) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Verify recording exists */
    ir_recording_info_t info;
    esp_err_t ret = ir_get_recording_info(index, &info);
    if (ret != ESP_OK || !info.valid) {
        ESP_LOGE(TAG, "Recording %lu not found", (unsigned long)index);
        return ESP_ERR_NOT_FOUND;
    }

    /* Create playback task */
    if (xTaskCreate(ir_playback_task, "ir_playback", 4096, (void *)(uintptr_t)index, 5, &s_playback_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t ir_playback_stop(void)
{
    if (!s_playing) {
        return ESP_OK;
    }

    /* Note: RMT transmission cannot be easily stopped mid-stream */
    /* We'll let it complete or timeout */
    s_playing = false;
    return ESP_OK;
}

bool ir_is_playing(void)
{
    return s_playing;
}

float ir_get_playback_progress(void)
{
    return s_playback_progress;
}

esp_err_t ir_get_storage_info(uint32_t *total_bytes, uint32_t *used_bytes)
{
    if (!s_storage_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t total = 0, used = 0;
    esp_err_t ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS info: %s", esp_err_to_name(ret));
        return ret;
    }

    if (total_bytes) *total_bytes = (uint32_t)total;
    if (used_bytes) *used_bytes = (uint32_t)used;
    return ESP_OK;
}

esp_err_t ir_freeze_last_frame(void)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_last_frame_symbols > 0) {
            memcpy(s_frozen_frame_buf, s_last_frame_buf, s_last_frame_symbols * sizeof(rmt_symbol_word_t));
            s_frozen_frame_symbols = s_last_frame_symbols;
        }
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "Frozen last frame (%lu symbols)", (unsigned long)s_frozen_frame_symbols);
    return ESP_OK;
}

esp_err_t ir_save_last_frame(void)
{
    if (!s_storage_initialized) {
        ESP_LOGE(TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Snapshot frozen data under mutex, then release before file I/O */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex for save");
        return ESP_ERR_TIMEOUT;
    }

    uint32_t num = s_frozen_frame_symbols;
    if (num == 0) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "No frame data to save");
        return ESP_ERR_INVALID_STATE;
    }

    rmt_symbol_word_t *snap_buf = heap_caps_malloc(num * sizeof(rmt_symbol_word_t), MALLOC_CAP_DEFAULT);
    if (!snap_buf) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(snap_buf, s_frozen_frame_buf, num * sizeof(rmt_symbol_word_t));
    xSemaphoreGive(s_mutex);

    /* Find next available index */
    uint32_t new_index = 0;
    esp_err_t ret = ir_find_free_index(&new_index);
    if (ret != ESP_OK) {
        free(snap_buf);
        return ret;
    }

    /* Prepare header */
    ir_recording_header_t header;
    memset(&header, 0, sizeof(header));
    header.magic = IR_RECORDING_MAGIC;
    header.version = 1;
    header.symbol_count = num;
    header.timestamp = (uint32_t)(esp_timer_get_time() / 1000000);

    uint32_t total_duration = 0;
    for (uint32_t i = 0; i < num; i++) {
        total_duration += snap_buf[i].duration0 + snap_buf[i].duration1;
    }
    header.total_duration_us = total_duration * 2;

    ret = ir_save_recording_to_file(new_index, &header, snap_buf);
    free(snap_buf);

    if (ret == ESP_OK) {
        s_saved_recording_count++;
        ESP_LOGI(TAG, "Saved frozen frame as recording %lu (%lu symbols)", (unsigned long)new_index, (unsigned long)num);
    }

    return ret;
}
