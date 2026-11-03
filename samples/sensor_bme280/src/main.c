/*
 * Copyright (c) Nanopower Semiconductor AS
 * SPDX-License-Identifier: Apache-2.0
 *
 * samples/sensor_bme280/src/main.c
 * --------------------------------
 * BME280 temperature sample for the nPZ2100 driver on Nordic nRF5x
 * platforms running NCS 3.3.4.
 *
 * What it does
 * ------------
 *   - The nPZ2100 polls the BME280 every 2 s on its own (peripheral 1):
 *     it powers the sensor, starts one forced-mode temperature conversion
 *     with the init command in nPZ2100 SRAM, and reads the raw result.
 *     A 10 s time-out (TOUT) also wakes the nRF5x. Both live in
 *     npz2100_regmap.h.
 *   - Start-up (a boot not caused by a wake source): the init command is
 *     written to SRAM, and the thresholds are set to a window far above any
 *     real temperature, so the sensor never wakes the nRF5x.
 *   - First time-out wake: the thresholds are changed to a 0-40 °C window,
 *     so the next poll (within 2 s) wakes the nRF5x, and so does every poll
 *     after it while the temperature stays in that window.
 *   - Every wake (time-out or sensor) prints the last temperature the
 *     nPZ2100 read.
 *
 * Temperature conversion
 * ----------------------
 * The nPZ2100 compares the raw reading against its thresholds, so the
 * 0 °C and 40 °C limits are turned into raw values with the sensor's own
 * calibration (dig_T1-dig_T3, datasheet section 4.2.2). The nRF5x reads the
 * calibration directly over I²C, on the bus it shares with the nPZ2100; the
 * nPZ2100 keeps the sensor switch on while the nRF5x is awake (IOCFG1).
 *
 * The nPZ2100 reads temp_msb/temp_lsb (0xFA/0xFB) as one 16-bit value, the
 * top 16 of the 20 bits of adc_T; temp_xlsb is not read.
 *
 * Power architecture
 * ------------------
 * The nPZ2100 controls the nRF5x power supply via SW_HP. When idle the
 * nPZ2100 cuts power to the nRF5x completely, so every boot of this
 * application is one wake cycle. The nPZ2100 keeps its registers and SRAM
 * while the nRF5x is off.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/byteorder.h>

#include <drivers/npz2100.h>
#include "npz2100_regmap.h"
#include "sample_log_color.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/** Peripheral slot the BME280 is configured in (peripheral 1). */
#define APP_SLOT             0u

/** Temperature window that wakes the nRF5x after the first time-out, °C. */
#define APP_WAKE_MIN_C       0
#define APP_WAKE_MAX_C       40

/** Start-up thresholds: a window no real reading reaches (raw 0xFFFE-0xFFFF). */
#define APP_THR_OFF_UNDER    0xFFFEu
#define APP_THR_OFF_OVER     0xFFFFu

/* BME280 registers (datasheet section 5.3). */
#define BME280_REG_CALIB_T   0x88u   /**< dig_T1 (u16), dig_T2, dig_T3 (s16), LE. */
#define BME280_REG_ID        0xD0u
#define BME280_REG_CTRL_MEAS 0xF4u
#define BME280_CHIP_ID       0x60u

/** ctrl_meas: osrs_t = x1, osrs_p = skipped, mode = forced. */
#define BME280_CTRL_MEAS_FORCED_T1  0x21u

/* -------------------------------------------------------------------------
 * Device handles. The nPZ2100 node comes from the npz2100 shield
 * (npz2100_module/boards/shields/npz2100/); the BME280 is on the same bus.
 * ---------------------------------------------------------------------- */
static const struct device *const npz2100 =
	DEVICE_DT_GET(DT_NODELABEL(npz2100));

static const struct i2c_dt_spec bme280 = {
	.bus  = DEVICE_DT_GET(DT_BUS(DT_NODELABEL(npz2100))),
	.addr = BME280_I2C_ADDR,
};

/* -------------------------------------------------------------------------
 * Init command the nPZ2100 sends to the sensor before every read, from
 * SRAM: (register, value) pairs. The sensor is powered off between polls,
 * so each poll starts one forced-mode conversion.
 * ---------------------------------------------------------------------- */
static const uint8_t sensor_init_cmds[] = {
	BME280_REG_CTRL_MEAS, BME280_CTRL_MEAS_FORCED_T1,
};

