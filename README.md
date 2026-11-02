# nPZ2100 - nRF Connect SDK Driver Package

**Target:** nRF Host Chip · **SDK:** NCS 3.0.2 · **IDE:** VS Code + nRF Connect extension

---

## What is in this package

```
npz2100_ncs/
├── README.md                    ← This file
├── npz2100_module/              ← Zephyr driver module (add once, reference forever)
│   ├── zephyr/module.yml        ← West module declaration
│   ├── CMakeLists.txt
│   ├── Kconfig
│   ├── drivers/npz2100/
│   │   ├── npz2100.c            ← Platform-agnostic HAL primitives
│   │   ├── npz2100_lib.c        ← Lib API (regmap, shadow, typed helpers)
│   │   ├── npz2100_datalog.c    ← SRAM data-log extraction and parsing
│   │   ├── npz2100_zephyr.c     ← Zephyr device-model wrapper
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   ├── dts/bindings/
│   │   ├── vendor-prefixes.txt  ← Registers the "nanopower" DT vendor prefix
│   │   └── misc/
│   │       └── nanopower,npz2100.yaml
│   ├── boards/shields/npz2100/  ← nPZero shield (selected in each sample's CMakeLists.txt)
│   │   ├── npz2100.overlay      ← nPZ2100 node on the Arduino I²C bus
│   │   ├── boards/<board>.overlay ← Board-specific additions (nRF52833/52840 DK)
│   │   ├── Kconfig.shield
│   │   ├── Kconfig.defconfig
│   │   ├── shield.yml
│   │   └── README.md
│   └── include/
│       ├── drivers/npz2100.h    ← Public Zephyr API (include this in your app)
│       ├── npz2100_hal.h
│       ├── npz2100_lib.h
│       ├── npz2100_datalog.h    ← SRAM data-log extraction and parsing
│       ├── npz2100_regs_system.h
│       ├── npz2100_regs_io.h
│       ├── npz2100_regs_periph.h
│       └── npz2100_regs_adc_log.h
│
├── scripts/ci/                  ← Bench test runner shared by all samples
├── .github/workflows/samples.yml ← CI: twister builds + bench tests
│
└── samples/
    ├── sensor_tmp117/           ← Reference application (open this in VS Code)
    │   ├── CMakeLists.txt
    │   ├── prj.conf
    │   ├── sample.yaml          ← Twister build/test metadata
    │   ├── ci/test.py           ← Bench test (see Continuous integration)
    │   └── src/
    │       └── main.c
    ├── global_counter_alarm/    ← Same layout
    └── watchdog/                ← Same layout
```

---

## Prerequisites

| Tool | Version | Download |
|------|---------|----------|
| VS Code | Any recent | https://code.visualstudio.com |
| nRF Connect for VS Code extension pack | Latest | VS Code Extensions Marketplace |
| nRF Connect SDK | **3.0.2** | Installed via nRF Connect for VS Code |
| nRF Host Chip DK or custom board | Any NCS-supported nRF Host Chip | https://www.nordicsemi.com |

Install the nRF Connect SDK through the **nRF Connect for VS Code** extension:
`View → Extensions → nRF Connect for VS Code → Install SDK 3.0.2`

---

## Quick start - 5 steps

### Step 1 - Extract the zip

Extract this zip to a location of your choice. The result must be:

```
<your-path>/npz2100_ncs/
├── npz2100_module/
└── samples/
    └── sensor_tmp117/
```

Keep this layout intact - the sample's `CMakeLists.txt` references the module via a relative path (`../../npz2100_module`).

---

### Step 2 - Open the sample in VS Code

```
File → Open Folder → select npz2100_ncs/samples/sensor_tmp117/
```

VS Code will detect the nRF Connect application structure automatically.

---

### Step 3 - Add a build configuration

In the **nRF Connect** sidebar panel:

1. Click **+ Add Build Configuration**
2. Board: select your target (e.g. `nrf52833dk/nrf52833`, `nrf5340dk/nrf5340/cpuapp`, `nrf9160dk/nrf9160`)
3. SDK: `3.0.2`
4. Leave all other settings at default
5. Click **Build Configuration**

The `CMakeLists.txt` in `samples/sensor_tmp117/` registers `npz2100_module/` via `ZEPHYR_EXTRA_MODULES` and selects the `npz2100` shield - no additional setup is required.

