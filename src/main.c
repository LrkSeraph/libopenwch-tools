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

/*
 * wchlink -- drive a WCH-LinkE from the command line.
 *
 * `info`, `chips`, `flash`, `read`, `reset` and `unbrick` are implemented.
 * Flashing is implemented for the CH32V00x family; the other families this
 * table knows about are rejected with an explanation rather than written to
 * with a sequence nobody has checked.
 *
 * None of the flashing paths has been run against a real part yet: the
 * protocol and the algorithm are exercised end to end against the simulated
 * programmer in tests/, which is as far as a machine without hardware can go.
 *
 * Exit status:
 *   0  success
 *   1  a USB, programmer or target error
 *   2  bad command line
 *   3  no usable programmer found
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dm.h"
#include "flash.h"
#include "linke.h"
#include "log.h"
#include "target.h"
#include "usb.h"

#define WL_VERSION "0.1.0"

/** Capacity for `programmer list` on a single host. */
#define WL_MAX_PROGRAMMERS 16

static const char *program_name = "wchlink";

#define WL_TARGET_EXTRA_MAX 4

struct options {
	const char *serial;
	const char *chip;
	const char *file;
	const char *extra[WL_TARGET_EXTRA_MAX];
	size_t extra_count;
	uint32_t address;
	size_t size;
	bool address_given;
	bool size_given;
	bool verify;
};

static void usage(FILE *out) {
	fprintf(
	    out,
	    "usage: %s <command> [options]\n"
	    "\n"
	    "Commands:\n"
	    "  info                 report the target chip; auto-detect it "
	    "unless --chip is given\n"
	    "  chips                list the parts this tool knows about\n"
	    "  flash <file.bin>     write a binary image to flash\n"
	    "  read  <file.bin>     read memory; no --address/--size dumps "
	    "the whole flash\n"
	    "  reset                reset the target and let it run\n"
	    "  unbrick              hold the target in reset and power-cycle "
	    "it\n"
	    "  programmer <sub>     inspect or control the WCH-LinkE itself\n"
	    "                       subcommands: info, list, rv, iap\n"
	    "  target <sub>         debug the target: halt, resume, reset, "
	    "pc,\n"
	    "                       regs, read32 <addr>, write32 <addr> "
	    "<value>\n"
	    "  terminal             single-wire debug terminal (milestone 4)\n"
	    "\n"
	    "Options:\n"
	    "  -s, --serial <sn>    pick a programmer by serial number\n"
	    "  -c, --chip <part>    target part; omitted means auto-detect\n"
	    "  -a, --address <addr> where to write, or what to read from\n"
	    "  -n, --size <bytes>   number of bytes to read\n"
	    "      --verify         read the image back and compare (default)\n"
	    "      --no-verify      skip the read-back\n"
	    "  -v, --verbose        show what is happening\n"
	    "  -q, --quiet          errors only\n"
	    "  -h, --help           this text\n"
	    "      --version        print the tool version\n",
	    program_name);
}

static bool parse_u32(const char *text, uint32_t *out) {
	char *end = NULL;
	unsigned long value;

	if (text == NULL || text[0] == '\0') {
		return false;
	}

	errno = 0;
	value = strtoul(text, &end, 0);

	if (errno != 0 || end == NULL || *end != '\0') {
		return false;
	}

	if (value > 0xfffffffful) {
		return false;
	}

	*out = (uint32_t)value;

	return true;
}

static void print_chip(const wl_chip_t *chip, size_t name_width) {
	printf("  %-*s  %-9s  %5uK flash  %5uK RAM\n", (int)name_width,
	       chip->name, chip->family, chip->flash_size / 1024u,
	       chip->ram_size / 1024u);
}

/** Print the target block shared by `info` before and after detection. */
static void print_target_info(const wl_chip_t *chip) {
	printf("target: %s (%s)\n", chip->name, chip->family);
	printf("  flash:  %u bytes (%uK)\n", chip->flash_size,
	       chip->flash_size / 1024u);
	printf("  ram:    %u bytes at 0x%08x\n", chip->ram_size,
	       chip->ram_offset);
}

