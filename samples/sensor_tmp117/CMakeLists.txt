# Copyright (c) Nanopower Semiconductor AS
# SPDX-License-Identifier: Apache-2.0
#
# samples/sensor_tmp117/CMakeLists.txt
# ------------------------------------
# CMake entry point for the nPZ2100 sample application.
# Target: nRF52833 / NCS 3.0.2 / VS Code nRF Connect extension.
#
# The nPZ2100 Zephyr module (npz2100_module/) is referenced via
# ZEPHYR_EXTRA_MODULES so no west.yml changes are required for evaluation.
#
# When promoting to production, add the module to your workspace west.yml
# instead (see npz2100_module/zephyr/module.yml for the manifest snippet).
#
# Building from the VS Code nRF Connect extension:
#   1. Open the samples/sensor_tmp117/ folder as the application.
#   2. Add build configuration → board: nrf52833dk/nrf52833.
#   3. The lines below add the module and the npz2100 shield automatically.
#
# Building from the terminal:
#   west build -b nrf52833dk/nrf52833 samples/sensor_tmp117/
#
# The nPZ2100 devicetree node and Kconfig defaults come from the npz2100
# shield in npz2100_module/boards/shields/npz2100/, so this sample carries
# no board overlays of its own.

cmake_minimum_required(VERSION 3.20)

# ---- Register the nPZ2100 module ----------------------------------------
#
# ZEPHYR_EXTRA_MODULES tells the Zephyr build system to treat the module
# directory as a West module, loading its zephyr/module.yml automatically.
# This adds the driver, DT bindings, include paths and the npz2100 shield
# without modifying west.yml or the NCS installation.
#
# The path below is relative to this CMakeLists.txt.  It assumes the
# delivered zip is extracted with the expected layout, i.e. npz2100_module/
# is two levels up from this sample:
#
#   npz2100_ncs/
#   ├── npz2100_module/        ← Zephyr module (driver, headers, bindings)
#   └── samples/
#       └── sensor_tmp117/     ← This application (open this in VS Code)
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

project(sensor_tmp117 VERSION 1.0.0)

target_sources(app PRIVATE src/main.c)

# Shared sample helpers (sample_log_color.h).
target_include_directories(app PRIVATE ../common)
