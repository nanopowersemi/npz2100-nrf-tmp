#ifndef __NPZ2100_REGMAP
#define __NPZ2100_REGMAP

// nPZ2100 Register Map — BMP280 sensor sample
//
// Global: wake the host on peripheral 1 or after a 10 s time-out.
//   TOUT_EXT = 0 selects 62.5 ms time-out steps, TOUT = 160 -> 10 s.
//
// Peripheral 1: BMP280 on I2C address BMP280_I2C_ADDR, powered from
// SW_LP<BMP280_SW_LP> (bench wiring, set below). Every 2 s the nPZ2100 powers the
// sensor, sends the SRAM init command (ctrl_meas = forced mode, pressure and
// temperature x1), waits for the conversion and reads press_msb/press_lsb
// (0xF7/0xF8).
//
// The thresholds (THROVP/THRUNP, 0x28-0x2B) are left out on purpose: main.c
// sets them at runtime, and the regmap is applied on every boot, so it
// would otherwise put them back each time.

/** BMP280 7-bit I2C address: 0x76 with SDO to GND, 0x77 with SDO to VDDIO. */
#define BMP280_I2C_ADDR  0x76

/** nPZ2100 peripheral power switch feeding the BMP280: 1-4 for SW_LP1-4. */
#define BMP280_SW_LP     1

static const unsigned char npz2100_regmap[] = {
  // ---- Global configuration registers ----
  10,         // Length
  0x05,       // Start address (IOCFG1 register)
  (1u << (BMP280_SW_LP - 1)), // IOCFG1: PSWA_Sn = 1, the sensor switch is
        // on while the host is awake, so the nRF can read the calibration
  0x00, // IOCFG2 = 0x00
  0xFF, // IOCFG3 = 0xFF
  0x00, // IOCFG4 = 0x00
  0x1D, // IOCFG5 = 0x1D
  0x01, // SYSCFG1 = 0x01 (WUP_P1: peripheral 1 wakes the host, WUPMOD = any)
  0x00, // SYSCFG2 = 0x00 (TOUT_EXT = 0: 62.5 ms time-out steps)
  0xA0, 0x00, // (TOUT_L/TOUT_H) = 0xA0 = 160 -> 10 s

  // ---- Peripheral 1, up to the thresholds ----
  10,                 // Length
  0x1F,               // Start address (P_BANK register)
  0x00,               // P_BANK = 0 (peripheral 1)
  0x01,               // CFGP = 0x01 (PWMOD = periodic, TMOD = init/read/compare, no log)
  (BMP280_SW_LP - 1), // IOP: SELPSW = the sensor switch
  0x29, // MODP = 0x29 (CMOD = 1: trigger inside the window, DTYPE = uint16,
        //              SEQRW = 1, SWPRREG = 1: 0xF7 is the high byte)
  0x20, 0x00, // (PERP_L/PERP_H) = 32 -> 32 x 62.5 ms = 2 s polling period
  0x01, // NCMDP = 1 (one init command in SRAM)
  BMP280_I2C_ADDR, // ADDRP = BMP280 I2C address
  0xF7, // RREGP = 0xF7 (press_msb, then press_lsb)

  // ---- Peripheral 1, timing ----
  3,          // Length
  0x2C,       // Start address (TWTP register)
  0x0C, // TWTP = 12 -> 12 x 256 / 400 kHz = 7.7 ms (conversion: 6.4 ms max)
  0x05, // TCFGP = 0x05 (TINIT_EN: start-up wait after power-on,
        //               TWT_EN: conversion wait after the init command)
};

#endif // __NPZ2100_REGMAP
