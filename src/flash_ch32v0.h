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

#ifndef WCHLINK_FLASH_CH32V0_H
#define WCHLINK_FLASH_CH32V0_H

#include "flash.h"

/**
 * The CH32V00x flash controller, driven over the debug interface.
 *
 * The sequence is the one libopenwch's `flash_common_v1.c` implements on the
 * target and the one WCH's own EVT driver uses, so all three agree:
 *
 *   erase    CTLR = PAGE_ER;  ADDR = page;  CTLR = PAGE_ER | STRT;  wait
 *   program  CTLR = PG;       store halfword;                      wait
 *
 * PAGE_ER erases 64 bytes, which is the memory's page; PG programs one
 * halfword.  The controller is unlocked with the usual two-key write to KEYR
 * first, and the LOCK bit is read back afterwards: if it is still set, the
 * unlock did not take, and programming would silently do nothing.
 *
 * Nothing here decides the family or checks the address range -- the caller in
 * flash.c has done that.
 */
enum wl_status wl_flash_ch32v0_write(wl_linke_t *link,
				     const wl_chip_t *chip,
				     uint32_t address,
				     const uint8_t *image,
				     size_t length,
				     void (*progress)(size_t done,
						      size_t total));

#endif /* WCHLINK_FLASH_CH32V0_H */
