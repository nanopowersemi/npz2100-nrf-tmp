/**
 * @file npz2100_lib.c
 * @brief nPZ2100 lib API implementation.
 *
 * See npz2100_lib.h for design rationale and layering diagram.
 *
 * Implementation notes
 * --------------------
 *
 * Shadow ↔ register address mapping
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * Two static helper tables translate between a raw register address and the
 * corresponding uint8_t field inside npz2100_config_t.  This avoids a
 * large switch statement and keeps the walk O(n) where n is the number
 * of data bytes in the regmap stream, not the number of registers in the
 * device.
 *
 * Byte-stream parsing
 * ~~~~~~~~~~~~~~~~~~~~
 * The regmap is a flat `const uint8_t[]` - a concatenation of segments
 * shaped `[length][start_addr][data...]`. The static map_walk() function is
 * the single parsing primitive: it validates segment framing and invokes a
 * visitor callback once per (addr, value) pair in stream order. Apply, diff,
 * and validate are each just a different visitor over the same walk.
 *
 * Banked peripheral registers (0x20–0x2D)
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * The nPZ2100 exposes one window of peripheral registers at fixed addresses
 * 0x20–0x2D, selected by writing P_BANK (0x1F). P_BANK is just another
 * register address in the stream - writing it updates cfg->p_bank like any
 * other shadowed register, and subsequent banked addresses resolve against
 * whatever bank was last selected, exactly as on real hardware. No special
 * casing or segment lookahead is required.
 *
 * No dynamic memory, no floating point, no stdlib beyond string.h memset.
 *
 * @version 0.8
 * @date    2026-09-11
 * @author  Nanopower Semiconductor AS
 */

#include "npz2100_lib.h"
#include <string.h>   /* memset */

/* =========================================================================
 * Internal: shadow field lookup
 *
 * Maps a non-banked register address → pointer to the matching uint8_t field
 * in an npz2100_config_t.  Returns NULL for read-only or unrecognised addrs.
 * ======================================================================= */

/* shadow_field, shadow_periph_field, and is_periph_addr are needed in both
 * shadow modes: the config struct is always kept in step with what was
 * written. With NPZ2100_SHADOW_ENABLE=0 they are also used on a struct of
 * reset defaults, which npz2100_map_apply() diffs against instead. */

/**
 * @brief Look up the shadow byte for a non-banked, non-peripheral register.
 *
 * @param cfg   Config shadow.
 * @param addr  Register address.
 * @return Pointer to the shadow field, or NULL if not found / read-only.
 */
static uint8_t *shadow_field(npz2100_config_t *cfg, uint8_t addr)
{
    switch (addr) {
        /* IDLE_RST always reads 0x00; we track last intent for diffing */
        case NPZ2100_REG_IDLE_RST:   return &cfg->idle_rst;
        case NPZ2100_REG_P_BANK:     return &cfg->p_bank;
        case NPZ2100_REG_IOCFG1:     return &cfg->iocfg1;
        case NPZ2100_REG_IOCFG2:     return &cfg->iocfg2;
        case NPZ2100_REG_IOCFG3:     return &cfg->iocfg3;
        case NPZ2100_REG_IOCFG4:     return &cfg->iocfg4;
        case NPZ2100_REG_IOCFG5:     return &cfg->iocfg5;
        case NPZ2100_REG_SYSCFG1:    return &cfg->syscfg1;
        case NPZ2100_REG_SYSCFG2:    return &cfg->syscfg2;
        case NPZ2100_REG_TOUT_L:     return &cfg->tout_l;
        case NPZ2100_REG_TOUT_H:     return &cfg->tout_h;
        case NPZ2100_REG_GCT_MS:     return &cfg->gct_ms;
        case NPZ2100_REG_GCT_0:      return &cfg->gct_0;
        case NPZ2100_REG_GCT_1:      return &cfg->gct_1;
        case NPZ2100_REG_GCT_2:      return &cfg->gct_2;
        case NPZ2100_REG_GCT_3:      return &cfg->gct_3;
        case NPZ2100_REG_GCT_ALM_0:  return &cfg->gct_alm_0;
        case NPZ2100_REG_GCT_ALM_1:  return &cfg->gct_alm_1;
        case NPZ2100_REG_GCT_ALM_2:  return &cfg->gct_alm_2;
        case NPZ2100_REG_GCT_ALM_3:  return &cfg->gct_alm_3;
        case NPZ2100_REG_WDOG_L:     return &cfg->wdog_l;
        case NPZ2100_REG_WDOG_H:     return &cfg->wdog_h;
        case NPZ2100_REG_GTC_CFG:    return &cfg->gtc_cfg;
        case NPZ2100_REG_PA_CFG:     return &cfg->pa_cfg;
        case NPZ2100_REG_ADCCFG:     return &cfg->adccfg;
        case NPZ2100_REG_THROVA1:    return &cfg->throva1;
        case NPZ2100_REG_THRUNA1:    return &cfg->thruna1;
        case NPZ2100_REG_THROVA2:    return &cfg->throva2;
        case NPZ2100_REG_THRUNA2:    return &cfg->thruna2;
        case NPZ2100_REG_THROVA3:    return &cfg->throva3;
        case NPZ2100_REG_THRUNA3:    return &cfg->thruna3;
        case NPZ2100_REG_LOGCFG:     return &cfg->logcfg;
        case NPZ2100_REG_LOGSADDR:   return &cfg->logsaddr;
        case NPZ2100_REG_CNT_VAL_0:  return &cfg->cnt_val_0;
        case NPZ2100_REG_CNT_VAL_1:  return &cfg->cnt_val_1;
        case NPZ2100_REG_CNT_VAL_2:  return &cfg->cnt_val_2;
        case NPZ2100_REG_CNT_VAL_3:  return &cfg->cnt_val_3;
        case NPZ2100_REG_CNTCFG:     return &cfg->cntcfg;
        case NPZ2100_REG_CNT_TRIG_0: return &cfg->cnt_trig_0;
        case NPZ2100_REG_CNT_TRIG_1: return &cfg->cnt_trig_1;
        case NPZ2100_REG_CNT_TRIG_2: return &cfg->cnt_trig_2;
        case NPZ2100_REG_CNT_TRIG_3: return &cfg->cnt_trig_3;
        case NPZ2100_REG_SRAM_BANK:  return &cfg->sram_bank;
        default:                      return NULL;
    }
}

/**
 * @brief Look up the shadow byte for a banked peripheral register (0x20–0x2D).
 *
 * @param cfg   Config shadow.
 * @param slot  Peripheral slot 0–5.
 * @param addr  Banked register address (0x20–0x2D).
 * @return Pointer to shadow field, or NULL if addr out of banked range.
 */
static uint8_t *shadow_periph_field(npz2100_config_t *cfg,
                                    uint8_t           slot,
                                    uint8_t           addr)
{
    npz2100_periph_shadow_t *p = &cfg->periph[slot];
    switch (addr) {
        case NPZ2100_REG_CFGP:      return &p->cfgp;
        case NPZ2100_REG_IOP:       return &p->iop;
        case NPZ2100_REG_MODP:      return &p->modp;
        case NPZ2100_REG_PERP_L:    return &p->perp_l;
        case NPZ2100_REG_PERP_H:    return &p->perp_h;
        case NPZ2100_REG_NCMDP:     return &p->ncmdp;
        case NPZ2100_REG_ADDRP:     return &p->addrp;
        case NPZ2100_REG_RREGP:     return &p->rregp;
        case NPZ2100_REG_THROVP_L:  return &p->throvp_l;
        case NPZ2100_REG_THROVP_H:  return &p->throvp_h;
        case NPZ2100_REG_THRUNP_L:  return &p->thrunp_l;
        case NPZ2100_REG_THRUNP_H:  return &p->thrunp_h;
        case NPZ2100_REG_TWTP:      return &p->twtp;
        case NPZ2100_REG_TCFGP:     return &p->tcfgp;
        default:                     return NULL;
    }
}

