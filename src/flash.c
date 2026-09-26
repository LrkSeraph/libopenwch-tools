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

#include <string.h>

#include "flash.h"
#include "flash_ch32v0.h"
#include "log.h"

/**
 * Where the programmer starts writing for a part, given the address the user
 * asked for.
 *
 * Flash is aliased at 0x00000000 and 0x08000000 on the V-series, and a .bin
 * built for one linker script may carry either.  Normalising here means
 * `flash -a 0x08000000` and `flash -a 0` are the same command, which is what a
 * user expects, and the comparison is exact rather than "looks like an
 * address".
 */
static uint32_t normalise(const wl_chip_t *chip, uint32_t address) {
	if (chip->flash_base == 0x08000000u && (address & 0xff000000u) == 0) {
		return address | 0x08000000u;
	}

	return address;
}

bool wl_flash_range_ok(const wl_chip_t *chip, uint32_t address, size_t length) {
	uint32_t start;

	if (chip == NULL || chip->flash_size == 0) {
		return false;
	}

	start = normalise(chip, address);

	return start >= chip->flash_base &&
	       (uint64_t)start + length <=
		   (uint64_t)chip->flash_base + (uint64_t)chip->flash_size;
}

enum wl_status wl_flash_write(wl_linke_t *link,
			      const wl_chip_t *chip,
			      uint32_t address,
			      const void *image,
			      size_t length,
			      void (*progress)(size_t done, size_t total)) {
	uint32_t start;

	if (chip == NULL || image == NULL) {
		return WL_ERR_USAGE;
	}

	if (length == 0) {
		wl_error("nothing to write: the image is empty");
		return WL_ERR_USAGE;
	}

	if (chip->algo == WL_FLASH_UNSUPPORTED) {
		wl_error("flashing %s is not implemented", chip->name);
		wl_error(
		    "this build writes the CH32V00x family only; the CH5xx "
		    "families need a different sequence, and guessing it "
		    "would risk bricking a part");
		return WL_ERR_NOT_IMPLEMENTED;
	}

	start = normalise(chip, address);

	if (!wl_flash_range_ok(chip, address, length)) {
		wl_error("0x%08x + %zu bytes does not fit %s's %uK of flash",
			 start, length, chip->name, chip->flash_size / 1024u);
		return WL_ERR_USAGE;
	}

	switch (chip->algo) {
	case WL_FLASH_CH32V0:
		return wl_flash_ch32v0_write(link, chip, start,
					     (const uint8_t *)image, length,
					     progress);
	default:
		wl_error("internal error: no writer for %s", chip->name);
		return WL_ERR_NOT_IMPLEMENTED;
	}
}

enum wl_status wl_flash_verify(wl_linke_t *link,
			       const wl_chip_t *chip,
			       uint32_t address,
			       const void *image,
			       size_t length,
			       void (*progress)(size_t done, size_t total)) {
	const uint8_t *expected = (const uint8_t *)image;
	uint8_t buffer[256];
	uint32_t start;
	size_t done = 0;

	if (chip == NULL || image == NULL) {
		return WL_ERR_USAGE;
	}

	start = normalise(chip, address);

	while (done < length) {
		size_t chunk = length - done;
		size_t i;
		enum wl_status status;

		if (chunk > sizeof(buffer)) {
			chunk = sizeof(buffer);
		}

		status = wl_linke_read_memory(
		    link, chip, start + (uint32_t)done, buffer, chunk);

		if (status != WL_OK) {
			return status;
		}

		for (i = 0; i < chunk; i++) {
			if (buffer[i] != expected[done + i]) {
				wl_error(
				    "verify failed at 0x%08x: wrote 0x%02x, "
				    "read 0x%02x",
				    start + (uint32_t)(done + i),
				    expected[done + i], buffer[i]);
				return WL_ERR_VERIFY;
			}
		}

		done += chunk;

		if (progress != NULL) {
			progress(done, length);
		}
	}

	return WL_OK;
}
