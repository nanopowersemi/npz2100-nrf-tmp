/**
 * @file npz2100_dump.c
 * @brief nPZ2100 register dump implementation.
 *
 * Platform-agnostic - no OS, no HAL, no standard library beyond <stddef.h>.
 * All I2C access goes through the npz2100_hal_t callbacks.  No shadow is
 * read or written; see npz2100_dump.h for the full behaviour description.
 *
 * @version 0.8
 * @date    2026-10-28
 * @author  Nanopower Semiconductor AS
 */

#include "npz2100_dump.h"
#include "npz2100_regs_system.h"
#include "npz2100_regs_io.h"
#include "npz2100_regs_periph.h"
#include "npz2100_regs_adc_log.h"

/* =========================================================================
 * Global (non-banked) segment table
 * ======================================================================= */

/** Segment flags. */
#define SEG_STATUS   (0x01u)   /* only read when NPZ2100_DUMP_STATUS is set     */
#define SEG_STABLE   (0x02u)   /* live counter: confirm with a second read      */
#define SEG_PBANK    (0x04u)   /* this segment holds P_BANK: remember its value */
#define SEG_SBANK    (0x08u)   /* this segment holds SRAM_BANK: remember it     */

typedef struct {
    uint8_t start;
    uint8_t len;
    uint8_t flags;
} dump_seg_t;

/* Ascending by address.  Every entry is a run of defined, contiguous
 * registers (see the register headers), so one burst read covers it. */
static const dump_seg_t k_global_segs[] = {
    { NPZ2100_REG_ID,        1u, 0u          },  /* 0x01                     */
    { NPZ2100_REG_STA1,      3u, SEG_STATUS  },  /* 0x02-0x04 (kicks WDOG)   */
    { NPZ2100_REG_IOCFG1,    9u, 0u          },  /* 0x05-0x0D                */
    { NPZ2100_REG_GCT_MS,   13u, 0u          },  /* 0x10-0x1C, one burst     */
    { NPZ2100_REG_P_BANK,    1u, SEG_PBANK   },  /* 0x1F                     */
    { NPZ2100_REG_ADCCFG,   10u, 0u          },  /* 0x40-0x49                */
    { NPZ2100_REG_LOGCFG,    3u, 0u          },  /* 0x50-0x52                */
    { NPZ2100_REG_CNT_VAL_0, 4u, SEG_STABLE  },  /* 0x53-0x56 (not frozen)   */
    { NPZ2100_REG_CNTCFG,    5u, 0u          },  /* 0x57-0x5B                */
    { NPZ2100_REG_SRAM_BANK, 1u, SEG_SBANK   },  /* 0x7F                     */
};

#define N_GLOBAL_SEGS  (sizeof(k_global_segs) / sizeof(k_global_segs[0]))

/** Longest global segment (GCT block). */
#define MAX_SEG_LEN    (16u)

/** Maximum reads of a live (non-frozen) counter segment. */
#define STABLE_TRIES   (3u)

/* =========================================================================
 * Internal helpers
 * ======================================================================= */

/**
 * @brief Read a segment; for live counters repeat until two reads agree.
 *
 * The datasheet recommends a second read of CNT_VAL to confirm the value
 * when the counter may increment during the transfer.  If no two
 * consecutive reads agree within STABLE_TRIES, the last read is returned.
 */
static npz2100_err_t read_segment(const npz2100_hal_t *hal,
                                   const dump_seg_t    *seg,
                                   uint8_t             *buf)
{
    npz2100_err_t err = npz2100_reg_burst_read(hal, seg->start, buf, seg->len);
    if (err != NPZ2100_OK || (seg->flags & SEG_STABLE) == 0u) {
        return err;
    }

    uint8_t again[MAX_SEG_LEN];
    for (uint8_t attempt = 1u; attempt < STABLE_TRIES; attempt++) {
        err = npz2100_reg_burst_read(hal, seg->start, again, seg->len);
        if (err != NPZ2100_OK) {
            return err;
        }
        uint8_t same = 1u;
        for (uint8_t i = 0u; i < seg->len; i++) {
            if (again[i] != buf[i]) {
                same = 0u;
            }
            buf[i] = again[i];          /* keep the most recent read */
        }
        if (same != 0u) {
            break;
        }
    }
    return NPZ2100_OK;
}

/* =========================================================================
 * npz2100_reg_dump
 * ======================================================================= */