/** True if addr falls in the banked peripheral window. */
static bool is_periph_addr(uint8_t addr)
{
    return (addr >= NPZ2100_REG_CFGP && addr <= NPZ2100_REG_TCFGP);
}

/**
 * @brief Look up the config byte for any writable register, resolving
 *        banked addresses against @p slot.
 */
static uint8_t *config_field(npz2100_config_t *cfg, uint8_t slot, uint8_t addr)
{
    return is_periph_addr(addr)
         ? shadow_periph_field(cfg, slot, addr)
         : shadow_field(cfg, addr);
}

/* =========================================================================
 * Internal: bank select helper
 * ======================================================================= */

/**
 * @brief Write P_BANK only if the active bank needs to change, and update
 *        the shadow's p_bank field to match.
 *
 * @param hal          HAL descriptor.
 * @param cfg          Shadow - p_bank field is updated on success.
 * @param target_slot  Desired slot (0–5).
 */
static npz2100_err_t ensure_bank(const npz2100_hal_t *hal,
                                  npz2100_config_t    *cfg,
                                  uint8_t              target_slot)
{
    if (cfg->p_bank == target_slot) {
        return NPZ2100_OK;  /* Already selected - no I²C transaction needed. */
    }
    npz2100_err_t err = npz2100_reg_write(hal, NPZ2100_REG_P_BANK,
                                           NPZ2100_P_BANK(target_slot));
    if (err == NPZ2100_OK) {
        cfg->p_bank = target_slot;
    }
    return err;
}

/* =========================================================================
 * Internal: burst helpers
 *
 * Writes and reads go to the device in bursts of consecutive registers,
 * one I2C transaction each, instead of one transaction per register.
 * ======================================================================= */

/** Longest block of consecutive writable registers (CFGP–TCFGP, 14). */
#define NPZ2100_RUN_MAX  14u

/**
 * @brief Find the multi-byte value a register belongs to.
 *
 * The bytes of a 16- or 32-bit value are always written together, in one
 * transaction and in address order, when any of them changes, so the
 * device never runs with a mix of old and new bytes. For the GCT block this
 * is required: the write to GCT_3 latches all five bytes.
 *
 * @param addr   Register address.
 * @param first  Set to the value's first (lowest) register address.
 * @param last   Set to the value's last register address. Both are @p addr
 *               for a single-byte register.
 */
static void reg_group(uint8_t addr, uint8_t *first, uint8_t *last)
{
    static const struct { uint8_t first; uint8_t last; } groups[] = {
        { NPZ2100_REG_TOUT_L,     NPZ2100_REG_TOUT_H     },
        { NPZ2100_REG_GCT_MS,     NPZ2100_REG_GCT_3      },
        { NPZ2100_REG_GCT_ALM_0,  NPZ2100_REG_GCT_ALM_3  },
        { NPZ2100_REG_WDOG_L,     NPZ2100_REG_WDOG_H     },
        { NPZ2100_REG_PERP_L,     NPZ2100_REG_PERP_H     },
        { NPZ2100_REG_THROVP_L,   NPZ2100_REG_THROVP_H   },
        { NPZ2100_REG_THRUNP_L,   NPZ2100_REG_THRUNP_H   },
        { NPZ2100_REG_CNT_VAL_0,  NPZ2100_REG_CNT_VAL_3  },
        { NPZ2100_REG_CNT_TRIG_0, NPZ2100_REG_CNT_TRIG_3 },
    };

    for (size_t i = 0u; i < sizeof(groups) / sizeof(groups[0]); i++) {
        if (addr >= groups[i].first && addr <= groups[i].last) {
            *first = groups[i].first;
            *last  = groups[i].last;
            return;
        }
    }
    *first = addr;
    *last  = addr;
}

/**
 * @brief True if writing a register with the value it should already hold
 *        has no side effect.
 *
 * npz2100_map_apply() may then write such an unchanged register to join
 * two changed neighbours into one burst. Registers that are not plain
 * configuration (the clock, counter, watchdog, alarm enable, logging, the
 * bank selects) are never written just to save a transaction.
 */
static bool reg_bridgeable(uint8_t addr)
{
    return (addr >= NPZ2100_REG_IOCFG1     && addr <= NPZ2100_REG_TOUT_H)
        || (addr >= NPZ2100_REG_GCT_ALM_0  && addr <= NPZ2100_REG_GCT_ALM_3)
        || (addr == NPZ2100_REG_PA_CFG)
        || is_periph_addr(addr)
        || (addr >= NPZ2100_REG_ADCCFG     && addr <= NPZ2100_REG_THRUNA3)
        || (addr >= NPZ2100_REG_CNT_TRIG_0 && addr <= NPZ2100_REG_CNT_TRIG_3);
}

/** Copy a block of register values just read or written into @p cfg. */
static void config_update(npz2100_config_t *cfg, uint8_t start_reg,
                          const uint8_t *data, size_t len)
{
    if (cfg == NULL) {
        return;
    }
    for (size_t i = 0u; i < len; i++) {
        uint8_t *field = config_field(cfg, cfg->p_bank,
                                      (uint8_t)(start_reg + i));
        if (field != NULL) {
            *field = data[i];
        }
    }
}

/**
 * @brief Write @p len consecutive registers from @p start_reg in one burst,
 *        with the values held in @p cfg.
 *
 * Banked addresses resolve against cfg->p_bank, which must match the bank
 * selected on the device. Every register in the block must be writable.
 *
 * Builds the [reg, data...] buffer itself and calls the HAL directly:
 * npz2100_reg_burst_write() puts a 257-byte buffer on the stack, too much
 * for the calling thread at the depth npz2100_map_apply() calls this from.
 */
static npz2100_err_t write_block(const npz2100_hal_t *hal,
                                 npz2100_config_t    *cfg,
                                 uint8_t              start_reg,
                                 uint8_t              len)
{
    uint8_t buf[1u + NPZ2100_RUN_MAX];

    if (hal == NULL || hal->write == NULL ||
        len == 0u || len > NPZ2100_RUN_MAX) {
        return NPZ2100_ERR_ARG;
    }
    buf[0] = start_reg;
    for (uint8_t i = 0u; i < len; i++) {
        const uint8_t *field = config_field(cfg, cfg->p_bank,
                                            (uint8_t)(start_reg + i));
        if (field == NULL) {
            return NPZ2100_ERR_ARG;
        }
        buf[1u + i] = *field;
    }
    return hal->write(hal->i2c_addr, buf, 1u + (size_t)len, hal->ctx);
}

/**
 * @brief Read @p len consecutive registers from @p start_reg in one burst
 *        and store them in @p cfg. Read-only registers in the block are
 *        read and dropped.
 */
static npz2100_err_t read_block(const npz2100_hal_t *hal,
                                npz2100_config_t    *cfg,
                                uint8_t              start_reg,
                                uint8_t              len)
{
    uint8_t buf[NPZ2100_RUN_MAX];

    if (len == 0u || len > NPZ2100_RUN_MAX) {
        return NPZ2100_ERR_ARG;
    }
    npz2100_err_t err = npz2100_reg_burst_read(hal, start_reg, buf, len);
    if (err == NPZ2100_OK) {
        config_update(cfg, start_reg, buf, len);
    }
    return err;
}

/** Number of registers from @p first to @p last, inclusive. */
#define REG_SPAN(first, last)  ((uint8_t)((last) - (first) + 1u))

/**
 * Non-banked blocks of consecutive writable registers, in address order.
 * Reserved addresses (0x0E–0x0F, 0x1D–0x1E) fall between blocks.
 */
