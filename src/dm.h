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

#ifndef WCHLINK_DM_H
#define WCHLINK_DM_H

#include <stddef.h>
#include <stdint.h>

#include "linke.h"

/*
 * The RISC-V debug module, as the QingKe cores implement it.
 *
 * These addresses are the Debug Module Interface (DMI) addresses from the
 * RISC-V Debug Specification, not WCH inventions: the WCH-LinkE's register
 * command (0x08) is a DMI access, so everything below is spec-shaped.
 */
#define WL_DMI_DATA0 0x04
#define WL_DMI_DATA1 0x05
#define WL_DMI_DMCONTROL 0x10
#define WL_DMI_DMSTATUS 0x11
#define WL_DMI_DMABSTRACTCS 0x16
#define WL_DMI_DMCOMMAND 0x17
#define WL_DMI_DMABSTRACTAUTO 0x18
#define WL_DMI_DMPROGBUF0 0x20
#define WL_DMI_DMPROGBUF1 0x21
#define WL_DMI_DMPROGBUF2 0x22
#define WL_DMI_DMPROGBUF3 0x23
#define WL_DMI_DMPROGBUF4 0x24
#define WL_DMI_DMPROGBUF5 0x25
#define WL_DMI_DMPROGBUF6 0x26
#define WL_DMI_DMPROGBUF7 0x27
#define WL_DMI_DCSR 0x7b0
#define WL_DMI_DPC 0x7b1

/** How many instruction words the program buffer holds. */
#define WL_DM_PROGBUF_WORDS 8

/**
 * Read one target general-purpose register (x0-x31).
 *
 * This is what makes memory access possible at all: the QingKe debug module
 * has no system-bus access block, so the only way to touch target memory is to
 * run a two-instruction program from the program buffer.
 */
enum wl_status wl_dm_gpr_read(wl_linke_t *link, unsigned reg, uint32_t *value);

/** Write one target general-purpose register. */
enum wl_status wl_dm_gpr_write(wl_linke_t *link, unsigned reg, uint32_t value);

/**
 * Read the halted target's program counter.
 *
 * The RISC-V debug module exposes its stopped PC through DMI register
 * WL_DMI_DPC (0x7b1).  The target must already be halted and configured for
 * a chip, because that is what makes the DMI access legal.
 */
enum wl_status wl_dm_pc_read(wl_linke_t *link, uint32_t *value);

/**
 * Load a program into the program buffer and run it.
 *
 * The program runs on the halted target with x9 holding @p address and x8
 * holding @p value; that is the calling convention every program in this tool
 * uses, because it needs no memory of its own.
 */
enum wl_status wl_dm_run_write(wl_linke_t *link,
			       const uint32_t *program,
			       size_t words,
			       uint32_t address,
			       uint32_t value);

/** As wl_dm_run_write(), but x8 is read back afterwards. */
enum wl_status wl_dm_run_read(wl_linke_t *link,
			      const uint32_t *program,
			      size_t words,
			      uint32_t address,
			      uint32_t *value);

/* --- memory access ------------------------------------------------------ */

/** Write @p value to @p address as a 32-bit store. */
enum wl_status
wl_dm_write32(wl_linke_t *link, uint32_t address, uint32_t value);

/** Write the low half of @p value to @p address as a 16-bit store. */
enum wl_status
wl_dm_write16(wl_linke_t *link, uint32_t address, uint16_t value);

/** Read a 32-bit word. */
enum wl_status
wl_dm_read32(wl_linke_t *link, uint32_t address, uint32_t *value);

/** Read a 16-bit halfword. */
enum wl_status
wl_dm_read16(wl_linke_t *link, uint32_t address, uint16_t *value);

/**
 * Read a run of target memory.
 *
 * Unaligned ends are read a byte at a time, which costs one program-buffer
 * round trip per byte; the aligned middle is read a word at a time.
 */
enum wl_status wl_dm_read_block(wl_linke_t *link,
				uint32_t address,
				void *buffer,
				size_t length);

/* --- instruction encoding ----------------------------------------------- */

/*
 * The encoder is public because it is the one part of the debug-module layer
 * that can be checked exactly, without a target or a programmer: a wrong
 * encoding is a silent corruption on hardware and an obvious mismatch in a
 * unit test.
 */

/** `sw`/`sh`/`sb` with a zero offset: enc_store(2, 8, 9) is `sw x8,0(x9)`. */
uint32_t wl_rv_store(unsigned size_log2, unsigned src, unsigned base);

/** `lw`/`lh`/`lbu` with a zero offset. */
uint32_t wl_rv_load(unsigned funct3, unsigned dest, unsigned base);

/** `addi rd, rs1, imm`. */
uint32_t wl_rv_addi(unsigned dest, unsigned src, int32_t immediate);

/** `bne rs1, rs2, imm`, used by the polling loops. */
uint32_t wl_rv_bne(unsigned src1, unsigned src2, int32_t immediate);

/** `ebreak`, which hands control back to the debug module. */
uint32_t wl_rv_ebreak(void);

#endif /* WCHLINK_DM_H */
