/*
 * Copyright (c) Nanopower Semiconductor AS
 * SPDX-License-Identifier: Apache-2.0
 *
 * samples/watchdog/src/main.c
 * ---------------------------
 * nPZ2100 watchdog sample for Nordic nRF5x platforms running NCS 3.3.4.
 *
 * What it does
 * ------------
 *   - The regmap enables the nPZ2100 watchdog (about 6 s) and a 3 s
 *     time-out wake (TOUT). Every boot reads STA1/STA2, which restarts the
 *     watchdog, so normal wakes never trigger it.
 *   - Start-up (a boot not caused by a time-out or the watchdog): the demo
 *     is armed.
 *   - First time-out wake after start-up: the demo is disarmed and the
 *     application simulates a hang — it logs once a second and never
 *     returns to idle. The watchdog resets the nPZ2100 about 6 s later,
 *     which power-cycles the nRF5x.
 *   - Watchdog reset boot: the reset is reported, and the 3 s wakes
 *     continue normally from then on.
 *
 * Power architecture
 * ------------------
 * The nPZ2100 controls the nRF5x power supply via SW_HP. When idle the
 * nPZ2100 cuts power to the nRF5x completely, so every boot of this
 * application is one wake cycle.
 *
 * Demo state
 * ----------
 * The nRF5x keeps no RAM across wakes, so whether the hang has already been
 * shown is kept in one byte of nPZ2100 SRAM (APP_SRAM_DEMO_ADDR). The
 * watchdog reset is a soft reset, which keeps the SRAM and the
 * configuration.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include <drivers/npz2100.h>
#include "npz2100_regmap.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/** nPZ2100 SRAM byte holding the demo state. */
#define APP_SRAM_DEMO_ADDR  0x00u

/** Demo state: hang on the next time-out wake. Any other value: don't. */
#define APP_DEMO_ARMED      0xA5u
#define APP_DEMO_DONE       0x00u

/* -------------------------------------------------------------------------
 * Device handle - obtained from the DT node labelled "npz2100".
 * Defined by the npz2100 shield (npz2100_module/boards/shields/npz2100/).
 * ---------------------------------------------------------------------- */
static const struct device *const npz2100 =
	DEVICE_DT_GET(DT_NODELABEL(npz2100));

static int demo_state_set(uint8_t state)
{
	return npz2100_sram_write(npz2100, APP_SRAM_DEMO_ADDR, &state, 1u);
}

static int demo_state_get(uint8_t *state)
{
	return npz2100_sram_read(npz2100, APP_SRAM_DEMO_ADDR, state, 1u);
}

/* -------------------------------------------------------------------------
 * simulate_hang
 *
 * Stands in for application code that gets stuck. It never reads STA1/STA2
 * and never calls npz2100_enter_idle(), so the watchdog is not restarted
 * and the nPZ2100 resets the system once it expires.
 * ---------------------------------------------------------------------- */
static void simulate_hang(void)
{
	uint32_t seconds = 0u;

	LOG_WRN("Simulating a hang - waiting for the watchdog");

	while (true) {
		LOG_INF("Hung for %u s", seconds);
		k_msleep(1000);
		seconds++;
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
	uint8_t state;

	LOG_INF("--- nPZ2100 watchdog sample boot ---");

	if (!device_is_ready(npz2100)) {
		LOG_ERR("nPZ2100 device not ready");
		while (true) {
			k_msleep(1000);
		}
	}

	/* ------------------------------------------------------------------ */
	/* 1. Read wake-up reason.  MUST be the first nPZ2100 operation.      */
	/*    Reading STA1/STA2 also restarts the watchdog.                    */
	/* ------------------------------------------------------------------ */
	ret = npz2100_boot_status(npz2100, &reason);
	if (ret != 0) {
		LOG_ERR("boot_status failed: %d - cannot determine wake reason", ret);
		reason = (npz2100_wake_reason_t){ 0 };
	}

	/* ------------------------------------------------------------------ */
	/* 2. Sync shadow from device, so the regmap below only writes        */
	/*    registers that actually differ.                                  */
	/* ------------------------------------------------------------------ */
	ret = npz2100_readback(npz2100);
	if (ret != 0) {
		LOG_ERR("readback failed: %d", ret);
	}

	/* ------------------------------------------------------------------ */
	/* 3. Apply the static configuration: TOUT = 3 s, watchdog = 6 s.     */
	/* ------------------------------------------------------------------ */
	ret = npz2100_apply_regmap(npz2100, npz2100_regmap, sizeof(npz2100_regmap));
	if (ret != 0) {
		LOG_ERR("apply_regmap failed: %d", ret);
	}

	/* ------------------------------------------------------------------ */
	/* 4. Handle the wake reason.                                          */
	/* ------------------------------------------------------------------ */
	if (reason.timeout) {
		LOG_INF("Time-out wake - watchdog restarted");

		ret = demo_state_get(&state);
		if (ret != 0) {
			LOG_ERR("Reading demo state failed: %d", ret);
		} else if (state == APP_DEMO_ARMED) {
			/* Disarm first, so the hang is shown only once. */
			ret = demo_state_set(APP_DEMO_DONE);
			if (ret != 0) {
				LOG_ERR("Writing demo state failed: %d", ret);
			} else {
				simulate_hang();
			}
		}
	} else if (reason.srst_src == NPZ2100_SRST_SRC_WDOG) {
		LOG_WRN("Recovered from a watchdog reset");
	} else {
		LOG_INF("Start-up - hang on the next time-out wake");

		ret = demo_state_set(APP_DEMO_ARMED);
		if (ret != 0) {
			LOG_ERR("Writing demo state failed: %d", ret);
		}
	}

	/* ------------------------------------------------------------------ */
	/* 5. Re-enter idle.  nPZ2100 will cut nRF5x power.                   */
	/*    LOG_PANIC() flushes the deferred log buffer first so no output  */
	/*    is lost when power is cut.                                       */
	/* ------------------------------------------------------------------ */
	LOG_INF("Returning to idle - next wake in 3 s");
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
