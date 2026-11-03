/*
 * Copyright (c) Nanopower Semiconductor AS
 * SPDX-License-Identifier: Apache-2.0
 *
 * samples/sensor_bmp280/src/main.c
 * --------------------------------
 * BMP280 air pressure sample for the nPZ2100 driver on Nordic nRF5x
 * platforms running NCS 3.3.4.
 *
 * What it does
 * ------------
 *   - The nPZ2100 polls the BMP280 every 2 s on its own (peripheral 1):
 *     it powers the sensor, starts one forced-mode conversion with the init
 *     command in nPZ2100 SRAM, and reads the raw pressure. A 10 s time-out
 *     (TOUT) also wakes the nRF5x. Both live in npz2100_regmap.h.
 *   - Start-up (a boot not caused by a wake source): the init command is
 *     written to SRAM, and the thresholds are set to a window no real
 *     reading reaches, so the sensor never wakes the nRF5x.
 *   - First time-out wake: the thresholds are changed to a 300-1100 hPa
 *     window, the BMP280's whole operating range, so the next poll (within
 *     2 s) wakes the nRF5x, and so does every poll after it.
 *   - Every wake (time-out or sensor) prints the last pressure the nPZ2100
 *     read.
 *
 * Pressure conversion
 * -------------------
 * The nPZ2100 compares the raw reading against its thresholds, so the
 * pressure limits are turned into raw values with the sensor's own
 * calibration (dig_T1-T3, dig_P1-P9). Pressure compensation also needs the
 * temperature (t_fine), which the nPZ2100 does not read, so on every wake
 * the nRF5x runs one temperature conversion itself. It reaches the sensor
 * directly over I²C, on the bus it shares with the nPZ2100; the nPZ2100
 * keeps the sensor switch on while the nRF5x is awake (IOCFG1).
 *
 * The nPZ2100 reads press_msb/press_lsb (0xF7/0xF8) as one 16-bit value,
 * the top 16 of the 20 bits of adc_P; press_xlsb is not read. A higher raw
 * value means a lower pressure.
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

/** Peripheral slot the BMP280 is configured in (peripheral 1). */
#define APP_SLOT             0u

/** Pressure window that wakes the nRF5x after the first time-out, hPa. */
#define APP_WAKE_MIN_HPA     300
#define APP_WAKE_MAX_HPA     1100

/** Start-up thresholds: a window no real reading reaches (raw 0xFFFE-0xFFFF). */
#define APP_THR_OFF_UNDER    0xFFFEu
#define APP_THR_OFF_OVER     0xFFFFu

/* BMP280 registers. */
#define BMP280_REG_CALIB     0x88u   /**< dig_T1..dig_P9, 12 x 16-bit LE. */
#define BMP280_REG_ID        0xD0u
#define BMP280_REG_STATUS    0xF3u
#define BMP280_REG_CTRL_MEAS 0xF4u
#define BMP280_REG_TEMP      0xFAu   /**< temp_msb, temp_lsb, temp_xlsb. */
#define BMP280_CHIP_ID       0x58u
#define BMP280_STATUS_MEASURING  0x08u

/** ctrl_meas: osrs_t = x1, osrs_p = x1, mode = forced (nPZ2100 polls). */
#define BMP280_CTRL_MEAS_FORCED_TP  0x25u
/** ctrl_meas: osrs_t = x1, osrs_p = skipped, mode = forced (nRF5x). */
#define BMP280_CTRL_MEAS_FORCED_T   0x21u

/* -------------------------------------------------------------------------
 * Device handles. The nPZ2100 node comes from the npz2100 shield
 * (npz2100_module/boards/shields/npz2100/); the BMP280 is on the same bus.
 * ---------------------------------------------------------------------- */
static const struct device *const npz2100 =
	DEVICE_DT_GET(DT_NODELABEL(npz2100));

static const struct i2c_dt_spec bmp280 = {
	.bus  = DEVICE_DT_GET(DT_BUS(DT_NODELABEL(npz2100))),
	.addr = BMP280_I2C_ADDR,
};

/* -------------------------------------------------------------------------
 * Init command the nPZ2100 sends to the sensor before every read, from
 * SRAM: (register, value) pairs. The sensor is powered off between polls,
 * so each poll starts one forced-mode conversion.
 * ---------------------------------------------------------------------- */