static const struct { uint8_t start; uint8_t len; } config_blocks[] = {
    /* IOCFG1–IOCFG5, SYSCFG1–SYSCFG2, TOUT_L/H */
    { NPZ2100_REG_IOCFG1,    REG_SPAN(NPZ2100_REG_IOCFG1,  NPZ2100_REG_TOUT_H)     },
    /* GCT (GCT_3 after GCT_MS–GCT_2, latching it), GCT_ALM, WDOG, GTC_CFG, PA_CFG */
    { NPZ2100_REG_GCT_MS,    REG_SPAN(NPZ2100_REG_GCT_MS,  NPZ2100_REG_PA_CFG)     },
    /* ADCCFG, THROVA1–THRUNA3 */
    { NPZ2100_REG_ADCCFG,    REG_SPAN(NPZ2100_REG_ADCCFG,  NPZ2100_REG_THRUNA3)    },
    /* LOGCFG, LOGSADDR */
    { NPZ2100_REG_LOGCFG,    REG_SPAN(NPZ2100_REG_LOGCFG,  NPZ2100_REG_LOGSADDR)   },
    /* CNTCFG, CNT_TRIG_0–3 (CNT_VAL is the running count and is not written) */
    { NPZ2100_REG_CNTCFG,    REG_SPAN(NPZ2100_REG_CNTCFG,  NPZ2100_REG_CNT_TRIG_3) },
    { NPZ2100_REG_SRAM_BANK, 1u },
};

/** Number of banked peripheral registers, CFGP–TCFGP. */
#define PERIPH_REGS  REG_SPAN(NPZ2100_REG_CFGP, NPZ2100_REG_TCFGP)

/* =========================================================================
 * Internal: regmap byte-stream walker
 *
 * Format: concatenation of segments shaped
 *   [length][start_addr][data_0]...[data_(length-2)]
 * where `length` includes start_addr, so data payload is (length - 1) bytes.
 *
 * The walker calls `visit(addr, value, user)` for every individual register
 * byte produced by every segment, in stream order. This single iteration
 * primitive backs npz2100_map_apply(), npz2100_map_diff_count(), and
 * npz2100_map_validate() - each just supplies a different visitor.
 * ======================================================================= */

/** Visitor callback signature for npz2100_map_walk(). */
typedef npz2100_err_t (*regmap_visit_fn)(uint8_t addr, uint8_t value, void *user);

/**
 * @brief Walk a regmap byte stream, calling @p visit for every register byte.
 *
 * Validates segment framing as it goes: if a segment's declared length would
 * read past the end of the buffer, parsing stops immediately with
 * NPZ2100_ERR_ARG (no partial segment is ever visited).
 *
 * A segment with length == 0 is invalid (there must be at least a start_addr
 * byte) and also yields NPZ2100_ERR_ARG.
 *
 * @param map      Pointer to the byte stream.
 * @param map_len  Total length of the stream.
 * @param visit    Callback invoked once per (addr, value) pair.
 * @param user     Opaque pointer forwarded to every visit call.
 * @return NPZ2100_OK if the entire stream was consumed without error, or
 *         whatever non-OK code the visitor returns (propagated immediately),
 *         or NPZ2100_ERR_ARG on malformed framing.
 */
static npz2100_err_t map_walk(const uint8_t  *map,
                               size_t          map_len,
                               regmap_visit_fn visit,
                               void           *user)
{
    size_t pos = 0u;

    while (pos < map_len) {
        /* Need at least 1 byte for `length`. */
        if (pos + 1u > map_len) {
            return NPZ2100_ERR_ARG;
        }
        uint8_t length = map[pos];

        /* A segment must contain at least start_addr (length >= 1). */
        if (length == 0u) {
            return NPZ2100_ERR_ARG;
        }

        /* Total bytes consumed by this segment: 1 (length field) + length. */
        if (pos + 1u + (size_t)length > map_len) {
            return NPZ2100_ERR_ARG;  /* Segment runs past end of buffer. */
        }

        uint8_t start_addr = map[pos + 1u];
        uint8_t data_len    = (uint8_t)(length - 1u);  /* bytes after start_addr */
        const uint8_t *data = &map[pos + 2u];

        for (uint8_t i = 0u; i < data_len; i++) {
            uint8_t addr = (uint8_t)(start_addr + i);
            npz2100_err_t err = visit(addr, data[i], user);
            if (err != NPZ2100_OK) {
                return err;
            }
        }

        pos += 1u + (size_t)length;
    }

    return NPZ2100_OK;
}

/* =========================================================================
 * Shadow initialisation
 * ======================================================================= */

npz2100_err_t npz2100_config_init_defaults(npz2100_config_t *cfg)
{
    if (cfg == NULL) {
        return NPZ2100_ERR_ARG;
    }

    /* Start from a clean slate. */
    memset(cfg, 0x00u, sizeof(*cfg));

    /*
     * Apply reset defaults from datasheet Table 3 and register reset fields.
     * Only non-zero reset values are set explicitly below; everything else
     * is already 0 from memset.
     */

    /* P_BANK: device powers up with bank 0 selected. */
    cfg->p_bank = 0x00u;

    /* IOCFG3: all INT pull-ups ON at ≈100 kΩ (reset = 0xFF). */
    cfg->iocfg3 = 0xFFu;

    /*
     * IOCFG5 reset: SPI_AUTO=1, I2C_PUP_AUTO=1, I2C_PUP_EN=1, PSW_SR=1.
     * Bits [3:0] = 0b1111 → 0x0F; bit[4]=1 (SPI_AUTO) → 0x1F.
     */
    cfg->iocfg5 = NPZ2100_IOCFG5_PSW_SR(1)
                | NPZ2100_IOCFG5_I2C_PUP_EN(1)
                | NPZ2100_IOCFG5_I2C_PUP_AUTO(1)
                | NPZ2100_IOCFG5_SPI_AUTO(1);

    /* TOUT: default 0xFFFF (max time-out). */
    cfg->tout_l = 0xFFu;
    cfg->tout_h = 0xFFu;

    /* PERP: reset default per-peripheral is 0x0002 (datasheet §2.2.23). */
    for (uint8_t i = 0u; i < 6u; i++) {
        cfg->periph[i].perp_l = 0x02u;
        cfg->periph[i].perp_h = 0x00u;
    }

    return NPZ2100_OK;
}

/* =========================================================================
 * Register-map operations
 * ======================================================================= */

/** Context passed to the apply visitor. */
typedef struct {
    const npz2100_hal_t *hal;
    npz2100_config_t    *cfg;
#if !NPZ2100_SHADOW_ENABLE
    npz2100_config_t    *defaults;  /* Reset values, the diff reference. */
#endif
    uint8_t              written;   /* Registers written to the device. */
    bool                 sram_pass; /* SRAM_BANK is left to the SRAM pass. */

    /*
     * The pending burst: run_len registers from run_start are to be
     * written, then `gap` unchanged registers that are only written if a
     * changed register follows them. must_end is the last address the
     * burst has to reach to complete a multi-byte value, or -1. The values
     * are taken from cfg when the burst is written.
     */
    uint8_t              run_start;
    uint8_t              run_len;
    uint8_t              gap;
    int16_t              must_end;
} apply_ctx_t;

/**
 * @brief Write the pending burst of npz2100_map_apply(), if any.
 *
 * Trailing unchanged registers (the gap) are dropped. If the burst stops
 * inside a multi-byte value, it is extended to the value's last byte with
 * the values from the config struct.
 */
static npz2100_err_t apply_flush(apply_ctx_t *ctx)
{
    npz2100_err_t err = NPZ2100_OK;

    if (ctx->run_len > 0u) {
        uint8_t len = ctx->run_len;
        if (ctx->must_end >= (int16_t)ctx->run_start + (int16_t)len) {
            len = (uint8_t)(ctx->must_end - ctx->run_start + 1);
        }
        err = write_block(ctx->hal, ctx->cfg, ctx->run_start, len);
        if (err == NPZ2100_OK) {
            ctx->written = (uint8_t)(ctx->written + len);
        }
    }
    ctx->run_len  = 0u;
    ctx->gap      = 0u;
    ctx->must_end = -1;
    return err;
}

