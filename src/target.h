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

#ifndef WCHLINK_TARGET_H
#define WCHLINK_TARGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * What the programmer needs to know about a target part.
 *
 * The memory figures are copied from libopenwch's `ld/devices.data`, which is
 * the project's own device database, rather than from a datasheet typed in by
 * hand -- the two would otherwise drift.
 *
 * There is deliberately no device-ID field yet.  Identifying a part from the
 * ID the programmer reports is milestone M2 work, and inventing IDs here would
 * be worse than having none: the flasher would silently pick the wrong part.
 */
typedef struct {
	const char *name;    /**< base part name, e.g. "ch32v003" */
	const char *family;  /**< libopenwch family, e.g. "ch32v0" */
	uint32_t flash_size; /**< bytes of code flash */
	uint32_t ram_size;   /**< bytes of RAM */
	uint32_t ram_offset; /**< where RAM starts, e.g. 0x20000000 */
} wl_chip_t;

/**
 * The whole table, in declaration order.
 * @param count  receives the number of entries
 */
const wl_chip_t *wl_chip_all(size_t *count);

/**
 * Look a part up by name.
 *
 * Matching is by prefix, so the full order code works as well as the base
 * name: "ch32v003f4p6", "ch32v003" and "ch32v003F4P6" all find the same part.
 * The longest match wins, so a shorter name that prefixes a longer one cannot
 * shadow it.  Matching is case-insensitive.
 *
 * @return the part, or NULL when nothing matches
 */
const wl_chip_t *wl_chip_by_name(const char *name);

/** Length of the longest name in the table, for aligning columns. */
size_t wl_chip_name_max(void);

/**
 * Whether two names refer to the same part.
 * @return true when both resolve to one entry and it is the same one
 */
bool wl_chip_same(const char *a, const char *b);

#endif /* WCHLINK_TARGET_H */