/** Print the USB identity and current mode of a discovered programmer. */
static void print_programmer_info(const wl_usb_info_t *info) {
	printf("programmer:\n");
	printf("  usb:    %04x:%04x at bus %u address %u\n", info->vid,
	       info->pid, info->bus, info->address);
	printf("  mode:   %s\n", wl_usb_mode_name(info->mode));
	printf("  serial: %s\n",
	       info->serial[0] != '\0' ? info->serial : "(not reported)");
	printf("  product: %s\n",
	       info->product[0] != '\0' ? info->product : "(not reported)");
}

/**
 * Read the programmer firmware version without changing its mode.
 *
 * The normal open path insists on RISC-V debug mode; a programmer-info
 * command should be able to report an ARM-mode or IAP device as it is, so
 * this opens the USB handle directly and talks the programmer-status command
 * over its normal transport.
 */
static void print_programmer_version(const wl_usb_info_t *info) {
	wl_usb_link_t *usb = NULL;
	wl_linke_t link;
	char version[64];
	enum wl_status status;
	int rc;

	memset(version, 0, sizeof(version));

	rc = wl_usb_open(info, &usb);

	if (rc < 0) {
		printf("  version: (cannot open: %s)\n", wl_usb_strerror(rc));
		return;
	}

	wl_linke_attach_transport(&link, wl_usb_transport(), usb);

	status = wl_linke_get_version(&link, version, sizeof(version));

	wl_usb_close(usb);

	if (status == WL_OK) {
		printf("  version: %s\n", version);
	} else {
		printf("  version: %s\n", wl_status_str(status));
	}
}

/* --- commands ----------------------------------------------------------- */

static enum wl_status cmd_chips(void) {
	size_t count = 0;
	size_t width = wl_chip_name_max();
	const wl_chip_t *chips = wl_chip_all(&count);
	size_t i;

	printf("%zu parts:\n", count);

	for (i = 0; i < count; i++) {
		print_chip(&chips[i], width);
	}

	return WL_OK;
}

static enum wl_status cmd_info(const struct options *opts) {
	const wl_chip_t *chip = NULL;
	wl_linke_t link;
	enum wl_status status;

	if (opts->chip != NULL) {
		chip = wl_chip_by_name(opts->chip);

		if (chip == NULL) {
			wl_error("unknown part '%s' (try `%s chips`)",
				 opts->chip, program_name);
			return WL_ERR_USAGE;
		}

		print_target_info(chip);
		return WL_OK;
	}

	status = wl_linke_open(&link, opts->serial);

	if (status != WL_OK) {
		return status;
	}

	status = wl_linke_identify_chip(&link, &chip);

	if (status != WL_OK) {
		wl_error(
		    "cannot detect the target; connect it, or pass --chip");
		wl_linke_close(&link);
		return status;
	}

	print_target_info(chip);

	wl_linke_close(&link);

	return WL_OK;
}

/** Resolve --chip, or explain why a command cannot go on without it. */
static const wl_chip_t *require_chip(const struct options *opts) {
	const wl_chip_t *chip;

	if (opts->chip == NULL) {
		wl_error(
		    "this command needs --chip, because the part decides the "
		    "debug clock and the flash algorithm");
		wl_error("try `%s chips` for the names", program_name);
		return NULL;
	}

	chip = wl_chip_by_name(opts->chip);

	if (chip == NULL) {
		wl_error("unknown part '%s' (try `%s chips`)", opts->chip,
			 program_name);
		return NULL;
	}

	return chip;
}

/** Read a whole file into memory.  The caller frees it. */
static enum wl_status
read_file(const char *path, uint8_t **data, size_t *length) {
	struct stat st;
	FILE *file;
	uint8_t *buffer;
	size_t got;

	if (stat(path, &st) != 0) {
		wl_error("cannot open '%s': %s", path, strerror(errno));
		return WL_ERR_USAGE;
	}

	if (!S_ISREG(st.st_mode)) {
		wl_error("'%s' is not a regular file", path);
		return WL_ERR_USAGE;
	}

	if (st.st_size == 0) {
		wl_error("'%s' is empty", path);
		return WL_ERR_USAGE;
	}

	buffer = malloc((size_t)st.st_size);

	if (buffer == NULL) {
		wl_error("cannot allocate %lld bytes for '%s'",
			 (long long)st.st_size, path);
		return WL_ERR_USAGE;
	}

	file = fopen(path, "rb");

	if (file == NULL) {
		wl_error("cannot open '%s': %s", path, strerror(errno));
		free(buffer);
		return WL_ERR_USAGE;
	}

	got = fread(buffer, 1, (size_t)st.st_size, file);
	fclose(file);

	if (got != (size_t)st.st_size) {
		wl_error("short read on '%s': got %zu of %lld bytes", path, got,
			 (long long)st.st_size);
		free(buffer);
		return WL_ERR_USAGE;
	}

	*data = buffer;
	*length = got;

	return WL_OK;
}