> **nPZero shield:** The nPZ2100 devicetree node and Kconfig defaults come from
> the `npz2100` shield in `npz2100_module/boards/shields/npz2100/`, so samples
> carry no board overlays of their own. The shield has board-specific overlays
> for the nRF52833 DK and nRF52840 DK, and works on any board with an
> Arduino-header I²C bus (`arduino_i2c`). See **Supporting another board** below.
>
> From the terminal:
> `west build -b nrf52833dk/nrf52833 samples/sensor_tmp117`

---

### Step 4 - Connect your hardware

Wire the nPZ2100 to your nRF Host Chip:

| nPZ2100 pin | nRF Host Chip pin | Notes |
|-------------|---------------|-------|
| SDA | I²C SDA pin of your nRF Host Chip | Check your board's default I²C0 SDA |
| SCL | I²C SCL pin of your nRF Host Chip | Check your board's default I²C0 SCL |
| VBAT | VDD (3.0 V) | Device VDD or external supply |
| VSS | GND | |
| SW_HP | nRF Host Chip VDD | **Power control line - see below** |

**Required bypass capacitors** (place as close to the nPZ2100 as possible):

| Capacitor | Value | Pin |
|-----------|-------|-----|
| C1 | 100 nF C0G | VBAT |
| C2 | 10 nF C0G | VDD1V2 |
| C3 | 10 nF C0G | VDDD |

**SW_HP → nRF Host Chip VDD:**
The nPZ2100's SW_HP pin controls the nRF Host Chip power supply. When the nPZ2100
enters idle mode it de-asserts SW_HP, cutting power to the nRF Host Chip completely.
SW_HP should feed the nRF Host Chip VDD rail through an appropriate P-channel MOSFET
or power switch IC rated for the chip's current requirements.

> **For initial evaluation on a DK**, you can leave SW_HP disconnected and
> power the nRF Host Chip from the DK's USB. The driver will still initialise,
> communicate with the nPZ2100, and `npz2100_enter_idle()` will write the
> idle command - but the nRF Host Chip will remain powered since SW_HP is not
> connected to its supply. This is sufficient to test I²C communication,
> register configuration, and wake-reason decoding.

---

### Step 5 - Build, flash, and observe

In the nRF Connect panel:

1. Click **Build** (or `Ctrl+Shift+B`)
2. Click **Flash** once the build completes
3. Open **RTT Viewer** or the **Serial Terminal** to see output

Expected first-boot output:

```
[00:00:00.008] <inf> npz2100: nPZ2100 ready on i2c@40003000 @ 0x3c
[00:00:00.012] <inf> app: --- nPZ2100 sample boot ---
[00:00:00.015] <inf> npz2100: boot_status: STA1=0x00 STA2=0x00 STA3=0x00
[00:00:00.016] <inf> npz2100:   reset_src: Power-on reset (0x00)
[00:00:00.031] <inf> npz2100: apply_regmap: 18 register(s) written
[00:00:00.034] <inf> app: Returning to idle - nRF Host Chip power will be cut
[00:00:00.036] <inf> npz2100: Entering idle - nRF Host Chip power will be cut
```

If you see `nPZ2100 not found`, check your SDA/SCL wiring and verify the
I²C address matches the `reg = <0x3c>` in the shield overlay.

---

## Adapting the sample for your application

### Supporting another board

All samples use the `npz2100` shield, so board support is added once in the
module rather than per sample.

- **Board with an Arduino-header I²C bus** (`arduino_i2c`): no changes needed,
  the generic `npz2100.overlay` places the nPZ2100 on that bus.
- **Other boards, or extra board-specific settings**: add
  `npz2100_module/boards/shields/npz2100/boards/<board_target>.overlay`, where
  `<board_target>` matches the board string with `/` replaced by `_`
  (e.g. `nrf5340dk_nrf5340_cpuapp`, `your_custom_board`). It is applied
  automatically on top of the generic shield overlay.

Example for a board without `arduino_i2c` - adjust the I²C controller
(`&i2c0`, `&i2c1`, etc.) to match your hardware:

```dts
&i2c0 {
    status = "okay";
    clock-frequency = <I2C_BITRATE_STANDARD>;   /* 100 kHz */

    npz2100: npz2100@3c {
        compatible = "nanopower,npz2100";
        reg = <0x3c>;
        label = "NPZ2100";
    };
};
```

### Set I²C pins for your board

If your board's I²C pins are not already assigned in its board definition,
add a `pinctrl` block to the shield's board overlay:

