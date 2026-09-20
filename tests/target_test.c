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
 * Unit tests for the chip table.
 *
 * The table is the one part of this tool that can be wrong in a way that
 * silently destroys data: too small a flash size and the flasher writes past
 * the end of the part.  So it is worth testing the lookup rules properly,
 * especially the prefix matching, which is where a part could be resolved to
 * the wrong entry.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "target.h"

static int failures;
static int checks;

static void check(bool ok, const char *what) {
	checks++;

	if (!ok) {
		failures++;
		printf("  FAIL  %s\n", what);
	}
}

static void check_str(const char *got, const char *want, const char *what) {
	checks++;

	if (got == NULL || strcmp(got, want) != 0) {
		failures++;
		printf("  FAIL  %s: got %s, want %s\n", what,
		       got != NULL ? got : "(null)", want);
	}
}

static void test_table_is_sane(void) {
	size_t count = 0;
	const wl_chip_t *chips = wl_chip_all(&count);
	size_t i;
	size_t j;

	check(count > 0, "table is not empty");
	check(chips != NULL, "table pointer is not NULL");

	for (i = 0; i < count; i++) {
		check(chips[i].name != NULL && chips[i].name[0] != '\0',
		      "every entry has a name");
		check(chips[i].family != NULL && chips[i].family[0] != '\0',
		      "every entry has a family");
		check(chips[i].flash_size > 0, "every entry has flash");
		check(chips[i].ram_size > 0, "every entry has RAM");

		/* Names must be unique, or lookup becomes ambiguous. */
		for (j = i + 1; j < count; j++) {
			if (strcmp(chips[i].name, chips[j].name) == 0) {
				printf("  FAIL  duplicate name %s\n",
				       chips[i].name);
				failures++;
			}
		}
	}
}

static void test_exact_lookup(void) {
	const wl_chip_t *chip = wl_chip_by_name("ch32v003");

	check(chip != NULL, "ch32v003 is found");
	check_str(chip != NULL ? chip->name : NULL, "ch32v003",
		  "ch32v003 resolves to itself");
	check_str(chip != NULL ? chip->family : NULL, "ch32v0",
		  "ch32v003 is in the ch32v0 family");
	check(chip != NULL && chip->flash_size == 16u * 1024u,
	      "ch32v003 has 16K flash");
}

static void test_prefix_lookup(void) {
	/* The full order code must resolve to the base part. */
	const wl_chip_t *chip = wl_chip_by_name("ch32v003f4p6");

	check(chip != NULL, "a full order code is recognised");
	check_str(chip != NULL ? chip->name : NULL, "ch32v003",
		  "ch32v003f4p6 resolves to ch32v003");
}

static void test_case_insensitive(void) {
	const wl_chip_t *lower = wl_chip_by_name("ch32v003");
	const wl_chip_t *upper = wl_chip_by_name("CH32V003");

	check(lower != NULL && lower == upper, "lookup ignores case");
}

static void test_longest_match_wins(void) {
	/*
	 * ch32v004 and ch32v005 share the ch32v00 prefix, and ch32v002 does
	 * not prefix ch32v003.  The interesting case is a name that is a
	 * prefix of another: ch32v00 is not a part, so it must not match, and
	 * ch32v002 must not be shadowed by anything shorter.
	 */
	check(wl_chip_by_name("ch32v00") == NULL,
	      "ch32v00 is not a part and matches nothing");
	check(wl_chip_by_name("ch32v1") == NULL,
	      "ch32v1 is not a part and matches nothing");

	check_str(wl_chip_by_name("ch32v002")
		      ? wl_chip_by_name("ch32v002")->name
		      : NULL,
		  "ch32v002", "ch32v002 resolves to itself");
	check_str(wl_chip_by_name("ch582m") ? wl_chip_by_name("ch582m")->name
					    : NULL,
		  "ch582", "ch582m resolves to ch582");
}

static void test_unknown(void) {
	check(wl_chip_by_name("stm32f103") == NULL,
	      "a foreign part matches nothing");
	check(wl_chip_by_name("") == NULL, "an empty name matches nothing");
	check(wl_chip_by_name(NULL) == NULL, "a NULL name matches nothing");
}

static void test_same(void) {
	check(wl_chip_same("ch32v003", "ch32v003f4p6"),
	      "base name and order code are the same part");
	check(!wl_chip_same("ch32v003", "ch32v002"),
	      "different parts are not the same");
	check(!wl_chip_same("ch32v003", "nonsense"),
	      "an unknown part is never the same as a known one");
}

static void test_name_max(void) {
	size_t max = wl_chip_name_max();
	size_t count = 0;
	const wl_chip_t *chips = wl_chip_all(&count);
	size_t i;

	for (i = 0; i < count; i++) {
		check(strlen(chips[i].name) <= max,
		      "no name is longer than the reported maximum");
	}
}

int main(void) {
	printf("target table:\n");

	test_table_is_sane();
	test_exact_lookup();
	test_prefix_lookup();
	test_case_insensitive();
	test_longest_match_wins();
	test_unknown();
	test_same();
	test_name_max();

	printf("  %d checks, %d failure(s)\n", checks, failures);

	return failures == 0 ? 0 : 1;
}