/** Temperature calibration (datasheet table 16). */
struct bme280_calib {
	uint16_t t1;
	int16_t  t2;
	int16_t  t3;
};

static int bme280_calib_read(struct bme280_calib *cal)
{
	uint8_t id;
	uint8_t buf[6];
	int ret;

	ret = i2c_reg_read_byte_dt(&bme280, BME280_REG_ID, &id);
	if (ret != 0) {
		LOG_ERR("BME280 read failed: %d", ret);
		return ret;
	}

	if (id != BME280_CHIP_ID) {
		LOG_ERR("BME280 chip ID 0x%02X, expected 0x%02X", id, BME280_CHIP_ID);
		return -ENODEV;
	}

	ret = i2c_burst_read_dt(&bme280, BME280_REG_CALIB_T, buf, sizeof(buf));
	if (ret != 0) {
		LOG_ERR("BME280 calibration read failed: %d", ret);
		return ret;
	}

	cal->t1 = sys_get_le16(&buf[0]);
	cal->t2 = (int16_t)sys_get_le16(&buf[2]);
	cal->t3 = (int16_t)sys_get_le16(&buf[4]);

	return 0;
}

/* -------------------------------------------------------------------------
 * bme280_temp_centi
 *
 * Datasheet section 4.2.3, BME280_compensate_T_int32(), in 64-bit so it
 * cannot overflow for any raw value. raw is the 16-bit value the nPZ2100
 * reads (adc_T >> 4). Returns hundredths of a degree Celsius.
 * ---------------------------------------------------------------------- */
static int32_t bme280_temp_centi(const struct bme280_calib *cal, uint16_t raw)
{
	int64_t adc_t = (int64_t)raw << 4;
	int64_t d = (adc_t >> 4) - cal->t1;
	int64_t var1 = (((adc_t >> 3) - ((int64_t)cal->t1 << 1)) * cal->t2) >> 11;
	int64_t var2 = (((d * d) >> 12) * cal->t3) >> 14;

	return (int32_t)(((var1 + var2) * 5 + 128) >> 8);
}

/* -------------------------------------------------------------------------
 * bme280_raw_for_temp
 *
 * Smallest raw value that reads as at least centi_c. The temperature rises
 * with the raw value over the whole 16-bit range for real calibration
 * values, so a binary search finds it.
 * ---------------------------------------------------------------------- */
static uint16_t bme280_raw_for_temp(const struct bme280_calib *cal, int32_t centi_c)
{
	uint32_t lo = 0u;
	uint32_t hi = 0xFFFFu;

	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2u;

		if (bme280_temp_centi(cal, (uint16_t)mid) < centi_c) {
			lo = mid + 1u;
		} else {
			hi = mid;
		}
	}

	return (uint16_t)lo;
}

/* -------------------------------------------------------------------------
 * Thresholds of peripheral 1 (THROVP 0x28-0x29, THRUNP 0x2A-0x2B).
 * Selecting the bank and accessing the four registers explicitly keeps
 * them out of the regmap, which is applied on every boot.
 * ---------------------------------------------------------------------- */
static int thresholds_get(uint16_t *under, uint16_t *over)
{
	const uint8_t bank = APP_SLOT;
	uint8_t buf[4];
	int ret;

	ret = npz2100_write_regs(npz2100, NPZ2100_REG_P_BANK, &bank, 1u);
	if (ret == 0) {
		ret = npz2100_read_regs(npz2100, NPZ2100_REG_THROVP_L, buf, sizeof(buf));
	}
	if (ret != 0) {
		return ret;
	}

	*over  = sys_get_le16(&buf[0]);
	*under = sys_get_le16(&buf[2]);
	return 0;
}

static int thresholds_set(uint16_t under, uint16_t over)
{
	const uint8_t bank = APP_SLOT;
	uint8_t buf[4];
	int ret;

	sys_put_le16(over, &buf[0]);
	sys_put_le16(under, &buf[2]);

	ret = npz2100_write_regs(npz2100, NPZ2100_REG_P_BANK, &bank, 1u);
	if (ret == 0) {
		ret = npz2100_write_regs(npz2100, NPZ2100_REG_THROVP_L, buf, sizeof(buf));
	}
	return ret;
}

/* -------------------------------------------------------------------------
 * temperature_print
 *
 * Reads the last value the nPZ2100 polled from the BME280 and logs it.
 * ---------------------------------------------------------------------- */