/** Write a buffer to a file. */
static enum wl_status
write_file(const char *path, const uint8_t *data, size_t length) {
	FILE *file = fopen(path, "wb");

	if (file == NULL) {
		wl_error("cannot create '%s': %s", path, strerror(errno));
		return WL_ERR_USAGE;
	}

	if (fwrite(data, 1, length, file) != length) {
		wl_error("short write on '%s': %s", path, strerror(errno));
		fclose(file);
		return WL_ERR_USAGE;
	}

	if (fclose(file) != 0) {
		wl_error("cannot finish writing '%s': %s", path,
			 strerror(errno));
		return WL_ERR_USAGE;
	}

	return WL_OK;
}

/**
 * Print how far the write has got, on one line.
 *
 * At most one line per percent, so that a slow USB link does not spend its
 * time printing, and nothing at all when the user asked for quiet.
 */
static void show_progress(size_t done, size_t total) {
	static int last_percent = -1;
	int percent;

	if (wl_log_level() <= WL_LEVEL_QUIET || total == 0) {
		return;
	}

	percent = (int)((done * 100u) / total);

	if (percent == last_percent) {
		return;
	}

	last_percent = percent;

	if (done >= total) {
		fprintf(stderr, "\r  written: 100%% (of %zu bytes)\n", total);
	} else {
		fprintf(stderr, "\r  writing: %3d%%", percent);
	}

	fflush(stderr);
}

static enum wl_status cmd_flash(const struct options *opts) {
	const wl_chip_t *chip = NULL;
	uint8_t *image = NULL;
	size_t length = 0;
	uint32_t address;
	wl_linke_t link;
	enum wl_status status;

	if (opts->chip != NULL) {
		chip = require_chip(opts);

		if (chip == NULL) {
			return WL_ERR_USAGE;
		}

		if (!wl_linke_can_flash(chip)) {
			wl_error("flashing %s is not implemented in this build",
				 chip->name);
			wl_error(
			    "only the CH32V00x family has a flash sequence "
			    "here; the other families need one that has "
			    "been checked against a part");
			return WL_ERR_NOT_IMPLEMENTED;
		}
	}

	status = read_file(opts->file, &image, &length);

	if (status != WL_OK) {
		return status;
	}

	if (chip != NULL) {
		address =
		    opts->address_given ? opts->address : chip->flash_base;

		/*
 * Check the request against the part before opening anything:
 * an image that cannot fit is the command line's problem, and
 * saying so should not depend on a programmer being plugged
 * in.
 */
		if (!wl_flash_range_ok(chip, address, length)) {
			wl_error("0x%08x + %zu bytes does not fit %s's %uK of "
				 "flash",
				 address, length, chip->name,
				 chip->flash_size / 1024u);
			free(image);
			return WL_ERR_USAGE;
		}
	}

	status = wl_linke_open(&link, opts->serial);

	if (status != WL_OK) {
		free(image);
		return status;
	}

	if (chip == NULL) {
		status = wl_linke_identify_chip(&link, &chip);

		if (status != WL_OK) {
			wl_error("cannot auto-detect the target; connect it, "
				 "or pass --chip");
			wl_linke_close(&link);
			free(image);
			return status;
		}

		wl_info("detected target %s", chip->name);
	}

	if (!wl_linke_can_flash(chip)) {
		wl_error("flashing %s is not implemented in this build",
			 chip->name);
		wl_error("only the CH32V00x family has a flash sequence here; "
			 "the other families need one that has been checked "
			 "against a part");
		wl_linke_close(&link);
		free(image);
		return WL_ERR_NOT_IMPLEMENTED;
	}

	address = opts->address_given ? opts->address : chip->flash_base;

	if (!wl_flash_range_ok(chip, address, length)) {
		wl_error("0x%08x + %zu bytes does not fit %s's %uK of flash",
			 address, length, chip->name, chip->flash_size / 1024u);
		wl_linke_close(&link);
		free(image);
		return WL_ERR_USAGE;
	}

	wl_info("writing %zu bytes to %s at 0x%08x%s", length, chip->name,
		address, opts->verify ? ", then reading it back" : "");

