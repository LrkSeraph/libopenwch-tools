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

#ifndef WCHLINK_TESTS_SIM_PROGRAMMER_H
#define WCHLINK_TESTS_SIM_PROGRAMMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "linke.h"
#include "target.h"
#include "transport.h"

/*
 * A simulated WCH-LinkE with a simulated target on the other end.
 *
 * It answers the same packets a real programmer does, interprets the programs
 * the tool loads into the target's debug program buffer, and implements the
 * CH32V00x flash controller well enough to tell a correct write sequence from
 * an incorrect one:
 *
 *   - the controller starts locked and only unlocks on the two magic keys in
 *     the right order;
 *   - an erase clears exactly the page the algorithm asked for;
 *   - a program can only clear bits, so programming a location that was not
 *     erased first is remembered as an error rather than silently accepted;
 *   - BSY stays set for a couple of status reads, so the polling loop is
 *     exercised rather than skipped.
 *
 * This is not a substitute for hardware -- it cannot be, because the protocol
 * itself is modelled from the same understanding as the code under test.  It
 * is what catches the mistakes that are not about the protocol: a wrong
 * address, a missed erase, a halfword in the wrong byte order, an unaligned
 * write that clobbers its neighbours, or an off-by-one at the end of the
 * image.
 */

struct wl_sim;

/** Create a simulator for @p chip.  Returns NULL if it cannot be built. */
struct wl_sim *wl_sim_new(const wl_chip_t *chip);

/** Destroy a simulator. */
void wl_sim_free(struct wl_sim *sim);

/** The transport that talks to this simulator. */
const struct wl_transport *wl_sim_transport(void);

/** The context to hand to wl_linke_attach_transport(). */
void *wl_sim_context(struct wl_sim *sim);

/** A pointer to the simulated flash array, flash_size bytes long. */
const uint8_t *wl_sim_flash(const struct wl_sim *sim);

/** Fill part of the simulated flash, to set up a test. */
void wl_sim_flash_fill(struct wl_sim *sim,
		       uint32_t offset,
		       uint8_t value,
		       size_t length);

/** Change one byte of flash behind the tool's back, to test verification. */
void wl_sim_flash_poke(struct wl_sim *sim, uint32_t offset, uint8_t value);

/** How many times the tool has driven the target reset line. */
unsigned wl_sim_reset_count(const struct wl_sim *sim);

/** How many pages the tool has erased. */
unsigned wl_sim_erase_count(const struct wl_sim *sim);

/** How many halfwords the tool has programmed. */
unsigned wl_sim_program_count(const struct wl_sim *sim);

/**
 * How many times a program write asked flash to turn a 0 bit back into a 1.
 *
 * Zero for a correct sequence: it means every programmed location had been
 * erased.  A non-zero value means the tool wrote over data it had not cleared.
 */
unsigned wl_sim_program_errors(const struct wl_sim *sim);

/** How many times the program buffer held an instruction the simulator does
 * not know.  Zero for a correct tool: it means every program it built was
 * valid code. */
unsigned wl_sim_illegal_instructions(const struct wl_sim *sim);

/** Whether the target is being held halted by the programmer. */
bool wl_sim_halted(const struct wl_sim *sim);

/**
 * Emulate LinkE firmware that refuses a second attach while already
 * attached, which is what makes auto-detection fail after a debug command
 * left the target halted.  Release must clear the condition.
 */
void wl_sim_set_fail_attach_while_attached(struct wl_sim *sim, bool fail);

#endif /* WCHLINK_TESTS_SIM_PROGRAMMER_H */
