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

/** How a family's flash controller is driven. */
enum wl_flash_algo {
	WL_FLASH_UNSUPPORTED = 0, /**< this tool has no algorithm for it yet */
	WL_FLASH_CH32V0,	  /**< FLASH controller at 0x40022000 */
};

/**
 * What the programmer needs to know about a target part.
 *
 * The memory figures are copied from libopenwch's `ld/devices.data`, which is
 * the project's own device database, rather than from a datasheet typed in by
 * hand -- the two would otherwise drift.
 *
 * `linke_id`, `model_id` and `interface_speed` are WCH's own protocol
 * constants: the family/model pair the programmer reports during detection
 * and expects to be told before it will talk to a part, and the selector for
 * the debug clock it should use for that part.  They are facts about the
 * programmer, not guesses, and a wrong value here is one of the ways a
 * flasher damages a target or silently addresses the wrong one.
 *
 * The erase and program sizes belong to the algorithm rather than to the part,
 * so they are zero for a family with no algorithm here yet: the value only
 * means anything once something uses it.
 */
typedef struct {
	const char *name;	 /**< base part name, e.g. "ch32v003" */
	const char *family;	 /**< libopenwch family, e.g. "ch32v0" */
	uint32_t flash_size;	 /**< bytes of code flash */
	uint32_t ram_size;	 /**< bytes of RAM */
	uint32_t ram_offset;	 /**< where RAM starts, e.g. 0x20000000 */
	uint8_t linke_id;	 /**< family byte for the programmer */
	uint16_t model_id;	 /**< LinkE model id used for auto-detection */
	uint8_t interface_speed; /**< debug clock selector for this part */
	uint32_t flash_base;	 /**< where the programmer sees the array */
	uint32_t erase_size;	 /**< smallest erasable unit, bytes */
	uint32_t program_size;	 /**< smallest programmable unit, bytes */
	enum wl_flash_algo algo; /**< which controller sequence to use */
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

/**
 * Resolve the family/model pair reported by the WCH-LinkE.
 *
 * The programmer answers a chip-detection command with a family byte and a
 * 16-bit model value; the low four bits of the model value are ignored, as
 * the vendor tools do.
 *
 * @return the matching part, or NULL when the pair is unknown
 */
const wl_chip_t *wl_chip_by_detect_id(uint8_t family_id, uint16_t model_id);

/** Length of the longest name in the table, for aligning columns. */
size_t wl_chip_name_max(void);

/**
 * Whether two names refer to the same part.
 * @return true when both resolve to one entry and it is the same one
 */
bool wl_chip_same(const char *a, const char *b);

#endif /* WCHLINK_TARGET_H */
