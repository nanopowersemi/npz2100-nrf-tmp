/*
 * Copyright (c) Nanopower Semiconductor AS
 * SPDX-License-Identifier: Apache-2.0
 *
 * samples/sensor_as6212/src/main.c
 * --------------------------------
 * AS6212 temperature sample for the nPZ2100 DK (npz2100dk/nrf52833) on
 * NCS 3.3.4.
 *
 * What it does
 * ------------
 *   - The nPZ2100 polls the AS6212 on the DK on its own (the peripheral
 *     configured in npz2100_regmap.h) and wakes the nRF52833 when the
 *     reading crosses its thresholds, or on the periodic time-out.
 *   - The regmap also carries the peripheral init commands for the nPZ2100
 *     SRAM; npz2100_apply_regmap() writes them on every boot.
 *   - Every peripheral wake prints the last value the nPZ2100 read; for the
 *     AS6212 slot it is converted to degrees Celsius.
 *   - The status LED is on while the nRF52833 is awake.
 *
 * Power architecture
 * ------------------
 * The nPZ2100 controls the nRF52833 power supply via SW_HP. When idle the
 * nPZ2100 cuts power to the nRF52833 completely, so every boot of this
 * application is one wake cycle. The nPZ2100 keeps its registers and SRAM
 * while the nRF52833 is off.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include <drivers/npz2100.h>
#include "npz2100_regmap.h"
#include "sample_log_color.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/** Peripheral slot the AS6212 is configured in (peripheral 4: ADDRP4 =
 *  0x48, the AS6212 address on the DK, in npz2100_regmap.h). */
#define AS6212_SLOT          3u

/** Set to 1, with CONFIG_NPZ2100_DUMP=y, to log every nPZ2100 register
 *  before going idle. With the SRAM included it adds about 60 ms of I²C
 *  traffic to every wake. */
#define APP_DUMP_REGISTERS   0

static const struct device *const npz2100 =
	DEVICE_DT_GET(DT_NODELABEL(npz2100));

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* -------------------------------------------------------------------------
 * AS6212 TVAL: 16-bit two's complement, 1 LSB = 1/128 °C. MODP.SWPRREG has
 * already put the two bytes in the right order, so the value is the
 * register exactly as the sensor reports it.
 * ---------------------------------------------------------------------- */
static void as6212_print(uint16_t raw)
{
	/* mdeg = raw * 1000 / 128 = raw * 125 / 16, exact without float. */
	int32_t mdeg = ((int32_t)(int16_t)raw * 125) / 16;
	int32_t a = (mdeg < 0) ? -mdeg : mdeg;

	LOG_INF_HL(SLC_GREEN, "Temperature: %s%d.%03d C (raw 0x%04X)",
		   (mdeg < 0) ? "-" : "", (int)(a / 1000), (int)(a % 1000), raw);
}

static void handle_peripheral_wake(uint8_t slot)
{
	uint16_t raw;
	int ret = npz2100_periph_read_value(npz2100, slot, &raw);

	if (ret != 0) {
		LOG_ERR("Reading peripheral %u failed: %d", slot + 1u, ret);
		return;
	}

	if (slot == AS6212_SLOT) {
		LOG_INF_BANNER("Peripheral 4 wake - AS6212");
		as6212_print(raw);
	} else {
		LOG_INF("Peripheral %u wake: raw 0x%04X", slot + 1u, raw);
	}
}

/* -------------------------------------------------------------------------
 * main
 *
 * Runs once per nRF52833 boot (= once per nPZ2100 wake cycle). Does not
 * contain a loop: npz2100_enter_idle() makes the nPZ2100 cut the power.
 * ---------------------------------------------------------------------- */
int main(void)
{
	int ret;
	npz2100_wake_reason_t reason;

	if (gpio_is_ready_dt(&led)) {
		(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	}

	LOG_INF_HL(SLC_CYAN, "--- nPZ2100 AS6212 sample boot ---");

	if (!device_is_ready(npz2100)) {
		/* Without the nPZ2100 the nRF52833 cannot go idle; wait for
		 * the nPZ2100 watchdog to power-cycle it. */
		LOG_ERR("nPZ2100 device not ready - waiting for watchdog");
		while (true) {
			k_msleep(1000);
		}
	}

	/* ------------------------------------------------------------------ */
	/* 1. Read wake-up reason.  MUST be the first nPZ2100 operation.      */
	/*    Also kicks the nPZ2100 watchdog (reading STA1/STA2).            */
	/* ------------------------------------------------------------------ */
	ret = npz2100_boot_status(npz2100, &reason);
	if (ret != 0) {
		LOG_ERR("boot_status failed: %d - cannot determine wake reason", ret);
		reason = (npz2100_wake_reason_t){ 0 };
	}

	/* ------------------------------------------------------------------ */
	/* 2. Sync shadow from device (no I²C unless CONFIG_NPZ2100_SHADOW).  */
	/* ------------------------------------------------------------------ */
	ret = npz2100_readback(npz2100);
	if (ret != 0) {
		LOG_ERR("readback failed: %d", ret);
	}

	/* ------------------------------------------------------------------ */
	/* 3. Apply the configuration, including the SRAM init commands at    */
	/*    the end of the regmap.  Do not also write the same SRAM bytes   */
	/*    with npz2100_sram_write().                                      */
	/* ------------------------------------------------------------------ */
	ret = npz2100_apply_regmap(npz2100, npz2100_regmap, sizeof(npz2100_regmap));
	if (ret != 0) {
		LOG_ERR("apply_regmap failed: %d", ret);
	}

	/* ------------------------------------------------------------------ */
	/* 4. Handle the wake reason.                                          */
	/* ------------------------------------------------------------------ */
	for (uint8_t slot = 0u; slot < ARRAY_SIZE(reason.periph); slot++) {
		if (reason.periph[slot]) {
			handle_peripheral_wake(slot);
		}
	}
	if (reason.timeout) {
		LOG_INF_HL(SLC_BLUE, "Global time-out wake");
	}
	if (reason.alarm) {
		LOG_INF_HL(SLC_BLUE, "Global time counter alarm wake");
	}

#if APP_DUMP_REGISTERS && defined(CONFIG_NPZ2100_DUMP)
	(void)npz2100_dump_registers(npz2100, NPZ2100_DUMP_SRAM);
#endif

	/* ------------------------------------------------------------------ */
	/* 5. Re-enter idle.  The nPZ2100 cuts nRF52833 power.                */
	/*    LOG_PANIC() flushes the deferred log buffer first so no output  */
	/*    is lost when power is cut.                                       */
	/* ------------------------------------------------------------------ */
	LOG_INF_HL(SLC_GRAY, "Returning to idle");
	LOG_PANIC();

	ret = npz2100_enter_idle(npz2100);

	/* Reaching here means the I²C write to IDLE_RST failed. The nPZ2100
	 * watchdog power-cycles the nRF52833 after its time-out. */
	LOG_ERR("enter_idle failed: %d - waiting for watchdog", ret);
	while (true) {
		k_msleep(1000);
	}

	return 0; /* unreachable */
}
