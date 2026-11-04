# Copyright (c) Nanopower Semiconductor AS
# SPDX-License-Identifier: Apache-2.0
#
# samples/sensor_as6212/CMakeLists.txt
# ------------------------------------
# CMake entry point for the nPZ2100 DK AS6212 sample: the nPZ2100 polls the
# AS6212 temperature sensor on the DK and wakes the host when the reading
# crosses its thresholds, or on the periodic time-out.
# Target: npz2100dk/nrf52833 / NCS 3.3.4 / VS Code nRF Connect extension.
#
# Building from the terminal:
#   west build -b npz2100dk/nrf52833 samples/sensor_as6212/
#
# With MCUboot (single application slot, serial recovery over the DK's
# USB-UART), see README.md:
#   west build -b npz2100dk/nrf52833 samples/sensor_as6212/ \
#       -- -DSB_CONF_FILE=sysbuild-mcuboot.conf

cmake_minimum_required(VERSION 3.20)

# ---- Register the nPZ2100 module ----------------------------------------
#
# The path below is relative to this CMakeLists.txt and assumes the layout:
#
#   npz2100_ncs/
#   ├── npz2100_module/        ← Zephyr module (driver, headers, board)
#   └── samples/
#       └── sensor_as6212/     ← This application (open this in VS Code)
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../npz2100_module
)

# ---- Select the nPZero shield ---------------------------------------------
#
# As in every sample. On the DK the shield overlay lands on the board's own
# nPZ2100 node (the DK labels I2C0 arduino_i2c), and it lets the sample
# build on the other boards too, where scripts/ci/build.py builds every
# sample. Selected here rather than with --shield: sysbuild validates
# --shield before this file registers the module.
list(APPEND SHIELD npz2100)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(npz2100_sensor_as6212 VERSION 1.0.0)

target_sources(app PRIVATE src/main.c)

# Shared sample helpers (sample_log_color.h).
target_include_directories(app PRIVATE ../common)
