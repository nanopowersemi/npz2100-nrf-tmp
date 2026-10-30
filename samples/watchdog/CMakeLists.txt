# Copyright (c) Nanopower Semiconductor AS
# SPDX-License-Identifier: Apache-2.0
#
# samples/watchdog/CMakeLists.txt
# -------------------------------
# CMake entry point for the nPZ2100 watchdog sample: the host wakes every
# 3 seconds and kicks the nPZ2100 watchdog, then simulates a hang that the
# watchdog resets after about 6 seconds.
# Target: nRF5x / NCS 3.3.4 / VS Code nRF Connect extension.
#
# The nPZ2100 Zephyr module (npz2100_module/) is referenced via
# ZEPHYR_EXTRA_MODULES so no west.yml changes are required for evaluation.
#
# Building from the VS Code nRF Connect extension:
#   1. Open the samples/watchdog/ folder as the application.
#   2. Add build configuration → board: e.g. nrf52840dk/nrf52840.
#   3. The lines below add the module and the npz2100 shield automatically.
#
# Building from the terminal:
#   west build -b nrf52840dk/nrf52840 samples/watchdog/

cmake_minimum_required(VERSION 3.20)

# ---- Register the nPZ2100 module ----------------------------------------
#
# The path below is relative to this CMakeLists.txt and assumes the layout:
#
#   npz2100_ncs/
#   ├── npz2100_module/            ← Zephyr module (driver, headers, bindings)
#   └── samples/
#       └── watchdog/              ← This application (open this in VS Code)
#
# If you move npz2100_module/ elsewhere, update the path below to match.
list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../npz2100_module
)

# ---- Select the nPZero shield ---------------------------------------------
#
# Selected here rather than with --shield: sysbuild validates --shield
# before this file registers the module, so it would not find the shield.
list(APPEND SHIELD npz2100)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(npz2100_watchdog VERSION 1.0.0)

target_sources(app PRIVATE src/main.c)
