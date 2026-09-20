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
 * Milestone 1 implements `info` and `chips`, which need only USB discovery.
 * The flashing commands are wired up, argument-checked, and then report that
 * they are not implemented yet: the protocol is milestone 2 and 3 work, and it
 * cannot be written honestly without a programmer to test against.
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

#include "linke.h"
#include "log.h"
#include "target.h"
#include "usb.h"

#define WL_VERSION "0.1.0"

static const char *program_name = "wchlink";

/** Where the tool is in its development.  Printed by --help and info. */
static const char *const milestone_note =
    "milestone 1: device discovery only; flashing arrives in milestone 3";

struct options {
	const char *serial;
	const char *chip;
	const char *file;
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
	    "  info                 report the programmer and, with --chip, "
	    "the target\n"
	    "  chips                list the parts this tool knows about\n"
	    "  flash <file.bin>     write a binary image to flash (milestone "
	    "3)\n"
	    "  read  <file.bin>     read target memory back (milestone 2)\n"
	    "  reset                reset the target (milestone 3)\n"
	    "  unbrick              clear all code flash by power-cycling "
	    "(milestone 3)\n"
	    "  terminal             single-wire debug terminal (milestone 4)\n"
	    "\n"
	    "Options:\n"
	    "  -s, --serial <sn>    pick a programmer by serial number\n"
	    "  -c, --chip <part>    target part, e.g. ch32v003 or ch582\n"
	    "  -a, --address <addr> address, decimal or 0x-prefixed\n"
	    "  -n, --size <bytes>   number of bytes to read\n"
	    "      --verify         read the image back and compare (flash)\n"
	    "  -v, --verbose        show what is happening\n"
	    "  -q, --quiet          errors only\n"
	    "  -h, --help           this text\n"
	    "      --version        version and milestone\n"
	    "\n"
	    "NOTE: %s\n",
	    program_name, milestone_note);
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
	wl_linke_t link;
	enum wl_status status;
	char version[64];
	size_t count = 0;

	(void)wl_chip_all(&count);

	if (opts->chip != NULL) {
		const wl_chip_t *chip = wl_chip_by_name(opts->chip);

		if (chip == NULL) {
			wl_error("unknown part '%s' (try `%s chips`)",
				 opts->chip, program_name);
			return WL_ERR_USAGE;
		}

		printf("target: %s (%s)\n", chip->name, chip->family);
		printf("  flash:  %u bytes (%uK)\n", chip->flash_size,
		       chip->flash_size / 1024u);
		printf("  ram:    %u bytes at 0x%08x\n", chip->ram_size,
		       chip->ram_offset);
	} else {
		printf("target: not specified (pass --chip; %zu parts known)\n",
		       count);
	}

	printf("\n");

	status = wl_linke_open(&link, opts->serial);

	if (status != WL_OK) {
		return status;
	}

	printf("programmer:\n");
	printf("  usb:    %04x:%04x at bus %u address %u\n", link.usb.vid,
	       link.usb.pid, link.usb.bus, link.usb.address);
	printf("  mode:   %s\n", wl_usb_mode_name(link.usb.mode));
	printf("  serial: %s\n",
	       link.usb.serial[0] != '\0' ? link.usb.serial : "(not reported)");
	printf("  product: %s\n", link.usb.product[0] != '\0'
				      ? link.usb.product
				      : "(not reported)");

	memset(version, 0, sizeof(version));
	status = wl_linke_get_version(&link, version, sizeof(version));

	if (status == WL_OK) {
		printf("  version: %s\n", version);
	} else {
		printf("  version: %s\n", wl_status_str(status));
	}

	printf("\n%s\n", milestone_note);

	wl_linke_close(&link);

	return WL_OK;
}

static bool check_file_readable(const struct options *opts) {
	struct stat st;

	if (opts->file == NULL) {
		wl_error("this command needs a file argument");
		return false;
	}

	if (stat(opts->file, &st) != 0) {
		wl_error("cannot open '%s': %s", opts->file, strerror(errno));
		return false;
	}

	if (!S_ISREG(st.st_mode)) {
		wl_error("'%s' is not a regular file", opts->file);
		return false;
	}

	return true;
}

static enum wl_status cmd_not_yet(const struct options *opts,
				  const char *what) {
	wl_linke_t link;
	enum wl_status status;

	/*
	 * Validate everything that can be validated without the protocol, so
	 * the command line a user writes today keeps working once the protocol
	 * lands.
	 */
	if (opts->chip != NULL && wl_chip_by_name(opts->chip) == NULL) {
		wl_error("unknown part '%s' (try `%s chips`)", opts->chip,
			 program_name);
		return WL_ERR_USAGE;
	}

	status = wl_linke_open(&link, opts->serial);

	if (status != WL_OK) {
		return status;
	}

	wl_linke_close(&link);

	wl_error("%s is not implemented yet (%s)", what, milestone_note);

	return WL_ERR_NOT_IMPLEMENTED;
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
			printf("%s %s -- %s\n", program_name, WL_VERSION,
			       milestone_note);
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
		status = check_file_readable(&opts)
			     ? cmd_not_yet(&opts, "flash")
			     : WL_ERR_USAGE;
	} else if (strcmp(command, "read") == 0) {
		if (!check_file_readable(&opts)) {
			status = WL_ERR_USAGE;
		} else if (!opts.address_given || !opts.size_given ||
			   opts.size == 0) {
			wl_error("read needs --address and --size");
			status = WL_ERR_USAGE;
		} else {
			status = cmd_not_yet(&opts, "read");
		}
	} else if (strcmp(command, "reset") == 0) {
		status = cmd_not_yet(&opts, "reset");
	} else if (strcmp(command, "unbrick") == 0) {
		status = cmd_not_yet(&opts, "unbrick");
	} else if (strcmp(command, "terminal") == 0) {
		status = cmd_not_yet(&opts, "terminal");
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
