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

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "target.h"

/*
 * Memory figures come from libopenwch's ld/devices.data.  Keep the two in
 * step: a wrong flash size means the flasher writes past the end of the part.
 *
 * Sizes are in bytes; devices.data states them in K.
 */
#define K(n) ((uint32_t)(n) * 1024u)

/*
 * One row per part.  The macros exist so that the interesting differences --
 * the LinkE chip id and the debug clock -- are visible as values in the table
 * rather than buried in nineteen columns of positional initialisers.
 *
 *   id   the family byte used by the LinkE protocol
 *   mid  the model id returned by chip detection
 *   spd  the debug clock selector for that part
 *   fb   where the programmer sees the code flash
 *   er   erase granularity, pr programmable granularity, algo the sequence
 */
#define CHIP(nm, fam, fl, rm, ro, id, mid, spd, fb, er, pr, algo)              \
	{nm, fam, K(fl), K(rm), ro, id, mid, spd, fb, er, pr, algo}

/* Where RAM starts.  Every part here has it at 0x20000000 except the two
 * CH57x parts that map it above their USB buffer. */
#define RAM_STD 0x20000000u
#define RAM_CH571 0x20003800u

/* Where the code flash is mapped, as the programmer addresses it. */
#define FB_V 0x08000000u /**< V, X and L series: the usual 0x08000000 */
#define FB_C 0x00000000u /**< CH5xx: code flash is at address zero */

/*
 * The order here is the order `chips` prints.  Parts grouped by family,
 * families in the order the library builds them.
 */
