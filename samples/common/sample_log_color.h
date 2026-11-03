/*
 * Copyright (c) Nanopower Semiconductor AS
 * SPDX-License-Identifier: Apache-2.0
 *
 * samples/common/sample_log_color.h
 * ---------------------------------
 * ANSI colors for highlighting key events in a sample's console log.
 *
 * Zephyr already prints errors in red and warnings in yellow when
 * CONFIG_LOG_BACKEND_SHOW_COLOR=y. This header adds colors for chosen info
 * messages on top of that:
 *
 *   LOG_INF_HL(SLC_GREEN, "Counter: %u s", n);
 *   LOG_INF_BANNER("Alarm fired");
 *
 * The color codes wrap the whole message, so the message text stays one
 * contiguous string that log parsers can match. The CI scripts strip ANSI
 * codes before matching (scripts/ci/ci_utils.py).
 *
 * Colors follow CONFIG_LOG_BACKEND_SHOW_COLOR, so turning that off gives
 * plain output. Build with -DSAMPLE_LOG_COLOR=0 to turn off only these.
 *
 * To use in a sample, add to its CMakeLists.txt:
 *   target_include_directories(app PRIVATE ../common)
 */

#ifndef SAMPLE_LOG_COLOR_H_
#define SAMPLE_LOG_COLOR_H_

#include <zephyr/logging/log.h>

#ifndef SAMPLE_LOG_COLOR
#ifdef CONFIG_LOG_BACKEND_SHOW_COLOR
#define SAMPLE_LOG_COLOR 1
#else
#define SAMPLE_LOG_COLOR 0
#endif
#endif

#if SAMPLE_LOG_COLOR
#define SLC_RESET    "\x1b[0m"
#define SLC_GRAY     "\x1b[90m"
#define SLC_GREEN    "\x1b[92m"
#define SLC_BLUE     "\x1b[94m"
#define SLC_MAGENTA  "\x1b[95m"
#define SLC_CYAN     "\x1b[1;96m"   /* bold */
/** Bold bright white on magenta, for the event that must stand out. */
#define SLC_BANNER   "\x1b[1;97;45m"
#else
#define SLC_RESET    ""
#define SLC_GRAY     ""
#define SLC_GREEN    ""
#define SLC_BLUE     ""
#define SLC_MAGENTA  ""
#define SLC_CYAN     ""
#define SLC_BANNER   ""
#endif

/** LOG_INF with the whole message in color. fmt must be a string literal. */
#define LOG_INF_HL(color, fmt, ...) \
	LOG_INF(color fmt SLC_RESET, ##__VA_ARGS__)

/**
 * Three-line banner: the message in SLC_BANNER between two bars. The bars
 * keep it visible in plain output too. msg must be a string literal.
 */
#define LOG_INF_BANNER(msg)                                                \
	do {                                                               \
		LOG_INF(SLC_BANNER "========================================" \
			SLC_RESET);                                        \
		LOG_INF(SLC_BANNER "   " msg "   " SLC_RESET);              \
		LOG_INF(SLC_BANNER "========================================" \
			SLC_RESET);                                        \
	} while (0)

#endif /* SAMPLE_LOG_COLOR_H_ */
