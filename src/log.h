/*
 * This file is part of the libopenwch-tools project.
 *
 * Copyright (C) 2025 libopenwch contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef WCHLINK_LOG_H
#define WCHLINK_LOG_H

#include <stdarg.h>
#include <stdbool.h>

/**
 * Message levels, in increasing verbosity.  Everything goes to stderr so that
 * a command's real output can be piped without filtering diagnostics out.
 */
enum wl_level {
	WL_LEVEL_QUIET = 0, /**< errors only */
	WL_LEVEL_INFO,	    /**< what the tool is doing */
	WL_LEVEL_DEBUG,	    /**< what it is doing in detail */
};

/** Raise or lower the reporting threshold. */
void wl_log_set_level(enum wl_level level);

/** Current threshold. */
enum wl_level wl_log_level(void);

/**
 * Report a fatal problem.  Always printed, and always prefixed with the
 * program name so it reads well when several tools share a build log.
 */
void wl_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** Report something suspicious that did not stop us. */
void wl_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** Report progress.  Hidden at WL_LEVEL_QUIET. */
void wl_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** Report detail.  Hidden below WL_LEVEL_DEBUG. */
void wl_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * Hex dump, for the protocol work.  Hidden below WL_LEVEL_DEBUG.
 *
 * @param label  printed once before the dump
 * @param data   bytes to show
 * @param len    how many
 */
void wl_hexdump(const char *label, const void *data, unsigned int len);

#endif /* WCHLINK_LOG_H */