static const uint8_t sensor_init_cmds[] = {
	BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS_FORCED_TP,
};

/** Calibration, plus t_fine from the temperature measured on this wake. */
struct bmp280_calib {
	uint16_t t1;
	int16_t  t2, t3;
	uint16_t p1;
	int16_t  p2, p3, p4, p5, p6, p7, p8, p9;
	int32_t  t_fine;
};

/* -------------------------------------------------------------------------
 * Compensation, from the Bosch datasheet: BMP280_compensate_T_int32() and
 * BMP280_compensate_P_int64(). The temperature part runs in 64-bit so it
 * cannot overflow for any raw value.
 * ---------------------------------------------------------------------- */
static int32_t bmp280_t_fine(const struct bmp280_calib *cal, int32_t adc_t)
{
	int64_t d = ((int64_t)adc_t >> 4) - cal->t1;
	int64_t var1 = ((((int64_t)adc_t >> 3) - ((int64_t)cal->t1 << 1)) * cal->t2) >> 11;
	int64_t var2 = (((d * d) >> 12) * cal->t3) >> 14;

	return (int32_t)(var1 + var2);
}

/** Pressure in Pa, Q24.8 (value / 256 = Pa). raw is adc_P >> 4. */
static uint32_t bmp280_press_q8(const struct bmp280_calib *cal, uint16_t raw)
{
	int64_t adc_p = (int64_t)raw << 4;
	int64_t var1, var2, p;

	var1 = (int64_t)cal->t_fine - 128000;
	var2 = var1 * var1 * cal->p6;
	var2 = var2 + ((var1 * cal->p5) << 17);
	var2 = var2 + ((int64_t)cal->p4 << 35);
	var1 = ((var1 * var1 * cal->p3) >> 8) + ((var1 * cal->p2) << 12);
	var1 = ((((int64_t)1) << 47) + var1) * cal->p1 >> 33;
	if (var1 == 0) {
		return 0u; /* avoid division by zero */
	}
	p = 1048576 - adc_p;
	p = (((p << 31) - var2) * 3125) / var1;
	var1 = ((int64_t)cal->p9 * (p >> 13) * (p >> 13)) >> 25;
	var2 = ((int64_t)cal->p8 * p) >> 19;
	p = ((p + var1 + var2) >> 8) + ((int64_t)cal->p7 << 4);

	return (uint32_t)p;
}

/* -------------------------------------------------------------------------
 * bmp280_raw_for_press
 *
 * Smallest raw value that reads as at most hpa. The pressure falls as the
 * raw value rises over the whole 16-bit range, so a binary search finds it.
 * ---------------------------------------------------------------------- */
static uint16_t bmp280_raw_for_press(const struct bmp280_calib *cal, uint32_t hpa)
{
	const uint32_t target = hpa * 100u * 256u;
	uint32_t lo = 0u;
	uint32_t hi = 0xFFFFu;

	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2u;

		if (bmp280_press_q8(cal, (uint16_t)mid) > target) {
			lo = mid + 1u;
		} else {
			hi = mid;
		}
	}

	return (uint16_t)lo;
}

/* -------------------------------------------------------------------------
 * bmp280_prepare
 *
 * Checks the chip ID, reads the calibration and runs one forced
 * temperature conversion to get t_fine for the pressure compensation.
 * ---------------------------------------------------------------------- */
