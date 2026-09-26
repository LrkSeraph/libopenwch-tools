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

#include "dm.h"
#include "flash_ch32v0.h"
#include "log.h"

/* --- the controller ------------------------------------------------------ */

#define FLASH_R_BASE 0x40022000u

#define FLASH_KEYR (FLASH_R_BASE + 0x04u)
#define FLASH_STATR (FLASH_R_BASE + 0x0cu)
#define FLASH_CTLR (FLASH_R_BASE + 0x10u)
#define FLASH_ADDR (FLASH_R_BASE + 0x14u)
#define FLASH_MODEKEYR (FLASH_R_BASE + 0x24u)

#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xcdef89abu

#define FLASH_CTLR_PG 0x00000001u      /**< normal halfword program */
#define FLASH_CTLR_STRT 0x00000040u    /**< start the operation */
#define FLASH_CTLR_LOCK 0x00000080u    /**< controller is locked */
#define FLASH_CTLR_PAGE_PG 0x00010000u /**< 64-byte page program mode */
#define FLASH_CTLR_PAGE_ER 0x00020000u /**< 64-byte page erase */
#define FLASH_CTLR_BUF_LOAD                                                    \
	0x00040000u /**< load one word into the page buffer */
#define FLASH_CTLR_BUF_RST 0x00080000u /**< reset the page buffer */

#define FLASH_STATR_BSY 0x00000001u	 /**< an operation is running */
#define FLASH_STATR_WRPRTERR 0x00000010u /**< write protection refused it */

/** How many status polls to allow before calling the controller stuck. */
#define FLASH_POLL_LIMIT 200000u

/** Largest page any algorithm here may hand to the merge buffer. */
#define FLASH_MERGE_MAX 256u

/**
 * Wait for the controller to go idle.
 *
 * Erasing a page takes on the order of milliseconds, and every poll is a USB
 * round trip, so this loops far longer than any real operation needs.  The
 * limit is there so that a target that has stopped answering ends the command
 * with a message rather than hanging the tool.
 */
static enum wl_status wait_ready(wl_linke_t *link, const char *what) {
	uint32_t status = 0;
	unsigned polls;

	for (polls = 0; polls < FLASH_POLL_LIMIT; polls++) {
		enum wl_status result =
		    wl_dm_read32(link, FLASH_STATR, &status);

		if (result != WL_OK) {
			return result;
		}

		if ((status & FLASH_STATR_BSY) == 0) {
			break;
		}
	}

	if ((status & FLASH_STATR_BSY) != 0) {
		wl_error("the flash controller never finished %s", what);
		return WL_ERR_TARGET;
	}

	if ((status & FLASH_STATR_WRPRTERR) != 0) {
		wl_error("the flash controller refused %s: the page is "
			 "write-protected (STATR = 0x%08x)",
			 what, status);
		return WL_ERR_TARGET;
	}

	return WL_OK;
}

/** Read-modify-write the control register. */
static enum wl_status
ctlr_update(wl_linke_t *link, uint32_t set, uint32_t clear) {
	uint32_t ctlr = 0;
	enum wl_status status = wl_dm_read32(link, FLASH_CTLR, &ctlr);

	if (status != WL_OK) {
		return status;
	}

	ctlr = (ctlr | set) & ~clear;

	return wl_dm_write32(link, FLASH_CTLR, ctlr);
}

/** Unlock both the normal controller and the fast page path. */
static enum wl_status unlock(wl_linke_t *link) {
	enum wl_status status;
	uint32_t ctlr = 0;

	status = wl_dm_write32(link, FLASH_KEYR, FLASH_KEY1);

	if (status == WL_OK) {
		status = wl_dm_write32(link, FLASH_KEYR, FLASH_KEY2);
	}

	if (status == WL_OK) {
		status = wl_dm_write32(link, FLASH_MODEKEYR, FLASH_KEY1);
	}

	if (status == WL_OK) {
		status = wl_dm_write32(link, FLASH_MODEKEYR, FLASH_KEY2);
	}

	if (status != WL_OK) {
		return status;
	}

	status = wl_dm_read32(link, FLASH_CTLR, &ctlr);

	if (status != WL_OK) {
		return status;
	}

	if ((ctlr & FLASH_CTLR_LOCK) != 0) {
		wl_error(
		    "the flash controller is still locked after the unlock "
		    "sequence (CTLR = 0x%08x)",
		    ctlr);
		wl_error("this usually means the target is not halted, or that "
			 "the part is not the one selected with --chip");
		return WL_ERR_TARGET;
	}

	return WL_OK;
}

/** Erase the 64-byte fast page at @p page. */
static enum wl_status erase_page_fast(wl_linke_t *link, uint32_t page) {
	enum wl_status status;

	status = wait_ready(link, "the previous operation");

	if (status == WL_OK) {
		status = ctlr_update(link, FLASH_CTLR_PAGE_ER, 0);
	}

	if (status == WL_OK) {
		status = wl_dm_write32(link, FLASH_ADDR, page);
	}

