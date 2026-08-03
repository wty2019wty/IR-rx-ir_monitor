#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define IR_RAW_MAX_SEGS 1024

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

/**
 * Initialize RMT RX capture task.
 */
esp_err_t ir_capture_init(void);

/**
 * Get the latest captured frame. Returns true once per new frame.
 */
bool ir_get_frame(ir_frame_t *out);
