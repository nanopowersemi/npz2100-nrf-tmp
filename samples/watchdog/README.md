# nPZ2100 watchdog sample

Shows the nPZ2100 watchdog resetting a host that has stopped responding.

- **Every 3 s:** the time-out (TOUT) wakes the nRF5x. Every boot reads
  STA1/STA2, which restarts the watchdog, so normal wakes never trigger it.
- **Start-up:** the demo is armed.
- **First time-out wake:** the application simulates a hang. It logs once a
  second and never returns to idle.
- **About 6 s later:** the watchdog resets the nPZ2100, which power-cycles the
  nRF5x. The next boot reports the watchdog reset, and the 3 s wakes carry on
  normally from then on.

## How it works

| What | Mechanism | Where |
|------|-----------|-------|
| 3-second wake | `SYSCFG2.TOUT_EXT = 0` (62.5 ms steps), `TOUT = 48` | `src/npz2100_regmap.h`, applied every boot |
| 6-second watchdog | `WDOG = 3` (2-second steps), `GTC_CFG.WDOGEN = 1` | `src/npz2100_regmap.h`, applied every boot |
| Watchdog kick | `npz2100_boot_status()` reads STA1/STA2 | `src/main.c`, every boot |
| Watchdog reset detection | `reason.srst_src == NPZ2100_SRST_SRC_WDOG` | `src/main.c` |
| Hang shown once | One byte of nPZ2100 SRAM, kept across the watchdog (soft) reset | `src/main.c` |

The watchdog period has 2-second resolution, so the reset comes 4 to 6
seconds into the hang depending on where the watchdog count was.

A boot that is neither a time-out wake nor a watchdog reset, such as after
flashing or pressing reset on the DK, re-arms the demo.

## Building

Open `samples/watchdog/` in VS Code with the nRF Connect extension, add a
build configuration for `nrf52833dk/nrf52833` or `nrf52840dk/nrf52840` and
build. The `npz2100` shield in `npz2100_module/boards/shields/npz2100/` is
selected in `CMakeLists.txt`, so no extra build arguments are needed.

## Expected output

```
<inf> app: --- nPZ2100 watchdog sample boot ---
<inf> app: Start-up - hang on the next time-out wake
<inf> app: Returning to idle - next wake in 3 s
<inf> app: --- nPZ2100 watchdog sample boot ---
<inf> app: Time-out wake - watchdog restarted
<wrn> app: Simulating a hang - waiting for the watchdog
<inf> app: Hung for 0 s
<inf> app: Hung for 1 s
...
<inf> app: --- nPZ2100 watchdog sample boot ---
<inf> npz2100:   soft_reset_src: Watchdog reset (0x02)
<wrn> app: Recovered from a watchdog reset
<inf> app: Returning to idle - next wake in 3 s
<inf> app: --- nPZ2100 watchdog sample boot ---
<inf> app: Time-out wake - watchdog restarted
<inf> app: Returning to idle - next wake in 3 s
```