```dts
&pinctrl {
    i2c0_default: i2c0_default {
        group1 {
            psels = <NRF_PSEL(TWIM_SDA, 0, YOUR_SDA_PIN)>,
                    <NRF_PSEL(TWIM_SCL, 0, YOUR_SCL_PIN)>;
        };
    };
    i2c0_sleep: i2c0_sleep {
        group1 {
            psels = <NRF_PSEL(TWIM_SDA, 0, YOUR_SDA_PIN)>,
                    <NRF_PSEL(TWIM_SCL, 0, YOUR_SCL_PIN)>;
            low-power-enable;
        };
    };
};

&i2c0 {
    pinctrl-0 = <&i2c0_default>;
    pinctrl-1 = <&i2c0_sleep>;
    pinctrl-names = "default", "sleep";
};
```

### Adding a new sample

Create a folder under `samples/` and copy these lines from
`sensor_tmp117/CMakeLists.txt`, before `find_package(Zephyr ...)`:

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../../npz2100_module)
list(APPEND SHIELD npz2100)
```

No `boards/` folder is needed. Add a `sample.yaml` so twister builds it,
and a `ci/test.py` (copy one from another sample) to test it on the bench.

> Select the shield in `CMakeLists.txt`, not with `--shield npz2100`.
> Sysbuild checks `--shield` before the sample registers the module and
> fails with `No shield named 'npz2100' found`. `--shield` only works when
> the module is in the workspace `west.yml`.

### Replace the register map

The `npz2100_regmap[]` array in `src/main.c` is a placeholder. Replace it
with the output of the **Nanopower configuration tool**.

The format is a flat byte stream of segments:
```
[length] [start_addr] [data_0] ... [data_(length-2)]
```
where `length = 1 (start_addr) + N (data bytes)`.

### Handle wake reasons

Add cases to `main.c` in the wake-reason handler section:

```c
if (reason.periph[0]) { /* peripheral 1 threshold crossed */ }
if (reason.periph[1]) { /* peripheral 2 threshold crossed */ }
if (reason.adc3)      { /* battery voltage threshold      */ }
if (reason.alarm)     { /* time counter alarm              */ }
if (reason.timeout)   { /* periodic time-out               */ }
```

### Modify configuration at runtime

Use the typed helpers and flush before re-entering idle:

```c
npz2100_config_t *shadow = npz2100_get_shadow(npz2100);

/* Example: slow down peripheral 1 polling when battery is low */
npz2100_periph_cfg_t pcfg = { .period = 4096, /* other fields */ };
npz2100_periph_set(shadow, 0, &pcfg);

npz2100_shadow_flush(npz2100);
npz2100_enter_idle(npz2100);
```

---

## Adding the module to your own application

### Option A - ZEPHYR_EXTRA_MODULES (no west.yml change)

Copy `npz2100_module/` next to your application and add to your
`CMakeLists.txt` before `find_package(Zephyr ...)`:

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../npz2100_module
)
list(APPEND SHIELD npz2100)
```

### Option B - west.yml (production, recommended)

Add the module as a West project and run `west update`:

```yaml
projects:
  - name: npz2100
    url: https://github.com/nanopowersemi/npz2100-zephyr  # your repo URL
    revision: v1.0.0
    path: modules/npz2100
```

Then remove the `ZEPHYR_EXTRA_MODULES` line from `CMakeLists.txt`.


---

## SRAM data-log

The nPZ2100 autonomously logs peripheral sensor readings into its SRAM while
the nRF Host Chip is powered off.  Use `npz2100_datalog.h` to extract and
parse the log after waking up.

`npz2100_datalog.c` is compiled automatically - it is registered in the
module's `CMakeLists.txt` alongside `npz2100_lib.c`.

Add the include in `main.c`:

```c
#include <drivers/npz2100.h>
#include "npz2100_datalog.h"   /* include directly - platform-agnostic */
```

### Extracting the log on FLOG wake

The `FLOG` flag in `reason.log_full` indicates the log is full.
Extract the log before re-entering idle to avoid losing data.