static void temperature_print(const struct bme280_calib *cal)
{
	uint16_t raw;
	int ret;

	ret = npz2100_periph_read_value(npz2100, APP_SLOT, &raw);
	if (ret != 0) {
		LOG_ERR("Reading peripheral 1 failed: %d", ret);
		return;
	}

	int32_t t = bme280_temp_centi(cal, raw);
	int32_t mag = (t < 0) ? -t : t;

	LOG_INF_HL(SLC_GREEN, "Temperature: %s%d.%02d C (raw 0x%04X)",
		(t < 0) ? "-" : "", mag / 100, mag % 100, raw);
}

/* -------------------------------------------------------------------------
 * thresholds_arm
 *
 * Moves the window from out of range to APP_WAKE_MIN_C-APP_WAKE_MAX_C,
 * unless that was already done on an earlier time-out wake.
 * ---------------------------------------------------------------------- */
static void thresholds_arm(const struct bme280_calib *cal)
{
	uint16_t under, over;
	int ret;

	ret = thresholds_get(&under, &over);
	if (ret != 0) {
		LOG_ERR("Reading thresholds failed: %d", ret);
		return;
	}

	if (under != APP_THR_OFF_UNDER || over != APP_THR_OFF_OVER) {
		return; /* Already armed. */
	}

	under = bme280_raw_for_temp(cal, APP_WAKE_MIN_C * 100);
	over  = bme280_raw_for_temp(cal, APP_WAKE_MAX_C * 100);

	ret = thresholds_set(under, over);
	if (ret != 0) {
		LOG_ERR("Setting thresholds failed: %d", ret);
		return;
	}

	LOG_INF_HL(SLC_MAGENTA, "Thresholds set to %d-%d C window (raw 0x%04X-0x%04X)",
		APP_WAKE_MIN_C, APP_WAKE_MAX_C, under, over);
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
	struct bme280_calib cal;

	LOG_INF_HL(SLC_CYAN, "--- nPZ2100 BME280 sample boot ---");

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

	bool sensor_wake = reason.periph[APP_SLOT];
	bool start_up = !reason.timeout && !sensor_wake;

	/* ------------------------------------------------------------------ */
	/* 2. Sync shadow from device (no I²C unless CONFIG_NPZ2100_SHADOW).  */
	/* ------------------------------------------------------------------ */
	ret = npz2100_readback(npz2100);
	if (ret != 0) {
		LOG_ERR("readback failed: %d", ret);
	}

	/* ------------------------------------------------------------------ */
	/* 3. Start-up: init command and out-of-range thresholds, written     */
	/*    before the regmap enables the peripheral.                       */
	/* ------------------------------------------------------------------ */
	if (start_up) {
		ret = npz2100_sram_write(npz2100, 0x00u, sensor_init_cmds,
					 sizeof(sensor_init_cmds));
		if (ret != 0) {
			LOG_ERR("SRAM write failed: %d", ret);
		}

		ret = thresholds_set(APP_THR_OFF_UNDER, APP_THR_OFF_OVER);
		if (ret != 0) {
			LOG_ERR("Setting thresholds failed: %d", ret);
		}

		LOG_INF_HL(SLC_MAGENTA,
			   "Start-up - thresholds set out of range, sensor will not wake");
	}

	/* ------------------------------------------------------------------ */
	/* 4. Apply the static configuration: peripheral 1, TOUT = 10 s.      */
	/* ------------------------------------------------------------------ */
	ret = npz2100_apply_regmap(npz2100, npz2100_regmap, sizeof(npz2100_regmap));
	if (ret != 0) {
		LOG_ERR("apply_regmap failed: %d", ret);
	}

	/* ------------------------------------------------------------------ */
	/* 5. Handle the wake reason.                                          */
	/* ------------------------------------------------------------------ */
	if (!start_up && bme280_calib_read(&cal) == 0) {
		if (reason.timeout) {
			LOG_INF_HL(SLC_BLUE, "Global time-out wake");
		}
		if (sensor_wake) {
			LOG_INF_BANNER("Peripheral 1 wake - temperature in window");
		}

		temperature_print(&cal);

		if (reason.timeout) {
			thresholds_arm(&cal);
		}
	}

	/* ------------------------------------------------------------------ */
	/* 6. Re-enter idle.  nPZ2100 will cut nRF5x power.                   */
	/*    LOG_PANIC() flushes the deferred log buffer first so no output  */
	/*    is lost when power is cut.                                       */
	/* ------------------------------------------------------------------ */
	LOG_INF_HL(SLC_GRAY, "Returning to idle");
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
