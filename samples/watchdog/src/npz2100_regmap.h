#ifndef __NPZ2100_REGMAP
#define __NPZ2100_REGMAP

// nPZ2100 Register Map — watchdog sample
//
// Wake the host every 3 seconds: TOUT_EXT = 0 selects 62.5 ms time-out
// steps, and TOUT = 48 gives 48 x 62.5 ms = 3 s.
//
// Watchdog: WDOG = 3 gives 3 x 2 s = 6 s, enabled by GTC_CFG.WDOGEN.
// Every boot reads STA1/STA2 in npz2100_boot_status(), which restarts the
// watchdog, so it only fires when the host stops booting normally.

static const unsigned char npz2100_regmap[] = {
  // ---- Global configuration registers ----
  10,         // Length
  0x05,       // Start address (IOCFG1 register)
  0x00, // IOCFG1 = 0x00
  0x00, // IOCFG2 = 0x00
  0xFF, // IOCFG3 = 0xFF
  0x00, // IOCFG4 = 0x00
  0x1D, // IOCFG5 = 0x1D
  0x00, // SYSCFG1 = 0x00 (no peripheral wake-up sources)
  0x00, // SYSCFG2 = 0x00 (TOUT_EXT = 0: 62.5ms time-out steps)
  0x30, 0x00, // (TOUT_L/TOUT_H) = 0x30 = 48 -> 3 s

  // ---- Watchdog ----
  4,          // Length
  0x19,       // Start address (WDOG_L register)
  0x03, 0x00, // (WDOG_L/WDOG_H) = 3 -> 6 s
  0x02, // GTC_CFG = 0x02 (WDOGEN = 1, alarm off)
};

#endif // __NPZ2100_REGMAP