```c
if (reason.log_full) {

    npz2100_datalog_info_t  info;
    npz2100_datalog_entry_t entries[64];
    size_t count = 0u;

    /* Get the HAL handle and shadow from the Zephyr device */
    struct npz2100_data *data = npz2100->data;
    npz2100_hal_t    *hal    = &data->hal;
    npz2100_config_t *shadow = npz2100_get_shadow(npz2100);

    /* 1. Read metadata: LOGCFG, LOGCADDR, idle-entry timestamp */
    npz2100_datalog_read_info(hal, shadow, &info);

    /* 2. Read and parse all entries - oldest-first, rotation handled */
    npz2100_datalog_read_entries(hal, shadow, &info,
                                  entries, 64u, &count);

    /* 3. Process entries */
    for (size_t i = 0u; i < count; i++) {
        npz2100_datalog_entry_t *e = &entries[i];
        /* Fields: e->peripheral, e->triggered, e->timestamp, e->value */
        LOG_INF("slot=%u trig=%d val=0x%04X",
                e->peripheral, e->triggered, e->value);
    }

    /* 4. Clear log so next idle cycle starts fresh */
    npz2100_datalog_clear(hal, shadow, &info);
}
```

### Data-log API summary

| Function | Description |
|---|---|
| `npz2100_datalog_read_info(hal, cfg, &info)` | Read LOGCFG + LOGCADDR + idle-entry timestamp |
| `npz2100_datalog_entry_size(cfg, slot)` | Byte size of one entry for a peripheral slot |
| `npz2100_datalog_parse_entry(buf, len, cfg, &entry)` | Parse one entry from a raw buffer |
| `npz2100_datalog_read_raw(hal, cfg, &info, buf, len, &n)` | Raw SRAM read, oldest-first |
| `npz2100_datalog_read_entries(hal, cfg, &info, entries, max, &n)` | Read + parse all entries |
| `npz2100_datalog_clear(hal, cfg, &info)` | Erase log region, reset write pointer |

All functions are platform-agnostic - they take `npz2100_hal_t*` and
`npz2100_config_t*` directly, not a `struct device*`.

### Log rotation

Set `LOG_ROT=1` in your regmap so the nPZ2100 wraps around instead of
stopping on a full log.  The `reason.log_full` flag will not be set when
rotation is enabled - check `info.rotated` after `read_info()` instead.
`read_entries()` always returns entries oldest-first regardless of rotation.

---

## Continuous integration

CI has two parts:

| Part | Where it lives | What it checks |
|------|----------------|----------------|
| Build | `samples/*/sample.yaml`, run by twister | Every sample builds, warnings as errors, for nRF52 DK, nRF52833 DK and nRF52840 DK |
| Bench test | `samples/*/ci/test.py`, run by `scripts/ci/run_samples.py` | The sample is flashed to the bench board and its UART log is checked |

The bench is an nRF52 DK wired to an nPZ2100, with an Analog Discovery 3
(AD3) supplying power. `.github/workflows/samples.yml` runs both parts on
the self-hosted Linux runner the bench is attached to; the defaults at the
top of the workflow can be overridden with repository variables.

The nRF is powered from the nPZ2100 SW_HP, so it can only be programmed
while the nPZ2100 is in standby. For each sample, `run_samples.py --ad3`
therefore holds the nRF in reset (AD3 DIO5 on P0.21) while it power-cycles
the bench through the AD3. The nPZ2100 comes out of its power-on reset in
standby, and no old firmware runs to put it back in idle. It then erases
the nRF, releases reset, flashes the sample and captures the log for the time its `ci/test.py` asks for.
Then it checks the log against the test's patterns, plus failure strings
every sample shares (`not ready`, `failed: -<errno>`).

The bench builds add `scripts/ci/ci.conf` to each sample's `prj.conf`. It
sets `CONFIG_LOG_MODE_IMMEDIATE`, so log lines leave the UART when they are
logged and line up with the sniffed I2C traffic; twister builds are not
affected.

To run it by hand from an NCS environment, at the repository root:

```
pip install -r scripts/ci/requirements.txt
west twister -T samples -p nrf52dk/nrf52832 -p nrf52833dk/nrf52833 -p nrf52840dk/nrf52840
python scripts/ci/ad3_open.py
python scripts/ci/run_samples.py --board nrf52dk/nrf52832 --port /dev/ttyACM0 --ad3
python scripts/ci/erase.py
python scripts/ci/ad3_close.py
```

The steps are also available one at a time, as in github-runner-test:

| Script | Does |
|--------|------|
| `build.py [--board B] [--sample S]` | Builds samples to `build/ci/<sample>` |
| `flash.py --sample S` | Flashes a sample built by `build.py` |
| `erase.py` | Erases the nRF with `nrfutil device erase --all`, after the reset-held power cycle if `NPZ_CI_AD3` is set |
| `ad3_open.py` / `ad3_close.py` | Switches the AD3 supply on (left on) / off |
| `ad3_test.py` | AD3 smoke test: reads the scope, toggles DIO0-2 |