/**
 * @brief Visitor for npz2100_map_apply().
 *
 * Read-only, reserved and SRAM addresses have no config field and are
 * skipped. A register needs writing when the value differs from the
 * reference:
 *   - NPZ2100_SHADOW_ENABLE=1: the shadow, i.e. the device state read by
 *     npz2100_map_readback() plus every write since.
 *   - NPZ2100_SHADOW_ENABLE=0: the register's reset default. No readback
 *     is needed; the regmap only costs I2C writes for the registers it
 *     changes from reset. P_BANK is always written in this mode, since the
 *     bank the device currently has selected is not known.
 *
 * Registers that need writing are collected into bursts of consecutive
 * addresses, written in one I2C transaction each, in stream order:
 *   - A changed byte of a 16- or 32-bit value (TOUT, PERP, THROVP, ...)
 *     writes the whole value in the same burst; see reg_group().
 *   - Up to NPZ2100_BURST_MAX_GAP unchanged registers between two changed
 *     ones are written with them, with the regmap's value, when that has
 *     no side effect; see reg_bridgeable().
 *   - A burst never crosses a read-only or reserved address.
 *   - P_BANK and IDLE_RST are written on their own: P_BANK selects the bank
 *     the following banked addresses resolve against.
 *
 * The config field is updated in both modes, written or not, so the shadow
 * holds the regmap's values afterwards. P_BANK updates cfg->p_bank so
 * subsequent banked addresses resolve to the correct slot.
 */
static npz2100_err_t apply_visitor(uint8_t addr, uint8_t value, void *user)
{
    apply_ctx_t *ctx = (apply_ctx_t *)user;
    npz2100_config_t *cfg = ctx->cfg;
    npz2100_err_t err;

    if (addr == NPZ2100_REG_SRAM_BANK && ctx->sram_pass) {
        /* The regmap has SRAM bytes: apply_sram_bytes() selects the bank
         * for them and leaves SRAM_BANK where the stream asks. */
        return apply_flush(ctx);
    }

    uint8_t *field = config_field(cfg, cfg->p_bank, addr);

    if (field == NULL) {
        /* Read-only / reserved / SRAM - skipped, and a burst must not
         * write across it. */
        return apply_flush(ctx);
    }

#if NPZ2100_SHADOW_ENABLE
    bool changed = (*field != value);
#else
    bool changed = (addr == NPZ2100_REG_P_BANK)
                || (*config_field(ctx->defaults, cfg->p_bank, addr) != value);
#endif

    if (addr == NPZ2100_REG_P_BANK || addr == NPZ2100_REG_IDLE_RST) {
        /* The pending burst belongs to the bank selected now. */
        err = apply_flush(ctx);
        if (err != NPZ2100_OK) {
            return err;
        }
        if (changed) {
            err = npz2100_reg_write(ctx->hal, addr, value);
            if (err != NPZ2100_OK) {
                return err;
            }
            ctx->written++;
        }
        *field = value;
        return NPZ2100_OK;
    }

    /* Does addr directly follow the pending burst (and its gap)? */
    bool next   = (ctx->run_len > 0u)
               && ((uint16_t)ctx->run_start + ctx->run_len + ctx->gap == addr);
    bool forced = next && ((int16_t)addr <= ctx->must_end);

    if (changed || forced) {
        uint8_t first;
        uint8_t last;
        reg_group(addr, &first, &last);

        if (next) {
            /* Extend the burst; the unchanged gap is written after all. */
            ctx->run_len = (uint8_t)(ctx->run_len + ctx->gap + 1u);
            ctx->gap     = 0u;
        } else {
            err = apply_flush(ctx);
            if (err != NPZ2100_OK) {
                return err;
            }
            /* Start at the first byte of a multi-byte value; bytes before
             * addr are taken from the config struct. */
            ctx->run_start = first;
            ctx->run_len   = (uint8_t)(addr - first + 1u);
        }
        if ((int16_t)last > ctx->must_end) {
            ctx->must_end = last;
        }
    } else if (next && ctx->gap < NPZ2100_BURST_MAX_GAP
               && reg_bridgeable(addr)) {
        /* Unchanged - keep it in case a changed register follows. */
        ctx->gap++;
    } else {
        err = apply_flush(ctx);
        if (err != NPZ2100_OK) {
            return err;
        }
    }

    *field = value;
    return NPZ2100_OK;
}

/* =========================================================================
 * SRAM bytes inside a regmap
 *
 * The configuration tool emits the peripheral init commands as part of the
 * regmap: a segment that starts at SRAM_BANK (0x7F) and runs on into the
 * SRAM window (0x80-0xFF), or a segment that starts inside the window. The
 * shadow does not mirror SRAM, so these bytes cannot be diffed; they are
 * written every time npz2100_map_apply() runs, after the registers.
 *
 * Consecutive bytes are written in bursts of up to NPZ2100_SRAM_RUN_MAX.
 * The SRAM_BANK value the stream writes before the bytes selects their bank.
 * SRAM_BANK is written explicitly before the first burst, whatever the
 * config struct says: after a host power cycle with NPZ2100_SHADOW_ENABLE=0
 * the struct holds the reset default, not the device's bank, and the
 * register pass may have skipped the stream's SRAM_BANK write as unchanged.
 * SRAM_BANK is left at the bank of the last burst and cfg->sram_bank is
 * kept in step.
 * ======================================================================= */

/** Largest SRAM burst from a regmap; the buffer lives on the stack. */
#define NPZ2100_SRAM_RUN_MAX  32u

/** Context passed to the SRAM visitor. */
typedef struct {
    const npz2100_hal_t *hal;
    npz2100_config_t    *cfg;
    uint8_t              bank_known;   /* 1 once SRAM_BANK has been written. */
    uint8_t              stream_bank;  /* Bank the next bytes belong to.    */
    uint8_t              run_bank;     /* Bank of the pending run.          */
    uint8_t              run_start;    /* I2C address of buf[1].            */
    uint8_t              run_len;
    uint8_t              buf[1u + NPZ2100_SRAM_RUN_MAX];
} sram_ctx_t;

/** Write the pending SRAM run, selecting its bank first if needed. */
static npz2100_err_t sram_flush(sram_ctx_t *c)
{
    npz2100_err_t err;

    if (c->run_len == 0u) {
        return NPZ2100_OK;
    }
    if (c->bank_known == 0u || c->cfg->sram_bank != c->run_bank) {
        err = npz2100_reg_write(c->hal, NPZ2100_REG_SRAM_BANK,
                                NPZ2100_SRAM_BANK(c->run_bank));
        if (err != NPZ2100_OK) {
            c->run_len = 0u;
            return err;
        }
        c->cfg->sram_bank = c->run_bank;
        c->bank_known     = 1u;
    }

    c->buf[0] = c->run_start;
    err = c->hal->write(c->hal->i2c_addr, c->buf,
                        1u + (size_t)c->run_len, c->hal->ctx);
    c->run_len = 0u;
    return err;
}

static npz2100_err_t sram_visitor(uint8_t addr, uint8_t value, void *user)
{
    sram_ctx_t *c = (sram_ctx_t *)user;

    if (addr == NPZ2100_REG_SRAM_BANK) {
        /* The pending run keeps its own bank; it is flushed when the next
         * byte does not continue it. */
        c->stream_bank = (uint8_t)(value & NPZ2100_SRAM_BANK_MSK);
        return NPZ2100_OK;
    }
    if (addr < NPZ2100_SRAM_WINDOW_BASE) {
        return NPZ2100_OK;     /* A register; written by the register pass. */
    }

    if (c->run_len != 0u &&
        (addr != (uint8_t)(c->run_start + c->run_len) ||
         c->run_len >= NPZ2100_SRAM_RUN_MAX ||
         c->run_bank != c->stream_bank)) {
        npz2100_err_t err = sram_flush(c);
        if (err != NPZ2100_OK) {
            return err;
        }
    }
    if (c->run_len == 0u) {
        c->run_start = addr;
        c->run_bank  = c->stream_bank;
    }
    c->buf[1u + c->run_len] = value;
    c->run_len++;
    return NPZ2100_OK;
}

static npz2100_err_t sram_count_visitor(uint8_t addr, uint8_t value, void *user)
{
    (void)value;
    if (addr >= NPZ2100_SRAM_WINDOW_BASE) {
        (*(size_t *)user)++;
    }
    return NPZ2100_OK;
}

size_t npz2100_map_sram_count(const uint8_t *map, size_t map_len)
{
    size_t n = 0u;

    if (map == NULL || map_len == 0u) {
        return 0u;
    }
    if (map_walk(map, map_len, sram_count_visitor, &n) != NPZ2100_OK) {
        return 0u;
    }
    return n;
}

