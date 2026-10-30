# nPZ2100 global counter alarm sample

Shows the nPZ2100 global time counter (GCT), its alarm (GCT_ALM) and the
periodic time-out (TOUT) working together.

- **First boot:** the counter is set to 1000 s and the alarm is armed for
  1030 s, 30 seconds later.
- **Every 5 s:** the time-out wakes the nRF5x, which prints the counter,
  the alarm time and the seconds remaining, then returns to idle.
- **Alarm wake:** the alarm wake is reported and the alarm is disabled, so
  it fires once. The 5-second wakes continue.

Change `APP_GCT_START_S` and `APP_ALARM_DELAY_S` in `src/main.c` to use other
values.

## How it works

| What | Mechanism | Where |
|------|-----------|-------|
| 5-second wake | `SYSCFG2.TOUT_EXT = 0` (62.5 ms steps), `TOUT = 80` | `src/npz2100_regmap.h`, applied every boot |
| Counter + alarm | `npz2100_timer_set()` on the shadow, then `npz2100_shadow_flush()` | `src/main.c`, first boot only |
| Status | GCT block (0x10–0x1B) from the shadow filled by `npz2100_readback()` | `src/main.c`, every boot |
| One-shot alarm | One-register regmap segment clears `GTC_CFG.GTC_AEN` | `src/main.c`, alarm wake only |

The counter registers are left out of the regmap on purpose: the regmap is
applied on every boot and would otherwise reset the counter each time.

**First-boot detection.** The application keeps no state across power
cycles. GCT_ALM and GTC_CFG are zero after an nPZ2100 power-on or system
reset, so "alarm time 0 and alarm disabled" means the counter has not been
configured yet. That check uses the `npz2100_readback()` the sample does on
every boot anyway, so it costs no extra I²C traffic. A soft reset keeps the
configuration and does not reset the counter.

The alarm is disabled with a one-register regmap segment rather than a
shadow flush, because a flush rewrites every register and would set the
running counter back to the value read at boot.

## Building

Open `samples/global_counter_alarm/` in VS Code with the nRF Connect
extension, add a build configuration for `nrf52833dk/nrf52833` or `nrf52840dk/nrf52840`
and build. The `npz2100` shield in `npz2100_module/boards/shields/npz2100/`
is selected in `CMakeLists.txt`, so no extra build arguments are needed.

## Expected output

```
<inf> app: --- nPZ2100 global counter alarm sample boot ---
<inf> npz2100: boot_status: STA1=0x00 STA2=0x00 STA3=0x00
<inf> app: First boot - counter set to 1000 s, alarm at 1030 s
<inf> app: Counter: 1000.000 s, alarm at 1030 s (in 30 s)
<inf> app: Returning to idle - next wake in 5 s
...
<inf> app: Periodic time-out wake
<inf> app: Counter: 1005.250 s, alarm at 1030 s (in 25 s)
...
<inf> app: Global counter alarm fired
<inf> app: Counter: 1030.062 s, alarm off (was 1030 s)
```

The exact fractions depend on how long each boot takes.