Add `--short-build-path` to twister on Windows. Add `--sample <name>` to
`run_samples.py` to test one sample, or run `samples/<name>/ci/test.py` with
the same arguments. `--build-only` skips flashing. To disable a bench test for a while, set
`SKIP = "<reason>"` in its `ci/test.py`: the sample is still built, and the
CI run shows a warning until `SKIP` is removed (`--no-skip` runs it anyway). The build output is only shown if a build
fails; add `--verbose` to always show it. Leave out `--ad3` (and
the `ad3_*.py` steps) on a bench without an AD3.

When the nRF is powered from the nPZ2100 SW_HP instead of USB, there is no
J-Link CDC port: flash with an external J-Link and use `--port ad3:4` (or
set `NPZ_CI_PORT` to `ad3:4`) to receive the nRF UART TX on AD3 DIO4.

To see the I2C traffic during a test, add `--i2c` (or set `NPZ_CI_AD3_I2C`
to `6,7`). On the bench, SDA is wired to AD3 DIO6 and SCL to DIO7; for other
pins, give them as `--i2c SDA,SCL` (not DIO0-2, which the AD3 drives, nor
the reset DIO5). The AD3 logic
analyzer then records the bus, plus the UART pin when the log comes from
`ad3:<pin>`, and decodes both, so each transaction is printed in order with
the log lines:

```
[UART] nPZ2100 global counter alarm sample boot
[I2C ]   1.204310 s  read  ID (0x01) -> 74
[I2C ]   1.204820 s  write ADCCFG (0x40) = 05
```

At the end of the capture comes a count of reads and writes per register,
and a test can look at the transactions in `log.i2c`. The default sample
rate of 1 MS/s suits 100 kHz I2C and 115200 baud; lost samples are reported, and the rate
can be set with `NPZ_CI_AD3_LA_RATE`.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| `nPZ2100 not found on i2c@... @ 0x3c` | Wrong I²C address or wiring | Check SDA/SCL, verify address with a logic analyser |
| `I2C bus ... not ready` | I²C controller not enabled in DT | Check that `CMakeLists.txt` has `list(APPEND SHIELD npz2100)` |
| Build error: `unknown vendor prefix 'nanopower'` | DT binding not found | Verify `npz2100_module/` path in `CMakeLists.txt` |
| Build error: `DT_HAS_NANOPOWER_NPZ2100_ENABLED` undefined, or `DT_N_NODELABEL_npz2100` undeclared | Shield not selected | Add `list(APPEND SHIELD npz2100)` to `CMakeLists.txt` |
| Build error: `No shield named 'npz2100' found` | Shield passed with `--shield` under sysbuild, or wrong module path | Select the shield in `CMakeLists.txt` instead; verify the `npz2100_module/` path |
| Build error: `undefined node label 'arduino_i2c'` | Board has no Arduino I²C bus | Add a shield board overlay, see **Supporting another board** |
| `apply_regmap: malformed byte stream` | Wrong `length` in regmap | Check segment framing: `length = 1 (start_addr) + N (data bytes)` |
| nRF Host Chip keeps rebooting immediately | SW_HP wired incorrectly | Verify SW_HP polarity and MOSFET circuit |
| Shield board overlay not picked up | Wrong overlay filename | Filename must match board target string, with `/` replaced by `_` |

---

## Key API reference

```c
#include <drivers/npz2100.h>

/* Boot sequence - call in this order on every boot */
int npz2100_boot_status(dev, &reason);   // read wake reason + kick watchdog
int npz2100_readback(dev);               // sync shadow from device
int npz2100_apply_regmap(dev, map, len); // write only changed registers

/* Runtime config */
npz2100_config_t *npz2100_get_shadow(dev);   // get shadow for typed helpers
int npz2100_shadow_flush(dev);               // push shadow to device

/* SRAM (sensor init commands) */
int npz2100_sram_write(dev, addr, data, len);
int npz2100_sram_read(dev, addr, data, len);

/* Control */
int npz2100_enter_idle(dev);   // does not return - nPZ2100 cuts power
int npz2100_soft_reset(dev);
int npz2100_probe(dev);        // verify ID register = 0x74
```

All functions return `0` on success or a standard Zephyr `errno` on failure.

---

## Contact

Nanopower Semiconductor AS  
www.nanopowersemi.com  
info@nanopowersemi.com
