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

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dm.h"
#include "flash.h"
#include "linke.h"
#include "sim_programmer.h"
#include "target.h"

static unsigned failures;
static unsigned checks;

/*
 * Some tests exist to make the tool refuse something, and the tool says so on
 * stderr.  Those messages would otherwise be indistinguishable in a CI log
 * from a test that actually went wrong, so they are swallowed here and the
 * assertion carries the meaning instead.
 */
static int saved_stderr = -1;

static void stderr_off(void) {
	int devnull;

	fflush(stderr);
	saved_stderr = dup(STDERR_FILENO);
	devnull = open("/dev/null", O_WRONLY);

	if (devnull >= 0) {
		(void)dup2(devnull, STDERR_FILENO);
		close(devnull);
	}
}

static void stderr_on(void) {
	fflush(stderr);

	if (saved_stderr >= 0) {
		(void)dup2(saved_stderr, STDERR_FILENO);
		close(saved_stderr);
		saved_stderr = -1;
	}
}

#define CHECK(condition)                                                       \
	do {                                                                   \
		checks++;                                                      \
		if (!(condition)) {                                            \
			failures++;                                            \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,          \
				__LINE__, #condition);                         \
		}                                                              \
	} while (0)

#define CHECK_EQ(actual, expected)                                             \
	do {                                                                   \
		unsigned long long a_ = (unsigned long long)(actual);          \
		unsigned long long e_ = (unsigned long long)(expected);        \
		checks++;                                                      \
		if (a_ != e_) {                                                \
			failures++;                                            \
			fprintf(stderr,                                        \
				"FAIL %s:%d: %s == %s (got 0x%llx, expected "  \
				"0x%llx)\n",                                   \
				__FILE__, __LINE__, #actual, #expected, a_,    \
				e_);                                           \
		}                                                              \
	} while (0)

/* --- instruction encoding ------------------------------------------------ */

/*
 * These are the encodings the ISA gives for the instructions the memory
 * programs use.  Checking them here means a mistyped opcode fails a test
 * rather than a target: on hardware it would run something else entirely, or
 * fault the debug module with no clue as to why.
 */
static void test_encoder(void) {
	/* sw x8, 0(x9) / sh x8, 0(x9) / sb x8, 0(x9) */
	CHECK_EQ(wl_rv_store(2, 8, 9), 0x0084a023u);
	CHECK_EQ(wl_rv_store(1, 8, 9), 0x00849023u);
	CHECK_EQ(wl_rv_store(0, 8, 9), 0x00848023u);

	/* lw x8, 0(x9) / lh x8, 0(x9) / lbu x8, 0(x9) */
	CHECK_EQ(wl_rv_load(0x2, 8, 9), 0x0004a403u);
	CHECK_EQ(wl_rv_load(0x1, 8, 9), 0x00049403u);
	CHECK_EQ(wl_rv_load(0x4, 8, 9), 0x0004c403u);

	/* addi x10, x0, 5, and the -4 a polling loop would use */
	CHECK_EQ(wl_rv_addi(10, 0, 5), 0x00500513u);
	CHECK_EQ(wl_rv_addi(10, 10, -1), 0xfff50513u);

	/* bne x8, x0, -4 */
	CHECK_EQ(wl_rv_bne(8, 0, -4), 0xfe041ee3u);

	CHECK_EQ(wl_rv_ebreak(), 0x00100073u);
}

/* --- talking to a simulated programmer ----------------------------------- */

static void test_version(wl_linke_t *link) {
	char version[64];

	CHECK_EQ(wl_linke_get_version(link, version, sizeof(version)), WL_OK);
	CHECK(strncmp(version, "WCH-LinkE 2.7", 13) == 0);
}

static void test_identify(wl_linke_t *link, const wl_chip_t *chip) {
	const wl_chip_t *detected = NULL;

	CHECK_EQ(wl_linke_identify_chip(link, &detected), WL_OK);
	CHECK(detected == chip);
}

static void test_registers(wl_linke_t *link) {
	uint32_t value = 0;

	CHECK_EQ(wl_linke_dmi_write(link, WL_DMI_DATA0, 0xdeadbeefu), WL_OK);
	CHECK_EQ(wl_linke_dmi_read(link, WL_DMI_DATA0, &value), WL_OK);
	CHECK_EQ(value, 0xdeadbeefu);

	/* A register the simulator leaves alone reads back as written. */
	CHECK_EQ(wl_linke_dmi_write(link, WL_DMI_DMCONTROL, 0x00000001u),
		 WL_OK);
	CHECK_EQ(wl_linke_dmi_read(link, WL_DMI_DMCONTROL, &value), WL_OK);
	CHECK_EQ(value, 0x00000001u);
}

/* --- memory ------------------------------------------------------------- */

