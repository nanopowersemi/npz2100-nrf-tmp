# nPZero (nPZ2100) shield

Zephyr shield that adds the Nanopower nPZ2100 power-saving IC to a host
board. Any application can use it without carrying its own board overlays.

## Usage

Register the module and select the shield in the application's
`CMakeLists.txt`, before `find_package(Zephyr ...)`:

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../../npz2100_module)
list(APPEND SHIELD npz2100)
```

Then build normally, from VS Code or with
`west build -b nrf52833dk/nrf52833 <app>`.

Passing `--shield npz2100` on the command line instead only works if the
module is already known when sysbuild starts, i.e. it is in the workspace
`west.yml` or passed as `-DZEPHYR_EXTRA_MODULES=<abs path>/npz2100_module`.
Sysbuild checks the shield name before the application's `CMakeLists.txt`
runs, so otherwise it reports `No shield named 'npz2100' found`.

## What the shield provides

| File | Content |
|------|---------|
| `npz2100.overlay` | nPZ2100 node (`npz2100`, address 0x3C) on `&arduino_i2c` at 100 kHz |
| `boards/<board>.overlay` | Board-specific additions (UART0 TX on P0.06, RX on P0.08, no flow control, for the nRF52 DKs) |
| `Kconfig.defconfig` | `CONFIG_I2C=y` by default; `CONFIG_NPZ2100` follows from the devicetree |

Supported boards with board-specific overlays:

- `nrf52dk/nrf52832`
- `nrf52833dk/nrf52833`
- `nrf52840dk/nrf52840`

Other boards with an Arduino-header I²C bus (`arduino_i2c`) work with the
generic overlay alone. This includes the nPZ2100 DK
(`npz2100dk/nrf52833`), whose board devicetree already has the nPZ2100
node on I²C0 and also labels that bus `arduino_i2c`. For a board without one, add
`boards/<board>.overlay` here that defines the `npz2100` node on the right
I²C controller, or put the node in the application's own overlay.

Applications reference the device as:

```c
const struct device *npz2100 = DEVICE_DT_GET(DT_NODELABEL(npz2100));
```

## Hardware connections

| nPZ2100 pin | Host pin (nRF52 DK / nRF52833 DK / nRF52840 DK) |
|-------------|--------------------------------------------------|
| SDA | P0.26 (Arduino SDA) |
| SCL | P0.27 (Arduino SCL) |
| VBAT | 3.0 V supply rail |
| VSS | GND |
| SW_HP | Host VDD (via power switch) |

Required bypass capacitors, close to the nPZ2100: 100 nF C0G on VBAT,
10 nF C0G on VDD1V2, 10 nF C0G on VDDD.