/** Write the SRAM-window bytes of a regmap to the device. */
static npz2100_err_t apply_sram_bytes(const npz2100_hal_t *hal,
                                      npz2100_config_t    *cfg,
                                      const uint8_t       *map,
                                      size_t               map_len)
{
    if (npz2100_map_sram_count(map, map_len) == 0u) {
        return NPZ2100_OK;     /* Common case: no SRAM data, no bus traffic. */
    }

    sram_ctx_t c;
    c.hal         = hal;
    c.cfg         = cfg;
    c.bank_known  = 0u;
    c.stream_bank = (uint8_t)(cfg->sram_bank & NPZ2100_SRAM_BANK_MSK);
    c.run_bank    = c.stream_bank;
    c.run_start   = 0u;
    c.run_len     = 0u;

    npz2100_err_t err = map_walk(map, map_len, sram_visitor, &c);
    if (err == NPZ2100_OK) {
        err = sram_flush(&c);
    }

    /* A trailing SRAM_BANK write with no bytes after it is still honoured. */
    if (err == NPZ2100_OK && cfg->sram_bank != c.stream_bank) {
        err = npz2100_reg_write(hal, NPZ2100_REG_SRAM_BANK,
                                NPZ2100_SRAM_BANK(c.stream_bank));
        if (err == NPZ2100_OK) {
            cfg->sram_bank = c.stream_bank;
        }
    }
    return err;
}