static void test_memory_roundtrip(wl_linke_t *link, const wl_chip_t *chip) {
	uint8_t expected[40];
	uint8_t read_back[10];
	size_t i;

	for (i = 0; i < sizeof(expected); i++) {
		expected[i] = (uint8_t)(0x11u * (i + 1u));
	}

	/* Ten words, written through the debug module's store program. */
	for (i = 0; i < 10u; i++) {
		uint32_t word = (uint32_t)expected[4u * i] |
				((uint32_t)expected[4u * i + 1u] << 8) |
				((uint32_t)expected[4u * i + 2u] << 16) |
				((uint32_t)expected[4u * i + 3u] << 24);

		CHECK_EQ(wl_dm_write32(
			     link, chip->ram_offset + 4u * (uint32_t)i, word),
			 WL_OK);
	}

	/*
	 * Read back starting one byte in, so the block reader has to handle an
	 * unaligned head, an aligned middle and a tail: three code paths that
	 * would otherwise only be exercised on a real target.
	 */
	CHECK_EQ(wl_linke_read_memory(link, chip, chip->ram_offset + 1u,
				      read_back, sizeof(read_back)),
		 WL_OK);

	for (i = 0; i < sizeof(read_back); i++) {
		CHECK_EQ(read_back[i], expected[i + 1u]);
	}
}

/* --- flashing ----------------------------------------------------------- */

static void fill_image(uint8_t *image, size_t length) {
	size_t i;

	for (i = 0; i < length; i++) {
		image[i] = (uint8_t)((i * 7u + 0x5au) & 0xffu);
	}
}

static void test_flash_image(const wl_chip_t *chip) {
	struct wl_sim *sim = wl_sim_new(chip);
	wl_linke_t link;
	uint8_t image[200];
	const uint8_t *flash;

	CHECK(sim != NULL);

	if (sim == NULL) {
		return;
	}

	fill_image(image, sizeof(image));

	wl_linke_attach_transport(&link, wl_sim_transport(),
				  wl_sim_context(sim));

	CHECK_EQ(wl_linke_write_flash(&link, chip, chip->flash_base, image,
				      sizeof(image), true, NULL),
		 WL_OK);

	flash = wl_sim_flash(sim);

	/* The image is where it was asked to go, byte for byte. */
	CHECK_EQ(memcmp(flash, image, sizeof(image)), 0);

	/* Everything past it is still erased: an off-by-one in the page loop
	 * would show up here rather than on a part. */
	CHECK_EQ(flash[sizeof(image)], 0xffu);
	CHECK_EQ(flash[sizeof(image) + 63], 0xffu);

	/* The sequence erased before it programmed, and never asked flash to
	 * set a bit back to one. */
	CHECK(wl_sim_erase_count(sim) > 0);
	CHECK_EQ(wl_sim_program_errors(sim), 0);
	CHECK_EQ(wl_sim_illegal_instructions(sim), 0);
	CHECK(!wl_sim_halted(sim));

	wl_linke_detach(&link);
	wl_sim_free(sim);
}

/** A write that covers part of a page must not disturb the rest of it. */
static void test_partial_page(const wl_chip_t *chip) {
	struct wl_sim *sim = wl_sim_new(chip);
	wl_linke_t link;
	uint8_t image[8];
	const uint8_t *flash;
	uint32_t address = chip->flash_base + 0x10u;
	size_t i;

	CHECK(sim != NULL);

	if (sim == NULL) {
		return;
	}

	/* Start from a flash that holds something, so the merge has to work. */
	wl_sim_flash_fill(sim, 0, 0xa5u, 256);

	for (i = 0; i < sizeof(image); i++) {
		image[i] = (uint8_t)(0x30u + i);
	}

	wl_linke_attach_transport(&link, wl_sim_transport(),
				  wl_sim_context(sim));

	CHECK_EQ(wl_linke_write_flash(&link, chip, address, image,
				      sizeof(image), true, NULL),
		 WL_OK);

	flash = wl_sim_flash(sim);

	/* The eight bytes are there... */
	for (i = 0; i < sizeof(image); i++) {
		CHECK_EQ(flash[0x10u + i], image[i]);
	}

	/*
	 * ...and the page around them is untouched.  This is the case an
	 * erase-then-write gets wrong if it forgets to read the page first:
	 * everything outside the image would be 0xff.
	 */
	for (i = 0; i < 0x10u; i++) {
		CHECK_EQ(flash[i], 0xa5u);
	}

	for (i = 0x10u + sizeof(image); i < 256u; i++) {
		CHECK_EQ(flash[i], 0xa5u);
	}

	CHECK_EQ(wl_sim_program_errors(sim), 0);
	CHECK_EQ(wl_sim_illegal_instructions(sim), 0);

	wl_linke_detach(&link);
	wl_sim_free(sim);
}

