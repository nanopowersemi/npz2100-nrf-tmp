/**
 * @file npz2100_dump.h
 * @brief nPZ2100 register dump - reads the live register space from the device.
 *
 * Streams every readable register to a caller-supplied callback, one
 * contiguous segment at a time.  No shadow is involved: the values come
 * straight from the chip, so the dump works with NPZ2100_SHADOW_ENABLE=0
 * and always reflects the real device state.
 *
 * Segments reported (all ascending by address):
 *
 *   Global (bank = -1)
 *     0x01        ID
 *     0x02-0x04   STA1-STA3          (only with NPZ2100_DUMP_STATUS)
 *     0x05-0x0D   IOCFG1-5, SYSCFG1-2, TOUT_L/H
 *     0x10-0x1C   GCT_MS, GCT_0-3, GCT_ALM_0-3, WDOG_L/H, GTC_CFG, PA_CFG
 *     0x1F        P_BANK             (value at the time of the dump)
 *     0x40-0x49   ADCCFG, THROVA/THRUNA 1-3, VAL_ADC1-3
 *     0x50-0x52   LOGCFG, LOGSADDR, LOGCADDR
 *     0x53-0x56   CNT_VAL_0-3        (confirmed by a second read)
 *     0x57-0x5B   CNTCFG, CNT_TRIG_0-3
 *     0x7F        SRAM_BANK
 *
 *   Peripheral banks (bank = 0..5), 16 bytes each, start address 0x20
 *     0x20-0x2D   CFGP, IOP, MODP, PERP_L/H, NCMDP, ADDRP, RREGP,
 *                 THROVP_L/H, THRUNP_L/H, TWTP, TCFGP
 *     0x2E-0x2F   VALP_L, VALP_H
 *
 *   SRAM (bank = NPZ2100_DUMP_BANK_SRAM, only with NPZ2100_DUMP_SRAM)
 *     16 segments of 16 bytes covering SRAM addresses 0x00-0xFF.  The
 *     start address reported is the SRAM address, not the I2C address.
 *
 * Not read:
 *   0x00 IDLE_RST   write-only command register.
 *   0x6F, 0x76-79   UUID (needs the unlock write; use npz2100_read_uuid_ll()).
 *
 * Side effects and caveats:
 *   - Reading STA1 or STA2 resets the nPZ2100 watchdog (datasheet 2.10).
 *     The status registers are therefore skipped unless NPZ2100_DUMP_STATUS
 *     is passed.  STA flags are not cleared by reading them.
 *   - The peripheral banks are read by writing P_BANK.  The original P_BANK
 *     value is written back before the function returns (also on error), so
 *     the device and any shadow keep a consistent active bank.
 *   - GCT_MS/GCT_0-3 are read in one burst because the chip freezes the
 *     counter registers during a read (datasheet 2.10).  CNT_VAL_0-3 are not
 *     frozen, so they are read until two consecutive reads agree (max 3).
 *   - The SRAM is read through the I2C window 0x80-0xFF by selecting
 *     SRAM_BANK 0 and then 1.  The original SRAM_BANK value is written back
 *     before the function returns (also on error).  It is read in 16-byte
 *     chunks, so no single transfer exceeds 16 bytes and the buffer stays
 *     small on the caller's stack.  That is about 32 transactions, roughly
 *     60 ms of bus time at 100 kHz, which is why it is opt-in.
 *   - Not safe to call while another transaction sequence that depends on the
 *     active P_BANK / SRAM_BANK is in progress (it temporarily changes them).
 *
 * @version 0.8
 * @date    2026-10-28
 * @author  Nanopower Semiconductor AS
 */

#ifndef NPZ2100_DUMP_H_
#define NPZ2100_DUMP_H_

#include "npz2100_hal.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Include STA1-STA3 in the dump.  Reading STA1/STA2 resets the watchdog. */
#define NPZ2100_DUMP_STATUS         (0x01u)

/** Include the 256-byte SRAM.  Switches SRAM_BANK (restored afterwards). */
#define NPZ2100_DUMP_SRAM           (0x02u)

/** Bank value passed to the callback for non-banked (global) segments. */
#define NPZ2100_DUMP_BANK_GLOBAL    (-1)

/** Bank value passed to the callback for SRAM segments (start = SRAM address). */
#define NPZ2100_DUMP_BANK_SRAM      (-2)

/** Bytes per SRAM segment. */
#define NPZ2100_DUMP_SRAM_CHUNK     (16u)

/** Bytes per peripheral bank segment (0x20..0x2F). */
#define NPZ2100_DUMP_PERIPH_LEN     (16u)

/** Number of peripheral banks. */
#define NPZ2100_DUMP_PERIPH_COUNT   (6u)

/**
 * @brief Callback invoked once per contiguous register segment.
 *
 * @param bank        NPZ2100_DUMP_BANK_GLOBAL (-1) for global registers,
 *                    NPZ2100_DUMP_BANK_SRAM (-2) for SRAM segments,
 *                    or 0..5 for a peripheral bank segment.
 * @param start_addr  Register address of data[0] (SRAM address for SRAM).
 * @param data        Register values.  Valid only during the callback.
 * @param len         Number of bytes in data.
 * @param user        Opaque pointer passed through from npz2100_reg_dump().
 */
typedef void (*npz2100_dump_cb_t)(int8_t        bank,
                                   uint8_t       start_addr,
                                   const uint8_t *data,
                                   size_t        len,
                                   void         *user);

/**
 * @brief Read all readable registers and report them through a callback.
 *
 * @param[in] hal    Pointer to initialised HAL descriptor.
 * @param[in] flags  0, or a combination of NPZ2100_DUMP_STATUS (include
 *                   STA1-STA3) and NPZ2100_DUMP_SRAM (include the SRAM).
 * @param[in] cb     Callback invoked for each segment (must not be NULL).
 * @param[in] user   Passed unchanged to every callback invocation.
 * @return NPZ2100_OK on success, NPZ2100_ERR_ARG on NULL hal/cb,
 *         NPZ2100_ERR_IO on the first I2C error.  After an error the
 *         original P_BANK and SRAM_BANK are still restored where possible.
 */
npz2100_err_t npz2100_reg_dump(const npz2100_hal_t *hal,
                                uint8_t              flags,
                                npz2100_dump_cb_t    cb,
                                void                *user);

#ifdef __cplusplus
}
#endif

#endif /* NPZ2100_DUMP_H_ */