	status = wl_linke_write_flash(&link, chip, address, image, length,
				      opts->verify, show_progress);

	wl_linke_close(&link);
	free(image);

	if (status == WL_OK) {
		wl_info("done: %s now holds the image", chip->name);
	}

	return status;
}

static enum wl_status cmd_read(const struct options *opts) {
	const wl_chip_t *chip = NULL;
	bool whole_flash = !opts->address_given && !opts->size_given;
	uint32_t address = 0;
	size_t size = 0;
	uint8_t *buffer;
	wl_linke_t link;
	enum wl_status status;

	/*
 * Neither option means "dump the whole flash".  One without the other
 * is ambiguous, so reject it rather than guessing at a range.
 */
	if (!whole_flash &&
	    (!opts->address_given || !opts->size_given || opts->size == 0)) {
		wl_error("read needs both --address and --size, or neither to "
			 "read the whole flash");
		return WL_ERR_USAGE;
	}

	if (opts->chip != NULL) {
		chip = require_chip(opts);

		if (chip == NULL) {
			return WL_ERR_USAGE;
		}
	}

	status = wl_linke_open(&link, opts->serial);

	if (status != WL_OK) {
		return status;
	}

	if (chip == NULL) {
		status = wl_linke_identify_chip(&link, &chip);

		if (status != WL_OK) {
			wl_error("cannot auto-detect the target; connect it, "
				 "or pass --chip");
			wl_linke_close(&link);
			return status;
		}

		wl_info("detected target %s", chip->name);
	}

	if (whole_flash) {
		address = chip->flash_base;
		size = chip->flash_size;
	} else {
		address = opts->address;
		size = opts->size;
	}

	buffer = malloc(size);

	if (buffer == NULL) {
		wl_error("cannot allocate %zu bytes", size);
		wl_linke_close(&link);
		return WL_ERR_USAGE;
	}

	wl_info("reading %zu bytes from 0x%08x", size, address);

	status = wl_linke_read_memory(&link, chip, address, buffer, size);

	wl_linke_close(&link);

	if (status == WL_OK) {
		status = write_file(opts->file, buffer, size);
	}

	free(buffer);

	return status;
}

static enum wl_status cmd_reset(const struct options *opts) {
	wl_linke_t link;
	enum wl_status status = wl_linke_open(&link, opts->serial);

	if (status != WL_OK) {
		return status;
	}

	status = wl_linke_reset(&link);

	wl_linke_close(&link);

	if (status == WL_OK) {
		wl_info("target reset");
	}

	return status;
}

static enum wl_status cmd_unbrick(const struct options *opts) {
	wl_linke_t link;
	enum wl_status status = wl_linke_open(&link, opts->serial);

	if (status != WL_OK) {
		return status;
	}

	status = wl_linke_unbrick(&link);

	wl_linke_close(&link);

	return status;
}

static enum wl_status cmd_programmer_info(const struct options *opts) {
	wl_usb_info_t info;
	enum wl_status status;

	status = wl_programmer_find(opts->serial, &info);

	if (status != WL_OK) {
		return status;
	}

	print_programmer_info(&info);
	print_programmer_version(&info);

	return WL_OK;
}

static enum wl_status cmd_programmer_list(const struct options *opts) {
	wl_usb_info_t found[WL_MAX_PROGRAMMERS];
	size_t count = 0;
	size_t i;
	int rc;

	(void)opts;

	rc = wl_usb_scan(found, WL_MAX_PROGRAMMERS, &count);

	if (rc < 0) {
		wl_error("cannot enumerate USB devices: %s",
			 wl_usb_strerror(rc));
		return WL_ERR_USB;
	}

	if (count == 0) {
		wl_error("no WCH programmer found on the USB bus");
		return WL_ERR_NO_DEVICE;
	}

	printf("%zu programmer(s):\n", count);

	for (i = 0; i < count; i++) {
		printf("\n[%zu] %s\n", i + 1,
		       found[i].accessible ? "accessible" : "not accessible");
		print_programmer_info(&found[i]);
	}

	return WL_OK;
}

static enum wl_status cmd_programmer_rv(const struct options *opts) {
	wl_usb_info_t info;
	enum wl_status status;

	status = wl_programmer_switch_rv(opts->serial, &info);

	if (status != WL_OK) {
		return status;
	}