static int bmp280_prepare(struct bmp280_calib *cal)
{
	uint8_t buf[24];
	uint8_t val;
	int ret;

	ret = i2c_reg_read_byte_dt(&bmp280, BMP280_REG_ID, &val);
	if (ret != 0) {
		LOG_ERR("BMP280 read failed: %d", ret);
		return ret;
	}
	if (val != BMP280_CHIP_ID) {
		LOG_ERR("BMP280 chip ID 0x%02X, expected 0x%02X", val, BMP280_CHIP_ID);
		return -ENODEV;
	}

	ret = i2c_burst_read_dt(&bmp280, BMP280_REG_CALIB, buf, sizeof(buf));
	if (ret != 0) {
		LOG_ERR("BMP280 calibration read failed: %d", ret);
		return ret;
	}

	cal->t1 = sys_get_le16(&buf[0]);
	cal->t2 = (int16_t)sys_get_le16(&buf[2]);
	cal->t3 = (int16_t)sys_get_le16(&buf[4]);
	cal->p1 = sys_get_le16(&buf[6]);
	cal->p2 = (int16_t)sys_get_le16(&buf[8]);
	cal->p3 = (int16_t)sys_get_le16(&buf[10]);
	cal->p4 = (int16_t)sys_get_le16(&buf[12]);
	cal->p5 = (int16_t)sys_get_le16(&buf[14]);
	cal->p6 = (int16_t)sys_get_le16(&buf[16]);
	cal->p7 = (int16_t)sys_get_le16(&buf[18]);
	cal->p8 = (int16_t)sys_get_le16(&buf[20]);
	cal->p9 = (int16_t)sys_get_le16(&buf[22]);

	/* One forced temperature conversion: 6.4 ms max, wait for it. */
	ret = i2c_reg_write_byte_dt(&bmp280, BMP280_REG_CTRL_MEAS,
				    BMP280_CTRL_MEAS_FORCED_T);
	if (ret != 0) {
		LOG_ERR("BMP280 conversion start failed: %d", ret);
		return ret;
	}

	for (int i = 0; i < 10; i++) {
		k_msleep(5);
		ret = i2c_reg_read_byte_dt(&bmp280, BMP280_REG_STATUS, &val);
		if (ret != 0 || !(val & BMP280_STATUS_MEASURING)) {
			break;
		}
	}

	ret = i2c_burst_read_dt(&bmp280, BMP280_REG_TEMP, buf, 3u);
	if (ret != 0) {
		LOG_ERR("BMP280 temperature read failed: %d", ret);
		return ret;
	}

	int32_t adc_t = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | (buf[2] >> 4);

	cal->t_fine = bmp280_t_fine(cal, adc_t);
	return 0;
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
 * pressure_print
 *
 * Reads the last value the nPZ2100 polled from the BMP280 and logs it,
 * with the temperature the nRF5x measured on this wake.
 * ---------------------------------------------------------------------- */
static void pressure_print(const struct bmp280_calib *cal)
{
	uint16_t raw;
	int ret;

	ret = npz2100_periph_read_value(npz2100, APP_SLOT, &raw);
	if (ret != 0) {
		LOG_ERR("Reading peripheral 1 failed: %d", ret);
		return;
	}

	/* Q24.8 Pa to hundredths of a hPa (= Pa): round to the nearest Pa. */
	uint32_t pa = (bmp280_press_q8(cal, raw) + 128u) / 256u;
	int32_t t = (cal->t_fine * 5 + 128) >> 8;   /* 0.01 °C */
	int32_t mag = (t < 0) ? -t : t;

	LOG_INF_HL(SLC_GREEN, "Pressure: %u.%02u hPa (raw 0x%04X) at %s%d.%02d C",
		pa / 100u, pa % 100u, raw,
		(t < 0) ? "-" : "", mag / 100, mag % 100);
}

/* -------------------------------------------------------------------------
 * thresholds_arm
 *
 * Moves the window from out of range to APP_WAKE_MIN_HPA-APP_WAKE_MAX_HPA,
 * unless that was already done on an earlier time-out wake. Higher
 * pressure is a lower raw value, so the maximum pressure gives the under
 * threshold.
 * ---------------------------------------------------------------------- */
static void thresholds_arm(const struct bmp280_calib *cal)
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

	under = bmp280_raw_for_press(cal, APP_WAKE_MAX_HPA);
	over  = bmp280_raw_for_press(cal, APP_WAKE_MIN_HPA);

	ret = thresholds_set(under, over);
	if (ret != 0) {
		LOG_ERR("Setting thresholds failed: %d", ret);
		return;
	}

	LOG_INF_HL(SLC_MAGENTA, "Thresholds set to %d-%d hPa window (raw 0x%04X-0x%04X)",
		APP_WAKE_MIN_HPA, APP_WAKE_MAX_HPA, under, over);
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
	struct bmp280_calib cal;

	LOG_INF_HL(SLC_CYAN, "--- nPZ2100 BMP280 sample boot ---");

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
	if (!start_up && bmp280_prepare(&cal) == 0) {
		if (reason.timeout) {
			LOG_INF_HL(SLC_BLUE, "Global time-out wake");
		}
		if (sensor_wake) {
			LOG_INF_BANNER("Peripheral 1 wake - pressure in window");
		}

		pressure_print(&cal);

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