/** Verification has to notice a byte that did not stick. */
static void test_verify_catches_corruption(const wl_chip_t *chip) {
	struct wl_sim *sim = wl_sim_new(chip);
	wl_linke_t link;
	uint8_t image[64];

	/* The mismatch below is the point of the test, not a problem to
	 * report. */
	stderr_off();

	CHECK(sim != NULL);

	if (sim == NULL) {
		return;
	}

	fill_image(image, sizeof(image));

	wl_linke_attach_transport(&link, wl_sim_transport(),
				  wl_sim_context(sim));

	/* Write without verifying, then damage one byte and verify by hand. */
	CHECK_EQ(wl_linke_write_flash(&link, chip, chip->flash_base, image,
				      sizeof(image), false, NULL),
		 WL_OK);
	CHECK_EQ(wl_flash_verify(&link, chip, chip->flash_base, image,
				 sizeof(image)),
		 WL_OK);

	wl_sim_flash_poke(sim, 17u, (uint8_t)(image[17] ^ 0xf0u));

	CHECK_EQ(wl_flash_verify(&link, chip, chip->flash_base, image,
				 sizeof(image)),
		 WL_ERR_VERIFY);

	stderr_on();
	wl_linke_detach(&link);
	wl_sim_free(sim);
}

/** An address in the 0x00000000 alias means the same as the 0x08000000 one. */
static void test_flash_alias_address(const wl_chip_t *chip) {
	struct wl_sim *sim = wl_sim_new(chip);
	wl_linke_t link;
	uint8_t image[32];

	CHECK(sim != NULL);

	if (sim == NULL) {
		return;
	}

	fill_image(image, sizeof(image));

	wl_linke_attach_transport(&link, wl_sim_transport(),
				  wl_sim_context(sim));

	CHECK_EQ(wl_linke_write_flash(&link, chip, 0x00000000u, image,
				      sizeof(image), true, NULL),
		 WL_OK);

	CHECK_EQ(memcmp(wl_sim_flash(sim), image, sizeof(image)), 0);

	wl_linke_detach(&link);
	wl_sim_free(sim);
}

/** A part with no algorithm is refused, and refused before any USB traffic. */
static void test_unsupported_family(void) {
	const wl_chip_t *chip = wl_chip_by_name("ch582");
	struct wl_sim *sim;
	wl_linke_t link;
	uint8_t image[16];

	/* Being refused is the expected outcome. */
	stderr_off();

	CHECK(chip != NULL);
	CHECK(!wl_linke_can_flash(chip));

	if (chip == NULL) {
		return;
	}

	sim = wl_sim_new(wl_chip_by_name("ch32v003"));

	if (sim == NULL) {
		return;
	}

	wl_linke_attach_transport(&link, wl_sim_transport(),
				  wl_sim_context(sim));

	fill_image(image, sizeof(image));

	CHECK_EQ(wl_linke_write_flash(&link, chip, 0x00000000u, image,
				      sizeof(image), true, NULL),
		 WL_ERR_NOT_IMPLEMENTED);

	stderr_on();
	wl_linke_detach(&link);
	wl_sim_free(sim);
}

/** An image that does not fit the part is refused rather than truncated. */
static void test_out_of_range(const wl_chip_t *chip) {
	struct wl_sim *sim = wl_sim_new(chip);
	wl_linke_t link;
	uint8_t image[256];

	/* The refusal is the expected outcome. */
	stderr_off();

	CHECK(sim != NULL);

	if (sim == NULL) {
		return;
	}

	wl_linke_attach_transport(&link, wl_sim_transport(),
				  wl_sim_context(sim));

	fill_image(image, sizeof(image));

	CHECK_EQ(wl_linke_write_flash(&link, chip,
				      chip->flash_base + chip->flash_size - 16u,
				      image, sizeof(image), true, NULL),
		 WL_ERR_USAGE);

	CHECK_EQ(wl_sim_erase_count(sim), 0);

	stderr_on();
	wl_linke_detach(&link);
	wl_sim_free(sim);
}

int main(void) {
	const wl_chip_t *chip = wl_chip_by_name("ch32v003");
	struct wl_sim *sim;
	wl_linke_t link;

	if (chip == NULL) {
		fprintf(stderr, "the chip table has no ch32v003\n");
		return 1;
	}

	test_encoder();

	sim = wl_sim_new(chip);

	if (sim == NULL) {
		fprintf(stderr, "cannot build the simulated target\n");
		return 1;
	}

	wl_linke_attach_transport(&link, wl_sim_transport(),
				  wl_sim_context(sim));

	test_version(&link);
	test_identify(&link, chip);
	CHECK_EQ(wl_linke_set_interface(&link, chip), WL_OK);
	test_registers(&link);
	test_memory_roundtrip(&link, chip);

	wl_linke_detach(&link);
	wl_sim_free(sim);

	test_flash_image(chip);
	test_partial_page(chip);
	test_verify_catches_corruption(chip);
	test_flash_alias_address(chip);
	test_unsupported_family();
	test_out_of_range(chip);

	printf("flash_test: %u checks, %u failures\n", checks, failures);

	return failures == 0 ? 0 : 1;
}