	wl_info("programmer is in RISC-V debug mode");
	print_programmer_info(&info);

	return WL_OK;
}

static enum wl_status cmd_programmer_iap(const struct options *opts) {
	wl_usb_info_t info;
	enum wl_status status;

	status = wl_programmer_eject_iap(opts->serial, &info);

	if (status != WL_OK) {
		return status;
	}

	wl_info("programmer is in RISC-V debug mode");
	print_programmer_info(&info);

	return WL_OK;
}

/**
 * Programmer subcommands.  The first non-option argument after `programmer`
 * is stored in opts->file by the common parser; no other command uses that
 * slot for a subcommand.
 */
static enum wl_status cmd_programmer(const struct options *opts) {
	const char *sub = opts->file != NULL ? opts->file : "info";

	if (strcmp(sub, "info") == 0) {
		return cmd_programmer_info(opts);
	}

	if (strcmp(sub, "list") == 0) {
		return cmd_programmer_list(opts);
	}

	if (strcmp(sub, "rv") == 0) {
		return cmd_programmer_rv(opts);
	}

	if (strcmp(sub, "iap") == 0) {
		return cmd_programmer_iap(opts);
	}

	wl_error("unknown programmer subcommand '%s'", sub);
	wl_error("expected one of: info, list, rv, iap");

	return WL_ERR_USAGE;
}

/** Open the target path and leave the chosen core halted for DMI access. */
static enum wl_status open_target_debug(const struct options *opts,
					wl_linke_t *link,
					const wl_chip_t **chip) {
	const wl_chip_t *resolved = NULL;
	enum wl_status status;

	if (opts->chip != NULL) {
		resolved = require_chip(opts);

		if (resolved == NULL) {
			return WL_ERR_USAGE;
		}
	}

	status = wl_linke_open(link, opts->serial);

	if (status != WL_OK) {
		return status;
	}

	if (resolved == NULL) {
		status = wl_linke_identify_chip(link, &resolved);

		if (status != WL_OK) {
			wl_error("cannot auto-detect the target; connect it, "
				 "or pass --chip");
			wl_linke_close(link);
			return status;
		}

		wl_info("detected target %s", resolved->name);
	}

	status = wl_linke_set_interface(link, resolved);

	if (status != WL_OK) {
		wl_linke_close(link);
		return status;
	}

	status = wl_linke_halt(link);

	if (status != WL_OK) {
		wl_linke_close(link);
		return status;
	}

	*chip = resolved;

	return WL_OK;
}

static enum wl_status cmd_target_halt(const struct options *opts) {
	wl_linke_t link;
	enum wl_status status = wl_linke_open(&link, opts->serial);

	if (status != WL_OK) {
		return status;
	}

	status = wl_linke_halt(&link);

	wl_linke_close_keep_target(&link);

	if (status == WL_OK) {
		wl_info("target halted");
	}

	return status;
}

static enum wl_status cmd_target_resume(const struct options *opts) {
	wl_linke_t link;
	enum wl_status status = wl_linke_open(&link, opts->serial);

	if (status != WL_OK) {
		return status;
	}

	status = wl_linke_resume(&link);

	wl_linke_close(&link);

	if (status == WL_OK) {
		wl_info("target resumed");
	}

	return status;
}

static enum wl_status cmd_target_reset(const struct options *opts) {
	wl_linke_t link;
	enum wl_status status = wl_linke_open(&link, opts->serial);

	if (status != WL_OK) {
		return status;
	}

	status = wl_linke_reset(&link);

	wl_linke_close(&link);

	if (status == WL_OK) {
		wl_info("target reset");
	}

	return status;
}

static enum wl_status cmd_target_pc(const struct options *opts) {
	wl_linke_t link;
	const wl_chip_t *chip = NULL;
	uint32_t pc = 0;
	enum wl_status status = open_target_debug(opts, &link, &chip);

	if (status != WL_OK) {
		return status;
	}

	status = wl_dm_pc_read(&link, &pc);

	wl_linke_close_keep_target(&link);

	if (status == WL_OK) {
		printf("pc  = 0x%08x\n", pc);
	}

	return status;
}

static enum wl_status cmd_target_regs(const struct options *opts) {
	wl_linke_t link;
	const wl_chip_t *chip = NULL;
	uint32_t pc = 0;
	unsigned reg;
	enum wl_status status = open_target_debug(opts, &link, &chip);

