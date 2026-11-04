# nPZ2100 DK AS6212 sensor sample

Shows the nPZ2100 on the nPZ2100 DK polling the on-board AS6212
temperature sensor on its own while the nRF52833 is powered off, and
waking the nRF52833 on a temperature threshold or the periodic time-out.

- **Every boot:** the status LED turns on, the wake reason is read, and the
  register map in `src/npz2100_regmap.h` is applied, including the
  peripheral init commands it carries for the nPZ2100 SRAM.
- **Peripheral 4 wake (AS6212):** the last reading is printed in °C
  (1 LSB = 1/128 °C).
- **Time-out wake:** logged; nothing else to do.

The register map was made with the nPZ2100 configuration tool. Peripheral
4 (`ADDRP4 = 0x48`) is the AS6212, with a window of `THRUNP4 = 0x0898`
(17.2 °C) to `THROVP4 = 0x0DAC` (27.3 °C).

## Building

```
west build -b npz2100dk/nrf52833 samples/sensor_as6212/
west flash
```

The console is on the DK's USB-UART (FT231X) at 115200 baud.

### With MCUboot

The sample can also be built with MCUboot in front of it, in single-slot
mode with serial recovery over the DK's USB-UART, so the DK can be
reprogrammed without a debugger:

```
west build -b npz2100dk/nrf52833 samples/sensor_as6212/ -- -DSB_CONF_FILE=sysbuild-mcuboot.conf
west flash
```

After that, a new image can be loaded over the USB-UART with an SMP client,
for example:

```
mcumgr --conntype serial --connstring "dev=<port>,baud=115200" image upload build/sensor_as6212/zephyr/zephyr.signed.bin
```

MCUboot waits 500 ms for an SMP command on every boot
(`CONFIG_BOOT_SERIAL_WAIT_FOR_DFU_TIMEOUT` in `sysbuild/mcuboot.conf`),
because the DK has no spare button to select recovery. Every nPZ2100 wake
is a boot, so this wait costs energy on every wake; build without MCUboot
for power measurements.

Images are signed with MCUboot's public development key, which NCS uses
when no key is given (the build warns about it). Anyone can sign an image
for a board running that MCUboot. For a product, generate your own key and
set `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` in `sysbuild-mcuboot.conf`.

## Register dump

To log every nPZ2100 register (and the SRAM) before each idle, set
`CONFIG_NPZ2100_DUMP=y` in `prj.conf` and `APP_DUMP_REGISTERS` to 1 in
`src/main.c`.
