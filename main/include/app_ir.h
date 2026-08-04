#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/rmt_types.h"

#define IR_RAW_MAX_SEGS 4096
#define IR_MAX_RECORDINGS 50  /* Maximum number of recordings to store */
#define IR_RECORDING_MAGIC 0x52495258  /* "RIRX" magic number for file format */

typedef struct {
    bool valid;               /* at least one frame captured since boot */
    uint32_t seq;             /* frame sequence number */

    /* raw signal features */
    uint32_t seg_count;       /* number of alternating level segments */
    uint32_t total_us;        /* frame duration (trailing idle excluded) */
    uint32_t leader_pulse_us; /* first pulse width */
    uint32_t leader_space_us; /* first space width */
    uint32_t pulse_count;     /* number of pulses (bursts) */
    uint32_t min_pulse_us;    /* smallest data pulse (leader excluded) */
    uint32_t max_pulse_us;    /* largest data pulse (leader excluded) */
    uint32_t last_gap_us;     /* last space before idle */

    /* NEC decode result */
    bool nec_ok;
    bool nec_repeat;          /* NEC repeat code received */
    bool nec_chksum_ok;       /* address/command inverse bytes match */
    bool nec_ext_addr;        /* 16-bit (extended) NEC address */
    uint8_t nec_bits;         /* 32 for a data frame, 0 for repeat */
    uint16_t nec_addr;
    uint16_t nec_cmd;
    uint32_t nec_raw;         /* full 32-bit NEC frame */
} ir_frame_t;

/* Recording file header structure */
typedef struct {
    uint32_t magic;           /* Magic number to identify valid recordings */
    uint32_t version;         /* File format version */
    uint32_t symbol_count;    /* Number of RMT symbols in this recording */
    uint32_t total_duration_us; /* Total duration of the signal in microseconds */
    uint32_t timestamp;       /* Recording timestamp (seconds since boot) */
    uint8_t protocol;         /* Protocol type: 0=unknown, 1=NEC, 2=NEC repeat, 3=NEC ext */
    uint8_t reserved[3];      /* Reserved for future use */
    uint16_t nec_addr;        /* NEC address (if protocol is NEC) */
    uint16_t nec_cmd;         /* NEC command (if protocol is NEC) */
    uint32_t nec_raw;         /* Raw NEC code (if protocol is NEC) */
    char name[32];            /* Optional name for the recording */
} ir_recording_header_t;

/* Recording info for listing */
typedef struct {
    uint32_t index;           /* Recording index */
    uint32_t symbol_count;    /* Number of symbols */
    uint32_t total_duration_us; /* Total duration */
    uint8_t protocol;         /* Protocol type */
    uint16_t nec_addr;        /* NEC address */
    uint16_t nec_cmd;         /* NEC command */
    bool valid;               /* Whether this recording is valid */
} ir_recording_info_t;

/**
 * Initialize RMT RX capture task.
 */
esp_err_t ir_capture_init(void);

/**
 * Start IR receiving. Must be called after ir_storage_init() to avoid
 * flash access conflicts with RMT ISR.
 */
esp_err_t ir_start_receive(void);

/**
 * Get the latest captured frame. Returns true once per new frame.
 */
bool ir_get_frame(ir_frame_t *out);

/**
 * Initialize the recording storage system (LittleFS).
 */
esp_err_t ir_storage_init(void);

/**
 * Initialize storage in a background task, then start IR receive.
 * Returns immediately; storage becomes ready asynchronously.
 */
esp_err_t ir_storage_init_async(void);

/**
 * Check if storage initialization has completed.
 */
bool ir_storage_is_ready(void);

/**
 * Get the number of saved recordings in storage.
 */
uint32_t ir_get_saved_recording_count(void);

/**
 * Get info about a specific recording.
 * @param index Recording index (0-based)
 * @param info Output parameter for recording info
 */
esp_err_t ir_get_recording_info(uint32_t index, ir_recording_info_t *info);

/**
 * Delete a specific recording.
 * @param index Recording index (0-based)
 */
esp_err_t ir_delete_recording(uint32_t index);

/**
 * Delete all recordings.
 */
esp_err_t ir_delete_all_recordings(void);

/**
 * Start playback of a recording.
 * @param index Recording index (0-based)
 */
esp_err_t ir_playback_start(uint32_t index);

/**
 * Stop playback of a recording.
 */
esp_err_t ir_playback_stop(void);

/**
 * Check if currently playing back.
 */
bool ir_is_playing(void);

/**
 * Get the playback progress (0.0 to 1.0).
 */
float ir_get_playback_progress(void);

/**
 * Get protocol type string.
 */
const char *ir_protocol_str(uint8_t protocol);

/**
 * Get storage info.
 * @param total_bytes Output: total storage size in bytes
 * @param used_bytes Output: used storage size in bytes
 */
esp_err_t ir_get_storage_info(uint32_t *total_bytes, uint32_t *used_bytes);

/**
 * Freeze the last received frame as a snapshot for saving.
 * Call this when entering pause mode to capture the displayed frame.
 */
esp_err_t ir_freeze_last_frame(void);

/**
 * Save the last received frame to storage.
 * This is used for "save on pause" feature.
 */
esp_err_t ir_save_last_frame(void);
