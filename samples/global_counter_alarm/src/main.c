/*
 * Copyright (c) Nanopower Semiconductor AS
 * SPDX-License-Identifier: Apache-2.0
 *
 * samples/global_counter_alarm/src/main.c
 * ---------------------------------------
 * Global time counter + alarm sample for the nPZ2100 driver on Nordic nRF5x
 * platforms running NCS 3.3.4.
 *
 * What it does
 * ------------
 *   - First boot (nPZ2100 not yet configured): set the global time counter
 *     (GCT) to APP_GCT_START_S and arm the alarm (GCT_ALM) APP_ALARM_DELAY_S
 *     seconds later.
 *   - Every boot: print the counter, the alarm time and how long until the
 *     alarm fires.
 *   - The periodic time-out (TOUT) wakes the nRF5x every 5 seconds; the
 *     setting lives in npz2100_regmap.h.
 *   - When the alarm wakes the nRF5x, it is reported and then disabled, so
 *     it fires once.
 *
 * Power architecture
 * ------------------
 * The nPZ2100 controls the nRF5x power supply via SW_HP. When idle the
 * nPZ2100 cuts power to the nRF5x completely, so every boot of this
 * application is one wake cycle. The nPZ2100 keeps counting and keeps its
 * registers while the nRF5x is off.
 *
 * First-boot detection
 * --------------------
 * The application keeps no state of its own across power cycles. The
 * npz2100_readback() done on every boot already brings the GCT block
 * (0x10–0x1B) into the driver shadow, and GCT_ALM and GTC_CFG are zero after
 * an nPZ2100 power-on or system reset. An alarm time of zero with the alarm
 * disabled therefore means "not yet configured", at no extra I²C cost. A
 * soft reset keeps the configuration, so the counter is not reset again.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include <drivers/npz2100.h>
#include "npz2100_regmap.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/** Value the global time counter is set to on first boot, in seconds. */
#define APP_GCT_START_S    1000u

/** Alarm delay after the counter is set, in seconds. */
#define APP_ALARM_DELAY_S  30u

/* -------------------------------------------------------------------------
 * Device handle - obtained from the DT node labelled "npz2100".
 * Defined by the npz2100 shield (npz2100_module/boards/shields/npz2100/).
 * ---------------------------------------------------------------------- */
static const struct device *const npz2100 =
	DEVICE_DT_GET(DT_NODELABEL(npz2100));

/** Decoded GCT block: GCT_MS, GCT_0–3, GCT_ALM_0–3, WDOG_L/H, GTC_CFG. */
struct gct_status {
	uint32_t seconds;   /**< Counter, whole seconds.         */
	uint8_t  frac;      /**< Counter, 1/16 s ticks (0–15).   */
	uint32_t alarm;     /**< Alarm time, seconds.            */
	bool     alarm_en;  /**< GTC_CFG.GTC_AEN.                */
};

static uint32_t get_le32(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
	return (uint32_t)b0
	     | ((uint32_t)b1 << 8u)
	     | ((uint32_t)b2 << 16u)
	     | ((uint32_t)b3 << 24u);
}

/* -------------------------------------------------------------------------
 * gct_status_get
 *
 * Decodes GCT_MS (0x10) through GTC_CFG (0x1B) from the driver shadow.
 * The shadow must be current: call after npz2100_readback(), or after the
 * GCT registers were written through the driver.
 * ---------------------------------------------------------------------- */
static void gct_status_get(struct gct_status *st)
{
	const npz2100_config_t *s = npz2100_get_shadow(npz2100);

	st->frac     = s->gct_ms & NPZ2100_GCT_MS_MSK;
	st->seconds  = get_le32(s->gct_0, s->gct_1, s->gct_2, s->gct_3);
	st->alarm    = get_le32(s->gct_alm_0, s->gct_alm_1,
				s->gct_alm_2, s->gct_alm_3);
	st->alarm_en = (s->gtc_cfg & NPZ2100_GTC_CFG_GTC_AEN_MSK) != 0u;
}

/* -------------------------------------------------------------------------
 * gct_first_boot_setup
 *
 * Sets the counter to APP_GCT_START_S and arms the alarm APP_ALARM_DELAY_S
 * later. The timer settings go into the shadow, and npz2100_shadow_flush()
 * writes them out in address order, so GCT_3 is written after GCT_MS–GCT_2
 * and latches the new counter value. The flush rewrites every register, so
 * it is only used here, once, right after npz2100_readback().
 * The nPZ2100 watchdog is left disabled (its reset state).
 * ---------------------------------------------------------------------- */
static int gct_first_boot_setup(void)
{
	const npz2100_timer_cfg_t timer = {
		.gct_seconds   = APP_GCT_START_S,
		.gct_ms        = 0u,
		.alarm_seconds = APP_GCT_START_S + APP_ALARM_DELAY_S,
		.alarm_enable  = true,
		.wdog_value    = 0u,
		.wdog_enable   = false,
	};

	LOG_INF("First boot - counter set to %u s, alarm at %u s",
		timer.gct_seconds, timer.alarm_seconds);

	if (npz2100_timer_set(npz2100_get_shadow(npz2100), &timer) != NPZ2100_OK) {
		return -EINVAL;
	}

	return npz2100_shadow_flush(npz2100);
}