	if (status == WL_OK) {
		status = ctlr_update(link, FLASH_CTLR_STRT, 0);
	}

	if (status == WL_OK) {
		status = wait_ready(link, "an erase");
	}

	(void)ctlr_update(link, 0, FLASH_CTLR_PAGE_ER | FLASH_CTLR_STRT);

	return status;
}

/** Reset the 64-byte load buffer. */
static enum wl_status buffer_reset(wl_linke_t *link) {
	enum wl_status status;

	status = ctlr_update(link, FLASH_CTLR_PAGE_PG, 0);

	if (status == WL_OK) {
		status = ctlr_update(link, FLASH_CTLR_BUF_RST, 0);
	}

	if (status == WL_OK) {
		status = wait_ready(link, "the flash buffer reset");
	}

	(void)ctlr_update(link, 0, FLASH_CTLR_PAGE_PG | FLASH_CTLR_BUF_RST);

	return status;
}

/** Load one 32-bit word into the next slot of the page buffer. */
static enum wl_status
buffer_load(wl_linke_t *link, uint32_t address, uint32_t word) {
	enum wl_status status;

	status = ctlr_update(link, FLASH_CTLR_PAGE_PG, 0);

	if (status == WL_OK) {
		status = wl_dm_write32(link, address, word);
	}

	if (status == WL_OK) {
		status = ctlr_update(link, FLASH_CTLR_BUF_LOAD, 0);
	}

	if (status == WL_OK) {
		status = wait_ready(link, "the flash buffer load");
	}

	(void)ctlr_update(link, 0, FLASH_CTLR_PAGE_PG | FLASH_CTLR_BUF_LOAD);

	return status;
}

/** Program the buffered 64-byte page at @p page. */
static enum wl_status program_page_fast(wl_linke_t *link, uint32_t page) {
	enum wl_status status;

	status = ctlr_update(link, FLASH_CTLR_PAGE_PG, 0);

	if (status == WL_OK) {
		status = wl_dm_write32(link, FLASH_ADDR, page);
	}

	if (status == WL_OK) {
		status = ctlr_update(link, FLASH_CTLR_STRT, 0);
	}

	if (status == WL_OK) {
		status = wait_ready(link, "a page program");
	}

	(void)ctlr_update(link, 0, FLASH_CTLR_PAGE_PG | FLASH_CTLR_STRT);

	return status;
}

/* --- the write itself ---------------------------------------------------- */

enum wl_status wl_flash_ch32v0_write(wl_linke_t *link,
				     const wl_chip_t *chip,
				     uint32_t address,
				     const uint8_t *image,
				     size_t length,
				     void (*progress)(size_t done,
						      size_t total)) {
	const uint32_t page_size = chip->erase_size;
	uint32_t start = address;
	uint32_t end = address + (uint32_t)length;
	uint32_t page;
	enum wl_status status;

	if (page_size == 0 || (page_size & (page_size - 1u)) != 0 ||
	    (page_size & 3u) != 0 || page_size > FLASH_MERGE_MAX) {
		wl_error("internal error: bad erase size for %s", chip->name);
		return WL_ERR_USAGE;
	}

	status = unlock(link);

	if (status != WL_OK) {
		return status;
	}

	/*
	 * Erase and program page by page.  A page that the image only partly
	 * covers is read first and merged, so that flashing a few bytes at an
	 * offset does not wipe the rest of the page: the erase unit is larger
	 * than the program unit, and that asymmetry is the caller's to not
	 * trip over.
	 */
	for (page = start & ~(page_size - 1u); page < end; page += page_size) {
		uint8_t merged[FLASH_MERGE_MAX];
		size_t i;
		bool whole_page = (page >= start) && (page + page_size <= end);

		if (whole_page) {
			memset(merged, 0xff, page_size);
		} else {
			status =
			    wl_dm_read_block(link, page, merged, page_size);

			if (status != WL_OK) {
				return status;
			}
		}

		for (i = 0; i < page_size; i++) {
			uint32_t at = page + (uint32_t)i;

			if (at >= start && at < end) {
				merged[i] = image[at - start];
			}
		}

		status = erase_page_fast(link, page);

		if (status == WL_OK) {
			status = buffer_reset(link);
		}

		if (status == WL_OK) {
			for (i = 0; i < page_size; i += 4) {
				uint32_t word =
				    (uint32_t)merged[i] |
				    ((uint32_t)merged[i + 1] << 8) |
				    ((uint32_t)merged[i + 2] << 16) |
				    ((uint32_t)merged[i + 3] << 24);

				status =
				    buffer_load(link, page + (uint32_t)i, word);

				if (status != WL_OK) {
					break;
				}
			}
		}

		if (status == WL_OK) {
			status = program_page_fast(link, page);
		}

		if (status != WL_OK) {
			wl_error("failed while writing 0x%08x", page);
			return status;
		}

		if (progress != NULL) {
			size_t done = page + page_size >= end
					  ? length
					  : (size_t)(page + page_size - start);

			progress(done, length);
		}
	}

	return WL_OK;
}
