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

static const wl_chip_t chips[] = {
    /* CH32V00x — QingKe V2, RV32EC */
    {"ch32v003", "ch32v0", K(16), K(2), 0x20000000u},
    {"ch32v002", "ch32v0", K(16), K(4), 0x20000000u},
    {"ch32v004", "ch32v0", K(32), K(6), 0x20000000u},
    {"ch32v005", "ch32v0", K(32), K(6), 0x20000000u},
    {"ch32v006", "ch32v0", K(62), K(8), 0x20000000u},
    {"ch32v007", "ch32v0", K(62), K(8), 0x20000000u},

    /* V-series with a V4 core — RV32IMAC */
    {"ch32v103", "ch32v0v4", K(64), K(20), 0x20000000u},
    {"ch32v203", "ch32v0v4", K(64), K(20), 0x20000000u},
    {"ch32v208", "ch32v0v4", K(64), K(20), 0x20000000u},
    {"ch32v303", "ch32v0v4", K(288), K(32), 0x20000000u},
    {"ch32v305", "ch32v0v4", K(288), K(32), 0x20000000u},
    {"ch32v307", "ch32v0v4", K(288), K(32), 0x20000000u},

    /* X and L series */
    {"ch32x033", "ch32x0", K(62), K(20), 0x20000000u},
    {"ch32x035", "ch32x0", K(62), K(20), 0x20000000u},
    {"ch32l103", "ch32l1", K(64), K(20), 0x20000000u},

    /* CH57x — RV32IMAC; the 571/573 put RAM higher in the map */
    {"ch570", "ch5xx57x", K(240), K(12), 0x20000000u},
    {"ch572", "ch5xx57x", K(240), K(12), 0x20000000u},
    {"ch571", "ch5xx57x", K(192), K(18), 0x20003800u},
    {"ch573", "ch5xx57x", K(448), K(18), 0x20003800u},

    /* CH58x */
    {"ch582", "ch5xx58x", K(448), K(32), 0x20000000u},
    {"ch583", "ch5xx58x", K(448), K(32), 0x20000000u},
    {"ch584", "ch5xx58x", K(448), K(96), 0x20000000u},
    {"ch585", "ch5xx58x", K(448), K(128), 0x20000000u},

    /* CH59x */
    {"ch591", "ch5xx59x", K(192), K(26), 0x20000000u},
    {"ch592", "ch5xx59x", K(448), K(26), 0x20000000u},
};

static const size_t chip_count = sizeof(chips) / sizeof(chips[0]);

const wl_chip_t *wl_chip_all(size_t *count) {
	if (count != NULL) {
		*count = chip_count;
	}

	return chips;
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
