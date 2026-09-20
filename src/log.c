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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

#define WL_PROGRAM "wchlink"

static enum wl_level threshold = WL_LEVEL_INFO;

void wl_log_set_level(enum wl_level level) {
	threshold = level;
}

enum wl_level wl_log_level(void) {
	return threshold;
}

static void
vreport(enum wl_level level, const char *prefix, const char *fmt, va_list ap) {
	if (level > threshold) {
		return;
	}

	fprintf(stderr, "%s: %s", WL_PROGRAM, prefix);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
}

void wl_error(const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	vreport(WL_LEVEL_QUIET, "error: ", fmt, ap);
	va_end(ap);
}

void wl_warn(const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	vreport(WL_LEVEL_QUIET, "warning: ", fmt, ap);
	va_end(ap);
}

void wl_info(const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	vreport(WL_LEVEL_INFO, "", fmt, ap);
	va_end(ap);
}

void wl_debug(const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	vreport(WL_LEVEL_DEBUG, "debug: ", fmt, ap);
	va_end(ap);
}

void wl_hexdump(const char *label, const void *data, unsigned int len) {
	const unsigned char *p = data;
	unsigned int i;

	if (threshold < WL_LEVEL_DEBUG) {
		return;
	}

	fprintf(stderr, "%s: debug: %s (%u bytes)\n", WL_PROGRAM, label, len);

	for (i = 0; i < len; i += 16) {
		unsigned int j;

		fprintf(stderr, "  %04x  ", i);

		for (j = 0; j < 16; j++) {
			if (i + j < len) {
				fprintf(stderr, "%02x ", p[i + j]);
			} else {
				fputs("   ", stderr);
			}

			if (j == 7) {
				fputc(' ', stderr);
			}
		}

		fputs(" |", stderr);

		for (j = 0; j < 16 && i + j < len; j++) {
			unsigned char c = p[i + j];

			fputc(c >= 0x20 && c < 0x7f ? c : '.', stderr);
		}

		fputs("|\n", stderr);
	}
}