static const wl_chip_t chips[] = {
    /* CH32V00x -- QingKe V2, RV32EC.  Only the CH32V003 has its own id; the
     * rest of the family shares the one WCH's own tool uses for it. */
    CHIP("ch32v003",
	 "ch32v0",
	 16,
	 2,
	 RAM_STD,
	 0x09,
	 0x0030,
	 0x01,
	 FB_V,
	 64,
	 2,
	 WL_FLASH_CH32V0),
    CHIP("ch32v002",
	 "ch32v0",
	 16,
	 4,
	 RAM_STD,
	 0x4e,
	 0x0020,
	 0x01,
	 FB_V,
	 64,
	 2,
	 WL_FLASH_CH32V0),
    CHIP("ch32v004",
	 "ch32v0",
	 32,
	 6,
	 RAM_STD,
	 0x4e,
	 0x0040,
	 0x01,
	 FB_V,
	 64,
	 2,
	 WL_FLASH_CH32V0),
    CHIP("ch32v005",
	 "ch32v0",
	 32,
	 6,
	 RAM_STD,
	 0x4e,
	 0x0050,
	 0x01,
	 FB_V,
	 64,
	 2,
	 WL_FLASH_CH32V0),
    CHIP("ch32v006",
	 "ch32v0",
	 62,
	 8,
	 RAM_STD,
	 0x4e,
	 0x0060,
	 0x01,
	 FB_V,
	 64,
	 2,
	 WL_FLASH_CH32V0),
    CHIP("ch32v007",
	 "ch32v0",
	 62,
	 8,
	 RAM_STD,
	 0x4e,
	 0x0070,
	 0x01,
	 FB_V,
	 64,
	 2,
	 WL_FLASH_CH32V0),

    /* V-series with a V4 core -- RV32IMAC */
    CHIP("ch32v103",
	 "ch32v0v4",
	 64,
	 20,
	 RAM_STD,
	 0x01,
	 0x2500,
	 0x01,
	 FB_V,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
    CHIP("ch32v203",
	 "ch32v0v4",
	 64,
	 20,
	 RAM_STD,
	 0x05,
	 0x2030,
	 0x01,
	 FB_V,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
    CHIP("ch32v208",
	 "ch32v0v4",
	 64,
	 20,
	 RAM_STD,
	 0x05,
	 0x2080,
	 0x01,
	 FB_V,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
    CHIP("ch32v303",
	 "ch32v0v4",
	 288,
	 32,
	 RAM_STD,
	 0x06,
	 0x3030,
	 0x01,
	 FB_V,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
    CHIP("ch32v305",
	 "ch32v0v4",
	 288,
	 32,
	 RAM_STD,
	 0x06,
	 0x3050,
	 0x01,
	 FB_V,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
    CHIP("ch32v307",
	 "ch32v0v4",
	 288,
	 32,
	 RAM_STD,
	 0x06,
	 0x3070,
	 0x01,
	 FB_V,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),

    /* X and L series */
    CHIP("ch32x033",
	 "ch32x0",
	 62,
	 20,
	 RAM_STD,
	 0x0d,
	 0x0330,
	 0x01,
	 FB_V,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
    CHIP("ch32x035",
	 "ch32x0",
	 62,
	 20,
	 RAM_STD,
	 0x0d,
	 0x0350,
	 0x01,
	 FB_V,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
    CHIP("ch32l103",
	 "ch32l1",
	 64,
	 20,
	 RAM_STD,
	 0x0e,
	 0x1030,
	 0x01,
	 FB_V,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),

    /* CH57x -- RV32IMAC; the 571/573 put RAM higher in the map */
    CHIP("ch570",
	 "ch5xx57x",
	 240,
	 12,
	 RAM_STD,
	 0x8b,
	 0x7000,
	 0x03,
	 FB_C,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
    CHIP("ch572",
	 "ch5xx57x",
	 240,
	 12,
	 RAM_STD,
	 0x8b,
	 0x7200,
	 0x03,
	 FB_C,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
    CHIP("ch571",
	 "ch5xx57x",
	 192,
	 18,
	 RAM_CH571,
	 0x02,
	 0x7100,
	 0x02,
	 FB_C,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
    CHIP("ch573",
	 "ch5xx57x",
	 448,
	 18,
	 RAM_CH571,
	 0x02,
	 0x7300,
	 0x02,
	 FB_C,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),

    /* CH58x */
    CHIP("ch582",
	 "ch5xx58x",
	 448,
	 32,
	 RAM_STD,
	 0x07,
	 0x8200,
	 0x03,
	 FB_C,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
    CHIP("ch583",
	 "ch5xx58x",
	 448,
	 32,
	 RAM_STD,
	 0x07,
	 0x8300,
	 0x03,
	 FB_C,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
    CHIP("ch584",
	 "ch5xx58x",
	 448,
	 96,
	 RAM_STD,
	 0x4b,
	 0x8400,
	 0x02,
	 FB_C,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
    CHIP("ch585",
	 "ch5xx58x",
	 448,
	 128,
	 RAM_STD,
	 0x4b,
	 0x9300,
	 0x02,
	 FB_C,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),

    /* CH59x */
    CHIP("ch591",
	 "ch5xx59x",
	 192,
	 26,
	 RAM_STD,
	 0x0b,
	 0x9100,
	 0x03,
	 FB_C,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
    CHIP("ch592",
	 "ch5xx59x",
	 448,
	 26,
	 RAM_STD,
	 0x0b,
	 0x9200,
	 0x03,
	 FB_C,
	 0,
	 0,
	 WL_FLASH_UNSUPPORTED),
};

static const size_t chip_count = sizeof(chips) / sizeof(chips[0]);

const wl_chip_t *wl_chip_all(size_t *count) {
	if (count != NULL) {
		*count = chip_count;
	}

	return chips;
}

const wl_chip_t *wl_chip_by_detect_id(uint8_t family_id, uint16_t model_id) {
	size_t i;

	/* The vendor tools ignore the low nibble of the model id. */
	model_id &= 0xfff0u;

	for (i = 0; i < chip_count; i++) {
		if (chips[i].linke_id == family_id &&
		    chips[i].model_id == model_id) {
			return &chips[i];
		}
	}

	return NULL;
}

static bool name_matches(const char *candidate, const char *query) {
	size_t i;

	for (i = 0; candidate[i] != '\0'; i++) {
		unsigned char a = (unsigned char)candidate[i];
		unsigned char b = (unsigned char)query[i];

		if (b == '\0') {
			return false; /* query is shorter than the name */
		}

		if (tolower(a) != tolower(b)) {
			return false;
		}
	}

	return true;
}

const wl_chip_t *wl_chip_by_name(const char *name) {
	const wl_chip_t *best = NULL;
	size_t best_len = 0;
	size_t i;

	if (name == NULL || name[0] == '\0') {
		return NULL;
	}

	for (i = 0; i < chip_count; i++) {
		size_t len = strlen(chips[i].name);

		if (!name_matches(chips[i].name, name)) {
			continue;
		}

		if (best == NULL || len > best_len) {
			best = &chips[i];
			best_len = len;
		}
	}

	return best;
}

size_t wl_chip_name_max(void) {
	size_t max = 0;
	size_t i;

	for (i = 0; i < chip_count; i++) {
		size_t len = strlen(chips[i].name);

		if (len > max) {
			max = len;
		}
	}

	return max;
}

bool wl_chip_same(const char *a, const char *b) {
	const wl_chip_t *ca = wl_chip_by_name(a);
	const wl_chip_t *cb = wl_chip_by_name(b);

	return ca != NULL && ca == cb;
}