	if (status != WL_OK) {
		return status;
	}

	status = wl_dm_pc_read(&link, &pc);

	if (status != WL_OK) {
		wl_linke_close_keep_target(&link);
		return status;
	}

	printf("pc  = 0x%08x\n", pc);

	for (reg = 0; reg < 32; reg++) {
		uint32_t value = 0;

		status = wl_dm_gpr_read(&link, reg, &value);

		if (status != WL_OK) {
			wl_linke_close_keep_target(&link);
			return status;
		}

		printf("x%-2u = 0x%08x\n", reg, value);
	}

	wl_linke_close_keep_target(&link);

	return WL_OK;
}

static enum wl_status cmd_target_read32(const struct options *opts) {
	wl_linke_t link;
	const wl_chip_t *chip = NULL;
	uint32_t address;
	uint32_t value = 0;
	enum wl_status status;

	if (opts->extra_count < 1 || !parse_u32(opts->extra[0], &address)) {
		wl_error("target read32 needs an address, e.g. "
			 "`target read32 0x08000000`");
		return WL_ERR_USAGE;
	}

	status = open_target_debug(opts, &link, &chip);

	if (status != WL_OK) {
		return status;
	}

	status = wl_dm_read32(&link, address, &value);

	wl_linke_close_keep_target(&link);

	if (status == WL_OK) {
		printf("0x%08x = 0x%08x\n", address, value);
	}

	return status;
}

static enum wl_status cmd_target_write32(const struct options *opts) {
	wl_linke_t link;
	const wl_chip_t *chip = NULL;
	uint32_t address;
	uint32_t value;
	enum wl_status status;

	if (opts->extra_count < 2 || !parse_u32(opts->extra[0], &address) ||
	    !parse_u32(opts->extra[1], &value)) {
		wl_error("target write32 needs address and value, e.g. "
			 "`target write32 0x20000000 0x12345678`");
		return WL_ERR_USAGE;
	}

	status = open_target_debug(opts, &link, &chip);

	if (status != WL_OK) {
		return status;
	}

	status = wl_dm_write32(&link, address, value);

	wl_linke_close_keep_target(&link);

	if (status == WL_OK) {
		printf("0x%08x <- 0x%08x\n", address, value);
	}

	return status;
}

/** Target debug subcommands.  The subcommand is opts->file, as for
 * `programmer`; read32/write32 take their remaining arguments from
 * opts->extra[]. */
static enum wl_status cmd_target(const struct options *opts) {
	const char *sub = opts->file;

	if (sub == NULL) {
		wl_error("target needs a subcommand: halt, resume, reset, pc, "
			 "regs, read32, write32");
		return WL_ERR_USAGE;
	}

	if (strcmp(sub, "halt") == 0) {
		return cmd_target_halt(opts);
	}

	if (strcmp(sub, "resume") == 0) {
		return cmd_target_resume(opts);
	}

	if (strcmp(sub, "reset") == 0) {
		return cmd_target_reset(opts);
	}

	if (strcmp(sub, "pc") == 0) {
		return cmd_target_pc(opts);
	}

	if (strcmp(sub, "regs") == 0) {
		return cmd_target_regs(opts);
	}

	if (strcmp(sub, "read32") == 0) {
		return cmd_target_read32(opts);
	}

	if (strcmp(sub, "write32") == 0) {
		return cmd_target_write32(opts);
	}

	wl_error("unknown target subcommand '%s'", sub);
	wl_error("expected one of: halt, resume, reset, pc, regs, read32, "
		 "write32");

	return WL_ERR_USAGE;
}

/* --- command line ------------------------------------------------------- */

static int status_to_exit(enum wl_status status) {
	switch (status) {
	case WL_OK:
		return EXIT_SUCCESS;
	case WL_ERR_USAGE:
		return 2;
	case WL_ERR_NO_DEVICE:
	case WL_ERR_ACCESS:
		return 3;
	default:
		return 1;
	}
}