npz2100_err_t npz2100_map_apply(const npz2100_hal_t *hal,
                                 npz2100_config_t    *cfg,
                                 const uint8_t        *map,
                                 size_t                map_len,
                                 uint8_t              *written)
{
    if (hal == NULL || cfg == NULL || map == NULL || map_len == 0u) {
        return NPZ2100_ERR_ARG;
    }

    apply_ctx_t ctx = { .hal = hal, .cfg = cfg, .written = 0u,
                        .sram_pass = (npz2100_map_sram_count(map, map_len) > 0u),
                        .run_len = 0u, .gap = 0u, .must_end = -1 };
#if !NPZ2100_SHADOW_ENABLE
    /* Static rather than on the stack: map_apply() runs on the caller's
     * thread, often main with a small stack. Every call writes the same
     * reset values, so concurrent calls cannot see a wrong value. */
    static npz2100_config_t defaults;
    (void)npz2100_config_init_defaults(&defaults);
    ctx.defaults = &defaults;
#endif
    npz2100_err_t err = map_walk(map, map_len, apply_visitor, &ctx);
    if (err == NPZ2100_OK) {
        err = apply_flush(&ctx);
    }

    /* SRAM-window bytes (peripheral init commands): not in the shadow,
     * so written every time, after the registers. */
    if (err == NPZ2100_OK) {
        err = apply_sram_bytes(hal, cfg, map, map_len);
    }

    if (written != NULL) {
        *written = ctx.written;
    }
    return err;
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_regs_read_ll(const npz2100_hal_t *hal,
                                    npz2100_config_t    *cfg,
                                    uint8_t              start_reg,
                                    uint8_t             *data,
                                    size_t               len)
{
    if (hal == NULL || data == NULL || len == 0u ||
        (size_t)start_reg + len > (size_t)NPZ2100_SRAM_WINDOW_BASE) {
        return NPZ2100_ERR_ARG;
    }

    npz2100_err_t err = npz2100_reg_burst_read(hal, start_reg, data, len);
    if (err != NPZ2100_OK) {
        return err;
    }

    config_update(cfg, start_reg, data, len);
    return NPZ2100_OK;
}

npz2100_err_t npz2100_regs_write_ll(const npz2100_hal_t *hal,
                                     npz2100_config_t    *cfg,
                                     uint8_t              start_reg,
                                     const uint8_t       *data,
                                     size_t               len)
{
    if (hal == NULL || data == NULL || len == 0u ||
        (size_t)start_reg + len > (size_t)NPZ2100_SRAM_WINDOW_BASE) {
        return NPZ2100_ERR_ARG;
    }

    npz2100_err_t err = npz2100_reg_burst_write(hal, start_reg, data, len);
    if (err != NPZ2100_OK) {
        return err;
    }

    config_update(cfg, start_reg, data, len);
    return NPZ2100_OK;
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_map_readback(const npz2100_hal_t *hal,
                                    npz2100_config_t    *cfg)
{
    if (hal == NULL || cfg == NULL) {
        return NPZ2100_ERR_ARG;
    }

    npz2100_err_t err;

    /* --- Non-banked registers: one burst per block ---------------------- */
    for (size_t i = 0u; i < sizeof(config_blocks) / sizeof(config_blocks[0]); i++) {
        err = read_block(hal, cfg, config_blocks[i].start, config_blocks[i].len);
        if (err != NPZ2100_OK) { return err; }
    }

    /* The event counter value, CNT_VAL_0–3, between LOGSADDR and CNTCFG. */
    err = read_block(hal, cfg, NPZ2100_REG_CNT_VAL_0,
                     REG_SPAN(NPZ2100_REG_CNT_VAL_0, NPZ2100_REG_CNT_VAL_3));
    if (err != NPZ2100_OK) { return err; }

    /* --- Per-peripheral banks: select, then one 14-byte burst ------------ */
    for (uint8_t slot = 0u; slot <= NPZ2100_P_BANK_MAX; slot++) {
        err = npz2100_reg_write(hal, NPZ2100_REG_P_BANK, NPZ2100_P_BANK(slot));
        if (err != NPZ2100_OK) { return err; }
        cfg->p_bank = slot;

        err = read_block(hal, cfg, NPZ2100_REG_CFGP, PERIPH_REGS);
        if (err != NPZ2100_OK) { return err; }
    }

    /* Restore P_BANK to 0 after readback. */
    err = npz2100_reg_write(hal, NPZ2100_REG_P_BANK, 0x00u);
    if (err != NPZ2100_OK) { return err; }
    cfg->p_bank = 0x00u;

    return NPZ2100_OK;
}

/* -------------------------------------------------------------------------*/

#if NPZ2100_SHADOW_ENABLE

/** Context passed to the diff-count visitor. */
typedef struct {
    npz2100_config_t *cfg;      /* Non-const working copy for field lookup. */
    uint8_t            p_bank;   /* Local bank tracker - does not touch cfg. */
    uint8_t            count;
} diff_ctx_t;

/**
 * @brief Visitor for npz2100_map_diff_count(): counts differing registers
 *        without writing to the device or modifying the caller's shadow.
 *
 * Tracks P_BANK locally (not via cfg->p_bank) so the function remains
 * non-destructive with respect to the caller-supplied shadow.
 */
static npz2100_err_t diff_visitor(uint8_t addr, uint8_t value, void *user)
{
    diff_ctx_t *ctx = (diff_ctx_t *)user;

    if (addr == NPZ2100_REG_P_BANK) {
        ctx->p_bank = value & NPZ2100_P_BANK_MSK;
        /* P_BANK itself still counts as a register that may differ. */
    }

    const uint8_t *field = is_periph_addr(addr)
                          ? shadow_periph_field(ctx->cfg, ctx->p_bank, addr)
                          : shadow_field(ctx->cfg, addr);

    if (field != NULL && *field != value) {
        ctx->count++;
    }
    return NPZ2100_OK;
}

uint8_t npz2100_map_diff_count(const npz2100_config_t *cfg,
                                const uint8_t          *map,
                                size_t                  map_len)
{
    if (cfg == NULL || map == NULL || map_len == 0u) {
        return 0u;
    }

    /*
     * We need a non-const copy of cfg to use shadow_field() - cast is safe
     * because the diff visitor never writes through the pointer.
     */
    npz2100_config_t *cfg_nc = (npz2100_config_t *)(uintptr_t)cfg;

    diff_ctx_t ctx = { .cfg = cfg_nc, .p_bank = cfg->p_bank, .count = 0u };

    /* Malformed streams simply stop early; partial count is still useful. */
    (void)map_walk(map, map_len, diff_visitor, &ctx);

    return ctx.count;
}

#endif /* NPZ2100_SHADOW_ENABLE */

/* -------------------------------------------------------------------------*/

/**
 * @brief Visitor for npz2100_map_validate(): no-op, just lets map_walk()
 *        do the framing checks.
 */
static npz2100_err_t validate_visitor(uint8_t addr, uint8_t value, void *user)
{
    (void)addr;
    (void)value;
    (void)user;
    return NPZ2100_OK;
}

npz2100_err_t npz2100_map_validate(const uint8_t *map, size_t map_len)
{
    if (map == NULL || map_len == 0u) {
        return NPZ2100_ERR_ARG;
    }
    return map_walk(map, map_len, validate_visitor, NULL);
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_shadow_write_reg(const npz2100_hal_t *hal,
                                        npz2100_config_t    *cfg,
                                        uint8_t              addr,
                                        uint8_t              value)
{
    if (hal == NULL) {
        return NPZ2100_ERR_ARG;
    }

    npz2100_err_t err = npz2100_reg_write(hal, addr, value);
    if (err != NPZ2100_OK) {
        return err;
    }

    /* Update the config struct - best-effort; ignore unknown addresses. */
    if (cfg != NULL) {
        uint8_t *field = shadow_field(cfg, addr);
        if (field != NULL) {
            *field = value;
        }
    }

    return NPZ2100_OK;
}

/* =========================================================================
 * Typed configuration helpers - shadow writes only
 * ======================================================================= */

npz2100_err_t npz2100_sys_set(npz2100_config_t       *cfg,
                               const npz2100_sys_cfg_t *sys)
{
    if (cfg == NULL || sys == NULL) {
        return NPZ2100_ERR_ARG;
    }
    if (sys->tout_value < NPZ2100_TOUT_MIN_SAFE) {
        return NPZ2100_ERR_ARG;
    }

    /* SYSCFG1: peripheral wake-up enables + mode. */
    cfg->syscfg1 = (uint8_t)(sys->wup_periph_mask & 0x3Fu)
                 | NPZ2100_SYSCFG1_WUPMOD(sys->wup_any ? 0u : 1u);

    /* SYSCFG2: ADC wake-ups + clock + TOUT extension. */
    cfg->syscfg2 = (uint8_t)(sys->wup_adc1 ? NPZ2100_SYSCFG2_WUP_ADC1_MSK : 0u)
                 | (uint8_t)(sys->wup_adc2 ? NPZ2100_SYSCFG2_WUP_ADC2_MSK : 0u)
                 | (uint8_t)(sys->wup_adc3 ? NPZ2100_SYSCFG2_WUP_ADC3_MSK : 0u)
                 | NPZ2100_SYSCFG2_TOUT_EXT(sys->tout_ext ? 1u : 0u)
                 | NPZ2100_SYSCFG2_SCLK_SEL(sys->clk_xo  ? 1u : 0u);

    /* TOUT: 16-bit little-endian. */
    cfg->tout_l = (uint8_t)(sys->tout_value & 0xFFu);
    cfg->tout_h = (uint8_t)((sys->tout_value >> 8u) & 0xFFu);

    return NPZ2100_OK;
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_timer_set(npz2100_config_t         *cfg,
                                 const npz2100_timer_cfg_t *timer)
{
    if (cfg == NULL || timer == NULL) {
        return NPZ2100_ERR_ARG;
    }

    /* GCT: fractional + 32-bit seconds. */
    cfg->gct_ms = timer->gct_ms & NPZ2100_GCT_MS_MSK;
    cfg->gct_0  = (uint8_t)( timer->gct_seconds        & 0xFFu);
    cfg->gct_1  = (uint8_t)((timer->gct_seconds >>  8u) & 0xFFu);
    cfg->gct_2  = (uint8_t)((timer->gct_seconds >> 16u) & 0xFFu);
    cfg->gct_3  = (uint8_t)((timer->gct_seconds >> 24u) & 0xFFu);

    /* GCTALM: 32-bit alarm in seconds. */
    cfg->gct_alm_0 = (uint8_t)( timer->alarm_seconds        & 0xFFu);
    cfg->gct_alm_1 = (uint8_t)((timer->alarm_seconds >>  8u) & 0xFFu);
    cfg->gct_alm_2 = (uint8_t)((timer->alarm_seconds >> 16u) & 0xFFu);
    cfg->gct_alm_3 = (uint8_t)((timer->alarm_seconds >> 24u) & 0xFFu);

    /* WDOG: 16-bit value in 2-second units. */
    cfg->wdog_l = (uint8_t)( timer->wdog_value       & 0xFFu);
    cfg->wdog_h = (uint8_t)((timer->wdog_value >> 8u) & 0xFFu);

    /* GTC_CFG: alarm enable + watchdog enable. */
    cfg->gtc_cfg = NPZ2100_GTC_CFG_GTC_AEN(timer->alarm_enable  ? 1u : 0u)
                 | NPZ2100_GTC_CFG_WDOGEN(timer->wdog_enable    ? 1u : 0u);

    return NPZ2100_OK;
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_pa_set(npz2100_config_t      *cfg,
                               const npz2100_pa_cfg_t *pa)
{
    if (cfg == NULL || pa == NULL) {
        return NPZ2100_ERR_ARG;
    }
    if (pa->src > NPZ2100_PA_SRC_ADC3) {
        return NPZ2100_ERR_ARG;
    }

    cfg->pa_cfg = NPZ2100_PA_CFG_PA_EN(pa->enable   ? 1u : 0u)
                | NPZ2100_PA_CFG_PA_NOWUP(pa->no_wup ? 1u : 0u)
                | NPZ2100_PA_CFG_PA_SRC(pa->src);

    return NPZ2100_OK;
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_io_set(npz2100_config_t *cfg,
                               uint8_t           iocfg1,
                               uint8_t           iocfg2,
                               uint8_t           iocfg3,
                               uint8_t           iocfg4,
                               uint8_t           iocfg5)
{
    if (cfg == NULL) {
        return NPZ2100_ERR_ARG;
    }

    cfg->iocfg1 = iocfg1;
    cfg->iocfg2 = iocfg2;
    cfg->iocfg3 = iocfg3;
    cfg->iocfg4 = iocfg4;
    cfg->iocfg5 = iocfg5;

    return NPZ2100_OK;
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_periph_set(npz2100_config_t           *cfg,
                                   uint8_t                     slot,
                                   const npz2100_periph_cfg_t *pcfg)
{
    if (cfg == NULL || pcfg == NULL || slot > NPZ2100_P_BANK_MAX) {
        return NPZ2100_ERR_ARG;
    }
    if (pcfg->period == 0u) {
        /* Datasheet §2.2.23: period = 0 causes undefined behaviour. */
        return NPZ2100_ERR_ARG;
    }

    npz2100_periph_shadow_t *p = &cfg->periph[slot];

    /* CFGP */
    p->cfgp = NPZ2100_CFGP_PWMOD(pcfg->pwmod)
            | NPZ2100_CFGP_TMOD(pcfg->tmod)
            | NPZ2100_CFGP_PLOG(pcfg->log_en  ? 1u : 0u)
            | NPZ2100_CFGP_PLOGTS(pcfg->log_ts ? 1u : 0u)
            | NPZ2100_CFGP_PLOGF(pcfg->log_freq);

    /* IOP */
    p->iop = NPZ2100_IOP_SELPSW(pcfg->psw_pin)
           | NPZ2100_IOP_SELINT(pcfg->int_pin)
           | NPZ2100_IOP_SELCSN(pcfg->csn_pin)
           | NPZ2100_IOP_PAMOD(pcfg->pamod);

    /* MODP */
    p->modp = NPZ2100_MODP_CMOD(pcfg->inv_cmp   ? 1u : 0u)
            | NPZ2100_MODP_DTYPE(pcfg->dtype)
            | NPZ2100_MODP_SEQRW(pcfg->seq_rw    ? 1u : 0u)
            | NPZ2100_MODP_WUNAK(pcfg->wunak      ? 1u : 0u)
            | NPZ2100_MODP_SWPRREG(pcfg->swap_bytes ? 1u : 0u)
            | NPZ2100_MODP_SPIMOD(pcfg->spi_mode);

    /* PERP: 16-bit polling period. */
    p->perp_l = (uint8_t)( pcfg->period       & 0xFFu);
    p->perp_h = (uint8_t)((pcfg->period >> 8u) & 0xFFu);

    /* NCMDP */
    p->ncmdp = pcfg->ncmd;

    /* ADDRP: I²C address (7-bit) or SPI read byte count. */
    p->addrp = pcfg->i2c_addr;   /* Tool sets this to byte count if use_spi. */

    /* RREGP */
    p->rregp = pcfg->read_reg;

    /* THROVP: 16-bit over-threshold. */
    p->throvp_l = (uint8_t)( pcfg->threshold_ov       & 0xFFu);
    p->throvp_h = (uint8_t)((pcfg->threshold_ov >> 8u) & 0xFFu);

    /* THRUNP: 16-bit under-threshold. */
    p->thrunp_l = (uint8_t)( pcfg->threshold_un       & 0xFFu);
    p->thrunp_h = (uint8_t)((pcfg->threshold_un >> 8u) & 0xFFu);

    /* TWTP */
    p->twtp = pcfg->twt;

    /* TCFGP */
    p->tcfgp = NPZ2100_TCFGP_TWT_EN(pcfg->twt_en     ? 1u : 0u)
             | NPZ2100_TCFGP_TWT_EXT(pcfg->twt_ext    ? 1u : 0u)
             | NPZ2100_TCFGP_TINIT_EN(pcfg->tinit_en  ? 1u : 0u)
             | NPZ2100_TCFGP_TINIT_EXT(pcfg->tinit_ext ? 1u : 0u)
             | NPZ2100_TCFGP_I2CRET(pcfg->i2c_retries)
             | NPZ2100_TCFGP_I2CRO(pcfg->i2c_read_only ? 1u : 0u)
             | NPZ2100_TCFGP_SPIEN(pcfg->use_spi       ? 1u : 0u);

    return NPZ2100_OK;
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_periph_apply(const npz2100_hal_t *hal,
                                    npz2100_config_t    *cfg,
                                    uint8_t              slot)
{
    if (hal == NULL || cfg == NULL || slot > NPZ2100_P_BANK_MAX) {
        return NPZ2100_ERR_ARG;
    }

    /*
     * npz2100_periph_set() has already placed the *desired* values into the
     * shadow, so this call pushes that intent to the device unconditionally.
     * Use npz2100_map_apply() (with a tool-generated stream) if you need
     * diff semantics for this peripheral.
     *
     * The banked registers CFGP–TCFGP (0x20–0x2D) are 14 consecutive
     * addresses: select the bank, then write them all in one burst.
     */
    npz2100_err_t err = ensure_bank(hal, cfg, slot);
    if (err != NPZ2100_OK) { return err; }

    err = write_block(hal, cfg, NPZ2100_REG_CFGP, PERIPH_REGS);
    if (err != NPZ2100_OK) { return err; }

    /* Restore P_BANK to slot 0 after apply. */
    return ensure_bank(hal, cfg, 0x00u);
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_config_flush_ll(const npz2100_hal_t *hal,
                                       npz2100_config_t    *cfg)
{
    if (hal == NULL || cfg == NULL) {
        return NPZ2100_ERR_ARG;
    }

    npz2100_err_t err;

    for (size_t i = 0u; i < sizeof(config_blocks) / sizeof(config_blocks[0]); i++) {
        err = write_block(hal, cfg, config_blocks[i].start, config_blocks[i].len);
        if (err != NPZ2100_OK) { return err; }
    }

    for (uint8_t slot = 0u; slot <= NPZ2100_P_BANK_MAX; slot++) {
        err = ensure_bank(hal, cfg, slot);
        if (err != NPZ2100_OK) { return err; }
        err = write_block(hal, cfg, NPZ2100_REG_CFGP, PERIPH_REGS);
        if (err != NPZ2100_OK) { return err; }
    }

    return ensure_bank(hal, cfg, 0x00u);
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_adc_set(npz2100_config_t       *cfg,
                                const npz2100_adc_cfg_t *adc)
{
    if (cfg == NULL || adc == NULL) {
        return NPZ2100_ERR_ARG;
    }

    cfg->adccfg = NPZ2100_ADCCFG_ADC1_EN(adc->en_ch1    ? 1u : 0u)
                | NPZ2100_ADCCFG_ADC2_EN(adc->en_ch2    ? 1u : 0u)
                | NPZ2100_ADCCFG_ADC3_EN(adc->en_ch3    ? 1u : 0u)
                | NPZ2100_ADCCFG_ADC_CLK_SEL(adc->clk_sel)
                | NPZ2100_ADCCFG_ADC1_PSYNC(adc->psync_ch1 ? 1u : 0u)
                | NPZ2100_ADCCFG_ADC2_PSYNC(adc->psync_ch2 ? 1u : 0u);

    cfg->throva1 = adc->throva[0];
    cfg->thruna1 = adc->thruna[0];
    cfg->throva2 = adc->throva[1];
    cfg->thruna2 = adc->thruna[1];
    cfg->throva3 = adc->throva[2];
    cfg->thruna3 = adc->thruna[2];

    return NPZ2100_OK;
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_log_set(npz2100_config_t       *cfg,
                                const npz2100_log_cfg_t *log)
{
    if (cfg == NULL || log == NULL) {
        return NPZ2100_ERR_ARG;
    }

    cfg->logcfg  = NPZ2100_LOGCFG_LOG_EN(log->enable  ? 1u : 0u)
                 | NPZ2100_LOGCFG_LOG_ROT(log->rotate  ? 1u : 0u);
    cfg->logsaddr = log->start_addr;

    return NPZ2100_OK;
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_counter_set(npz2100_config_t           *cfg,
                                   const npz2100_counter_cfg_t *cnt)
{
    if (cfg == NULL || cnt == NULL) {
        return NPZ2100_ERR_ARG;
    }
    if (cnt->src > NPZ2100_CNT_SRC_SW_LP4) {
        return NPZ2100_ERR_ARG;
    }

    cfg->cntcfg    = NPZ2100_CNTCFG_CNT_EN(cnt->enable ? 1u : 0u)
                   | NPZ2100_CNTCFG_CNT_SRC(cnt->src);
    cfg->cnt_trig_0 = (uint8_t)( cnt->trigger        & 0xFFu);
    cfg->cnt_trig_1 = (uint8_t)((cnt->trigger >>  8u) & 0xFFu);
    cfg->cnt_trig_2 = (uint8_t)((cnt->trigger >> 16u) & 0xFFu);
    cfg->cnt_trig_3 = (uint8_t)((cnt->trigger >> 24u) & 0xFFu);

    return NPZ2100_OK;
}

/* =========================================================================
 * Device control helpers
 * ======================================================================= */

npz2100_err_t npz2100_probe_ll(const npz2100_hal_t *hal)
{
    if (hal == NULL) {
        return NPZ2100_ERR_ARG;
    }

    uint8_t id;
    npz2100_err_t err = npz2100_reg_read(hal, NPZ2100_REG_ID, &id);
    if (err != NPZ2100_OK) {
        return err;
    }
    return (id == NPZ2100_ID_EXPECTED) ? NPZ2100_OK : NPZ2100_ERR_DEV;
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_status_read(const npz2100_hal_t *hal,
                                   uint8_t             *sta1,
                                   uint8_t             *sta2,
                                   uint8_t             *sta3)
{
    if (hal == NULL) {
        return NPZ2100_ERR_ARG;
    }

    /*
     * STA1–STA3 are at 0x02, 0x03, 0x04 - three consecutive bytes.
     * Read in a single burst to minimise bus time and ensure the watchdog
     * kick (triggered by reading STA1/STA2) covers both registers.
     */
    uint8_t buf[3];
    npz2100_err_t err = npz2100_reg_burst_read(hal, NPZ2100_REG_STA1, buf, 3u);
    if (err != NPZ2100_OK) {
        return err;
    }

    if (sta1 != NULL) { *sta1 = buf[0]; }
    if (sta2 != NULL) { *sta2 = buf[1]; }
    if (sta3 != NULL) { *sta3 = buf[2]; }

    return NPZ2100_OK;
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_periph_read_value_ll(const npz2100_hal_t    *hal,
                                         const npz2100_config_t *cfg,
                                         uint8_t                 slot,
                                         uint16_t               *value)
{
    if (hal == NULL || cfg == NULL || value == NULL || slot > NPZ2100_P_BANK_MAX) {
        return NPZ2100_ERR_ARG;
    }

    /* Select the peripheral bank. */
    npz2100_err_t err = npz2100_reg_write(hal, NPZ2100_REG_P_BANK,
                                           NPZ2100_P_BANK(slot));
    if (err != NPZ2100_OK) { return err; }

    /* Read VALP_L and VALP_H in one burst. */
    uint8_t buf[2];
    err = npz2100_reg_burst_read(hal, NPZ2100_REG_VALP_L, buf, 2u);
    if (err != NPZ2100_OK) { return err; }

    /* Reconstruct value per dtype. */
    uint8_t dtype = NPZ2100_MODP_DTYPE_GET(cfg->periph[slot].modp);
    if (dtype == NPZ2100_DTYPE_UINT8) {
        *value = (uint16_t)buf[0];
    } else {
        *value = (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8u));
    }

    /* Restore P_BANK to 0. */
    return npz2100_reg_write(hal, NPZ2100_REG_P_BANK, 0x00u);
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_adc_read(const npz2100_hal_t *hal,
                                uint8_t             *ch1,
                                uint8_t             *ch2,
                                uint8_t             *ch3)
{
    if (hal == NULL) {
        return NPZ2100_ERR_ARG;
    }

    /* VAL_ADC1–3 at 0x47, 0x48, 0x49 - three consecutive bytes. */
    uint8_t buf[3];
    npz2100_err_t err = npz2100_reg_burst_read(hal, NPZ2100_REG_VAL_ADC1,
                                                buf, 3u);
    if (err != NPZ2100_OK) { return err; }

    if (ch1 != NULL) { *ch1 = buf[0]; }
    if (ch2 != NULL) { *ch2 = buf[1]; }
    if (ch3 != NULL) { *ch3 = buf[2]; }

    return NPZ2100_OK;
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_sram_write_ll(const npz2100_hal_t *hal,
                                  npz2100_config_t    *cfg,
                                  uint8_t              sram_addr,
                                  const uint8_t       *data,
                                  size_t               len)
{
    if (hal == NULL || cfg == NULL || data == NULL || len == 0u) {
        return NPZ2100_ERR_ARG;
    }
    /* Bounds check: sram_addr + len must not exceed SRAM size. */
    if ((size_t)sram_addr + len > (size_t)NPZ2100_SRAM_TOTAL_SIZE) {
        return NPZ2100_ERR_ARG;
    }

    npz2100_err_t err;
    size_t        written   = 0u;
    uint8_t       cur_addr  = sram_addr;

    while (written < len) {
        /* Determine which bank this byte lives in. */
        uint8_t bank        = (cur_addr >= NPZ2100_SRAM_WINDOW_SIZE) ? 1u : 0u;
        uint8_t bank_offset = (bank == 0u)
                            ? cur_addr
                            : (uint8_t)(cur_addr - NPZ2100_SRAM_WINDOW_SIZE);
        uint8_t i2c_addr_w  = (uint8_t)(NPZ2100_SRAM_WINDOW_BASE + bank_offset);

        /* How many bytes remain in this bank? */
        size_t bank_remaining = (bank == 0u)
                              ? (size_t)(NPZ2100_SRAM_WINDOW_SIZE - bank_offset)
                              : (size_t)(NPZ2100_SRAM_TOTAL_SIZE  - cur_addr);
        size_t chunk = (len - written) < bank_remaining
                     ? (len - written)
                     : bank_remaining;

        /* Switch bank only if needed. */
        if (cfg->sram_bank != bank) {
            err = npz2100_reg_write(hal, NPZ2100_REG_SRAM_BANK,
                                    NPZ2100_SRAM_BANK(bank));
            if (err != NPZ2100_OK) { return err; }
            cfg->sram_bank = bank;
        }

        /* Burst-write the chunk. */
        err = npz2100_reg_burst_write(hal, i2c_addr_w, data + written, chunk);
        if (err != NPZ2100_OK) { return err; }

        written  += chunk;
        cur_addr += (uint8_t)chunk;
    }

    return NPZ2100_OK;
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_sram_read_ll(const npz2100_hal_t *hal,
                                 npz2100_config_t    *cfg,
                                 uint8_t              sram_addr,
                                 uint8_t             *data,
                                 size_t               len)
{
    if (hal == NULL || cfg == NULL || data == NULL || len == 0u) {
        return NPZ2100_ERR_ARG;
    }
    if ((size_t)sram_addr + len > (size_t)NPZ2100_SRAM_TOTAL_SIZE) {
        return NPZ2100_ERR_ARG;
    }

    npz2100_err_t err;
    size_t        read_n   = 0u;
    uint8_t       cur_addr = sram_addr;

    while (read_n < len) {
        uint8_t bank        = (cur_addr >= NPZ2100_SRAM_WINDOW_SIZE) ? 1u : 0u;
        uint8_t bank_offset = (bank == 0u)
                            ? cur_addr
                            : (uint8_t)(cur_addr - NPZ2100_SRAM_WINDOW_SIZE);
        uint8_t i2c_addr_r  = (uint8_t)(NPZ2100_SRAM_WINDOW_BASE + bank_offset);

        size_t bank_remaining = (bank == 0u)
                              ? (size_t)(NPZ2100_SRAM_WINDOW_SIZE - bank_offset)
                              : (size_t)(NPZ2100_SRAM_TOTAL_SIZE  - cur_addr);
        size_t chunk = (len - read_n) < bank_remaining
                     ? (len - read_n)
                     : bank_remaining;

        if (cfg->sram_bank != bank) {
            err = npz2100_reg_write(hal, NPZ2100_REG_SRAM_BANK,
                                    NPZ2100_SRAM_BANK(bank));
            if (err != NPZ2100_OK) { return err; }
            cfg->sram_bank = bank;
        }

        err = npz2100_reg_burst_read(hal, i2c_addr_r, data + read_n, chunk);
        if (err != NPZ2100_OK) { return err; }

        read_n   += chunk;
        cur_addr += (uint8_t)chunk;
    }

    return NPZ2100_OK;
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_enter_idle_ll(const npz2100_hal_t *hal)
{
    if (hal == NULL) {
        return NPZ2100_ERR_ARG;
    }
    /*
     * IDLE_RST always reads 0x00 - no shadow update needed.
     * One I²C transaction hands control to the nPZ2100.
     */
    return npz2100_reg_write(hal, NPZ2100_REG_IDLE_RST,
                              NPZ2100_IDLE_RST_ENTER_IDLE);
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_soft_reset_ll(const npz2100_hal_t *hal)
{
    if (hal == NULL) {
        return NPZ2100_ERR_ARG;
    }
    return npz2100_reg_write(hal, NPZ2100_REG_IDLE_RST,
                              NPZ2100_IDLE_RST_SOFT_RESET);
}

/* -------------------------------------------------------------------------*/

npz2100_err_t npz2100_read_uuid_ll(const npz2100_hal_t *hal, uint8_t uuid[4])
{
    if (hal == NULL || uuid == NULL) {
        return NPZ2100_ERR_ARG;
    }

    npz2100_err_t err = npz2100_reg_write(hal, NPZ2100_REG_UUID_UNLOCK,
                                          NPZ2100_UUID_UNLOCK_KEY);
    if (err != NPZ2100_OK) {
        return err;
    }
    return npz2100_reg_burst_read(hal, NPZ2100_REG_UUID_0, uuid,
                                  NPZ2100_UUID_LEN);
}
