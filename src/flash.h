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

#ifndef WCHLINK_FLASH_H
#define WCHLINK_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "linke.h"
#include "target.h"

/*
 * The flash half of the flasher: erase, program and verify, one family at a
 * time, on top of the debug-module primitives.
 *
 * Everything in here runs with the target halted, and reaches the flash
 * controller through the same two-instruction programs that reach memory --
 * the controller is a peripheral like any other.  That is why no family needs
 * a helper program uploaded into the target's RAM, which is what the
 * alternative approach (a "flash stub") would cost: an image per family, per
 * revision, embedded in the tool.
 *
 * The sequences themselves are the ones libopenwch's own flash drivers
 * implement for the same parts, so the flasher and the library agree about the
 * controller by construction rather than by two independent readings of the
 * reference manual.
 */

/**
 * Whether a write of @p length bytes at @p address lands inside the part.
 *
 * Exposed so that a caller can refuse a bad request before opening a
 * programmer: "that image is too big for this part" is a command-line
 * mistake, not a hardware problem.
 */
bool wl_flash_range_ok(const wl_chip_t *chip, uint32_t address, size_t length);

/**
 * Write @p length bytes of @p image at @p address.
 *
 * @param progress  called with (done, total) after each erase step
 */
enum wl_status wl_flash_write(wl_linke_t *link,
			      const wl_chip_t *chip,
			      uint32_t address,
			      const void *image,
			      size_t length,
			      void (*progress)(size_t done, size_t total));

/** Read the image back and compare it with what was written. */
enum wl_status wl_flash_verify(wl_linke_t *link,
			       const wl_chip_t *chip,
			       uint32_t address,
			       const void *image,
			       size_t length);

#endif /* WCHLINK_FLASH_H */