int main(int argc, char **argv) {
	struct options opts;
	const char *command = NULL;
	enum wl_status status = WL_OK;
	int i;

	memset(&opts, 0, sizeof(opts));
	opts.verify = true;

	if (argv[0] != NULL && argv[0][0] != '\0') {
		const char *slash = strrchr(argv[0], '/');

		program_name = slash != NULL ? slash + 1 : argv[0];
	}

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
			usage(stdout);
			return EXIT_SUCCESS;
		}

		if (strcmp(arg, "--version") == 0) {
			printf("%s %s\n", program_name, WL_VERSION);
			return EXIT_SUCCESS;
		}

		if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
			wl_log_set_level(WL_LEVEL_DEBUG);
			continue;
		}

		if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
			wl_log_set_level(WL_LEVEL_QUIET);
			continue;
		}

		if (strcmp(arg, "--verify") == 0) {
			opts.verify = true;
			continue;
		}

		if (strcmp(arg, "--no-verify") == 0) {
			opts.verify = false;
			continue;
		}

		if (strcmp(arg, "-s") == 0 || strcmp(arg, "--serial") == 0) {
			if (++i >= argc) {
				wl_error("%s needs an argument", arg);
				return 2;
			}

			opts.serial = argv[i];
			continue;
		}

		if (strcmp(arg, "-c") == 0 || strcmp(arg, "--chip") == 0) {
			if (++i >= argc) {
				wl_error("%s needs an argument", arg);
				return 2;
			}

			opts.chip = argv[i];
			continue;
		}

		if (strcmp(arg, "-a") == 0 || strcmp(arg, "--address") == 0) {
			if (++i >= argc || !parse_u32(argv[i], &opts.address)) {
				wl_error("%s needs a number", arg);
				return 2;
			}

			opts.address_given = true;
			continue;
		}

		if (strcmp(arg, "-n") == 0 || strcmp(arg, "--size") == 0) {
			uint32_t value;

			if (++i >= argc || !parse_u32(argv[i], &value)) {
				wl_error("%s needs a number", arg);
				return 2;
			}

			opts.size = (size_t)value;
			opts.size_given = true;
			continue;
		}

		if (arg[0] == '-' && arg[1] != '\0') {
			wl_error("unknown option '%s'", arg);
			usage(stderr);
			return 2;
		}

		if (command == NULL) {
			command = arg;
			continue;
		}

		if (opts.file == NULL) {
			opts.file = arg;
			continue;
		}

		if (strcmp(command, "target") == 0 &&
		    opts.extra_count < WL_TARGET_EXTRA_MAX) {
			opts.extra[opts.extra_count++] = arg;
			continue;
		}

		wl_error("unexpected argument '%s'", arg);
		return 2;
	}

	if (command == NULL) {
		usage(stderr);
		return 2;
	}

	/* At debug level only: a banner on every command is just noise. */
	wl_debug("%s %s", program_name, WL_VERSION);

	/*
	 * Everything below may touch USB, so bring libusb up first.  Scanning
	 * before this point would fail with LIBUSB_ERROR_NOT_INITIALIZED and
	 * report "no programmer found", which is true but useless.
	 */
	{
		int rc = wl_usb_global_init();

		if (rc < 0) {
			wl_error("cannot initialise libusb: %s",
				 wl_usb_strerror(rc));
			return 1;
		}
	}

	if (strcmp(command, "chips") == 0) {
		status = cmd_chips();
	} else if (strcmp(command, "info") == 0) {
		status = cmd_info(&opts);
	} else if (strcmp(command, "flash") == 0) {
		if (opts.file == NULL) {
			wl_error("flash needs a file argument");
			status = WL_ERR_USAGE;
		} else {
			status = cmd_flash(&opts);
		}
	} else if (strcmp(command, "read") == 0) {
		if (opts.file == NULL) {
			wl_error("read needs a file argument");
			status = WL_ERR_USAGE;
		} else {
			status = cmd_read(&opts);
		}
	} else if (strcmp(command, "programmer") == 0) {
		status = cmd_programmer(&opts);
	} else if (strcmp(command, "target") == 0) {
		status = cmd_target(&opts);
	} else if (strcmp(command, "reset") == 0) {
		status = cmd_reset(&opts);
	} else if (strcmp(command, "unbrick") == 0) {
		status = cmd_unbrick(&opts);
	} else if (strcmp(command, "terminal") == 0) {
		wl_error("the single-wire terminal is not implemented yet "
			 "(milestone 4)");
		status = WL_ERR_NOT_IMPLEMENTED;
	} else {
		wl_error("unknown command '%s'", command);
		usage(stderr);
		status = WL_ERR_USAGE;
	}

	wl_usb_global_exit();

	if (status != WL_OK) {
		wl_debug("exiting with %s", wl_status_str(status));
	}

	return status_to_exit(status);
}