/* -------------------------------------------------------------------------
 * gct_alarm_disable
 *
 * Clears GTC_CFG.GTC_AEN. Applied as a one-register regmap segment, which
 * writes GTC_CFG only: a shadow flush would also rewrite the running
 * counter with the value read at boot.
 * ---------------------------------------------------------------------- */
static int gct_alarm_disable(void)
{
	const npz2100_config_t *s = npz2100_get_shadow(npz2100);
	const uint8_t segment[] = {
		2u, NPZ2100_REG_GTC_CFG,
		(uint8_t)(s->gtc_cfg & ~NPZ2100_GTC_CFG_GTC_AEN_MSK),
	};

	return npz2100_apply_regmap(npz2100, segment, sizeof(segment));
}

/* -------------------------------------------------------------------------
 * gct_status_print
 * ---------------------------------------------------------------------- */
static void gct_status_print(const struct gct_status *st)
{
	/* 1/16 s ticks to milliseconds: 1000 / 16 = 62.5. */
	uint32_t ms = ((uint32_t)st->frac * 625u) / 10u;

	if (!st->alarm_en) {
		LOG_INF("Counter: %u.%03u s, alarm off (was %u s)",
			st->seconds, ms, st->alarm);
	} else if (st->seconds < st->alarm) {
		LOG_INF("Counter: %u.%03u s, alarm at %u s (in %u s)",
			st->seconds, ms, st->alarm, st->alarm - st->seconds);
	} else {
		LOG_INF("Counter: %u.%03u s, alarm at %u s (due)",
			st->seconds, ms, st->alarm);
	}
}

/* -------------------------------------------------------------------------
 * main
 *
 * Executes once per nRF5x boot (= once per nPZ2100 wake cycle).
 * Does not contain a loop — the final call to npz2100_enter_idle() causes
 * the nPZ2100 to cut power to the nRF5x.
 * ---------------------------------------------------------------------- */
int main(void)
{
	int ret;
	npz2100_wake_reason_t reason;
	struct gct_status st;

	LOG_INF("--- nPZ2100 global counter alarm sample boot ---");

	if (!device_is_ready(npz2100)) {
		LOG_ERR("nPZ2100 device not ready");
		while (true) {
			k_msleep(1000);
		}
	}

	/* ------------------------------------------------------------------ */
	/* 1. Read wake-up reason.  MUST be the first nPZ2100 operation.      */
	/* ------------------------------------------------------------------ */
	ret = npz2100_boot_status(npz2100, &reason);
	if (ret != 0) {
		LOG_ERR("boot_status failed: %d - cannot determine wake reason", ret);
		reason = (npz2100_wake_reason_t){ 0 };
	}

	/* ------------------------------------------------------------------ */
	/* 2. Sync shadow from device, including the GCT block.               */
	/* ------------------------------------------------------------------ */
	ret = npz2100_readback(npz2100);
	if (ret != 0) {
		LOG_ERR("readback failed: %d", ret);
		goto idle;
	}

	/* ------------------------------------------------------------------ */
	/* 3. Apply the static configuration: TOUT = 5 s (see regmap).        */
	/*    GCT, GCT_ALM and GTC_CFG are runtime parameters and stay at      */
	/*    their reset value in the regmap, so they are never reverted.     */
	/* ------------------------------------------------------------------ */
	ret = npz2100_apply_regmap(npz2100, npz2100_regmap, sizeof(npz2100_regmap));
	if (ret != 0) {
		LOG_ERR("apply_regmap failed: %d", ret);
	}

	/* ------------------------------------------------------------------ */
	/* 4. Read the counter; configure it if this is the first boot.       */
	/* ------------------------------------------------------------------ */
	gct_status_get(&st);

	if (st.alarm == 0u && !st.alarm_en) {
		ret = gct_first_boot_setup();
		if (ret != 0) {
			LOG_ERR("Setting global counter failed: %d", ret);
			goto idle;
		}

		gct_status_get(&st);
	}

	/* ------------------------------------------------------------------ */
	/* 5. Handle the wake reason.                                          */
	/* ------------------------------------------------------------------ */
	if (reason.timeout) {
		LOG_INF("Periodic time-out wake");
	}

	if (reason.alarm) {
		LOG_INF("Global counter alarm fired");

		/* One-shot: disable the alarm so it does not wake again. */
		ret = gct_alarm_disable();
		if (ret != 0) {
			LOG_ERR("Disabling alarm failed: %d", ret);
		} else {
			st.alarm_en = false;
		}
	}

	gct_status_print(&st);

idle:
	/* ------------------------------------------------------------------ */
	/* 6. Re-enter idle.  nPZ2100 will cut nRF5x power.                   */
	/*    LOG_PANIC() flushes the deferred log buffer first so no output  */
	/*    is lost when power is cut.                                       */
	/* ------------------------------------------------------------------ */
	LOG_INF("Returning to idle - next wake in 5 s");
	LOG_PANIC();

	ret = npz2100_enter_idle(npz2100);

	/*
	 * Reaching here means the I²C write to IDLE_RST failed.
	 */
	LOG_ERR("enter_idle failed: %d", ret);
	while (true) {
		k_msleep(1000);
	}

	return 0; /* unreachable */
}
