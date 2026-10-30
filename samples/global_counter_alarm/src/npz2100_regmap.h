#ifndef __NPZ2100_REGMAP
#define __NPZ2100_REGMAP

// nPZ2100 Register Map — global counter alarm sample
//
// Wake the host every 5 seconds: TOUT_EXT = 0 selects 62.5 ms time-out
// steps, and TOUT = 80 gives 80 x 62.5 ms = 5 s.
//
// GCT, GCT_ALM and GTC_CFG are set at runtime by main.c on first boot and
// are deliberately left out of the regmap so they are not reverted on every
// boot.

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
  0x50, 0x00, // (TOUT_L/TOUT_H) = 0x50 = 80 -> 5 s
};

#endif // __NPZ2100_REGMAP