npz2100_err_t npz2100_reg_dump(const npz2100_hal_t *hal,
                                uint8_t              flags,
                                npz2100_dump_cb_t    cb,
                                void                *user)
{
    if (hal == NULL || cb == NULL) {
        return NPZ2100_ERR_ARG;
    }

    uint8_t       buf[MAX_SEG_LEN];
    uint8_t       orig_bank = 0u;
    uint8_t       have_bank = 0u;
    uint8_t       orig_sram = 0u;
    uint8_t       have_sram = 0u;
    npz2100_err_t err;

    /* ---- Global registers (no bank change happens here) ---------------- */
    for (size_t s = 0u; s < N_GLOBAL_SEGS; s++) {
        const dump_seg_t *seg = &k_global_segs[s];

        if ((seg->flags & SEG_STATUS) != 0u &&
            (flags & NPZ2100_DUMP_STATUS) == 0u) {
            continue;
        }

        err = read_segment(hal, seg, buf);
        if (err != NPZ2100_OK) {
            return err;
        }
        if ((seg->flags & SEG_PBANK) != 0u) {
            orig_bank = buf[0];
            have_bank = 1u;
        }
        if ((seg->flags & SEG_SBANK) != 0u) {
            orig_sram = buf[0];
            have_sram = 1u;
        }
        cb(NPZ2100_DUMP_BANK_GLOBAL, seg->start, buf, seg->len, user);
    }

    /* ---- Peripheral banks 0..5 ----------------------------------------- */
    npz2100_err_t first_err = NPZ2100_OK;

    for (uint8_t slot = 0u; slot <= NPZ2100_P_BANK_MAX; slot++) {
        err = npz2100_reg_write(hal, NPZ2100_REG_P_BANK, slot);
        if (err != NPZ2100_OK) {
            first_err = err;
            break;
        }

        /* 0x20..0x2D then 0x2E..0x2F: both are documented burst starts. */
        err = npz2100_reg_burst_read(hal, NPZ2100_REG_CFGP, buf, 14u);
        if (err == NPZ2100_OK) {
            err = npz2100_reg_burst_read(hal, NPZ2100_REG_VALP_L, &buf[14], 2u);
        }
        if (err != NPZ2100_OK) {
            first_err = err;
            break;
        }

        cb((int8_t)slot, NPZ2100_REG_CFGP, buf, NPZ2100_DUMP_PERIPH_LEN, user);
    }

    /* ---- SRAM (opt-in): 2 banks x 128 bytes through window 0x80-0xFF --- */
    uint8_t sram_touched = 0u;

    if (first_err == NPZ2100_OK && have_sram != 0u &&
        (flags & NPZ2100_DUMP_SRAM) != 0u) {
        for (uint8_t sbank = 0u; sbank < 2u && first_err == NPZ2100_OK; sbank++) {
            sram_touched = 1u;
            err = npz2100_reg_write(hal, NPZ2100_REG_SRAM_BANK,
                                    NPZ2100_SRAM_BANK(sbank));
            if (err != NPZ2100_OK) {
                first_err = err;
                break;
            }
            for (uint8_t off = 0u; off < NPZ2100_SRAM_WINDOW_SIZE;
                 off = (uint8_t)(off + NPZ2100_DUMP_SRAM_CHUNK)) {
                err = npz2100_reg_burst_read(
                    hal, (uint8_t)(NPZ2100_SRAM_WINDOW_BASE + off),
                    buf, NPZ2100_DUMP_SRAM_CHUNK);
                if (err != NPZ2100_OK) {
                    first_err = err;
                    break;
                }
                cb(NPZ2100_DUMP_BANK_SRAM,
                   (uint8_t)(sbank * NPZ2100_SRAM_WINDOW_SIZE + off),
                   buf, NPZ2100_DUMP_SRAM_CHUNK, user);
            }
        }
    }

    /* ---- Always put P_BANK / SRAM_BANK back, even after an error -------- */
    if (have_bank != 0u) {
        err = npz2100_reg_write(hal, NPZ2100_REG_P_BANK, orig_bank);
        if (first_err == NPZ2100_OK) {
            first_err = err;
        }
    }
    if (sram_touched != 0u) {
        err = npz2100_reg_write(hal, NPZ2100_REG_SRAM_BANK, orig_sram);
        if (first_err == NPZ2100_OK) {
            first_err = err;
        }
    }

    return first_err;
}
