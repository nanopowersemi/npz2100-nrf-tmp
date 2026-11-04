# Copyright (c) Nanopower Semiconductor AS
# SPDX-License-Identifier: Apache-2.0
#
# CMakeLists.txt  (module root)
# ------------------------------
# Entry point for the Zephyr build system when this repository is used as a
# West module under NCS v3.3.4.
#
# Zephyr's module infrastructure calls add_subdirectory() on the path declared
# in zephyr/module.yml cmake: field (this file's directory).  We forward into
# the driver subdirectory only when CONFIG_NPZ2100 is enabled.  Every image
# under sysbuild loads this module, MCUboot included, and there I2C is off,
# so CONFIG_NPZ2100 (and CONFIG_NPZ2100_LOG_LEVEL) is not defined.
#
# The include/ directory is added here so that <drivers/npz2100.h> is
# resolvable from application and driver code without any extra configuration.

cmake_minimum_required(VERSION 3.20)

# Expose include/ to all targets in the workspace.
zephyr_include_directories(include)

# Build the driver only in images that enable it.
add_subdirectory_ifdef(CONFIG_NPZ2100 drivers/npz2100)
