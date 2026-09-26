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
#include "log.h"

/*
 * Abstract command encodings, from the RISC-V Debug Specification.
 *
 * A command word is
 *
 *   31:24  cmdtype            0 = access register
 *   23     aarpostincrement
 *   22:20  aarsize            2 = 32 bits
 *   19     postincrement
 *   18     postexec           run the program buffer afterwards
 *   17     transfer           move data between DATA0 and the register
 *   16     write              1 = DATA0 into the register
 *   15:0   regno              0x1000+n is x<n>, 0x0000+n is CSR n
 *
 * Every command this tool issues is cmdtype 0 with aarsize 2, so the three
 * constants below cover the whole set.
 */
#define WL_DM_CMD_GPR_BASE 0x00200000u /**< aarsize=2, cmdtype=0 */
#define WL_DM_CMD_TRANSFER 0x00020000u /**< transfer = 1 */
#define WL_DM_CMD_WRITE 0x00010000u    /**< write = 1 */
#define WL_DM_CMD_POSTEXEC 0x00040000u /**< postexec = 1 */
#define WL_DM_GPR_REGNO(n) (0x1000u + ((n) & 0x1fu))

/** abstractcs: busy is bit 12, cmderr is bits 10:8, and are write-1-to-clear. */
#define WL_DM_ABSTRACTCS_BUSY (1u << 12)
#define WL_DM_ABSTRACTCS_CMDMASK (0x7u << 8)
#define WL_DM_ABSTRACTCS_CLEAR 0x00000700u

#define WL_DM_TIMEOUT_US                                                       \
	2000000 /**< two seconds, far longer than an operation */

static enum wl_status dm_wait(wl_linke_t *link) {
	uint32_t abstractcs = 0;
	unsigned spins = 0;

	/*
	 * Poll until the command finishes.  The bound is a safety net for a
	 * programmer that has stopped answering rather than a timing
	 * assumption: an abstract command takes microseconds.
	 */
	for (spins = 0; spins < WL_DM_TIMEOUT_US; spins++) {
		enum wl_status status =
		    wl_linke_dmi_read(link, WL_DMI_DMABSTRACTCS, &abstractcs);

		if (status != WL_OK) {
			return status;
		}

		if ((abstractcs & WL_DM_ABSTRACTCS_BUSY) == 0) {
			break;
		}
	}

	if ((abstractcs & WL_DM_ABSTRACTCS_BUSY) != 0) {
		wl_error("the target's debug module stayed busy; "
			 "is the programmer still attached?");
		return WL_ERR_TARGET;
	}

	if ((abstractcs & WL_DM_ABSTRACTCS_CMDMASK) != 0) {
		unsigned error = (abstractcs & WL_DM_ABSTRACTCS_CMDMASK) >> 8;

		/*
		 * Clear the error so the next command is not rejected for the
		 * previous one's sake, then report which it was.  A bus error
		 * is the interesting one: it means the address does not answer,
		 * which on these parts usually means the core is running or the
		 * peripheral clock is gated.
		 */
		(void)wl_linke_dmi_write(link, WL_DMI_DMABSTRACTCS,
					 WL_DM_ABSTRACTCS_CLEAR);

		switch (error) {
		case 1:
			wl_error("debug command is still executing");
			break;
		case 2:
			wl_error(
			    "the debug module does not support that command");
			break;
		case 3:
			wl_error("the debug module reported an exception");
			break;
		case 4:
			wl_error("the target is not halted");
			break;
		case 5:
			wl_error("target bus error: address 0x%08x did not "
				 "answer",
				 0u);
			break;
		default:
			wl_error("debug command failed (cmderr=%u)", error);
			break;
		}

		return WL_ERR_TARGET;
	}

	return WL_OK;
}

/** Issue one access-register command and wait for it. */
static enum wl_status dm_command(wl_linke_t *link,
				 uint32_t command,
				 uint32_t value,
				 bool transfer_in,
				 uint32_t *result) {
	enum wl_status status;

	if (transfer_in) {
		status = wl_linke_dmi_write(link, WL_DMI_DATA0, value);

		if (status != WL_OK) {
			return status;
		}
	}

	status = wl_linke_dmi_write(link, WL_DMI_DMCOMMAND, command);

	if (status != WL_OK) {
		return status;
	}

	status = dm_wait(link);

	if (status != WL_OK) {
		return status;
	}

	if (result != NULL) {
		return wl_linke_dmi_read(link, WL_DMI_DATA0, result);
	}

	return WL_OK;
}

enum wl_status wl_dm_gpr_write(wl_linke_t *link, unsigned reg, uint32_t value) {
	uint32_t command;

	if (reg > 31) {
		return WL_ERR_USAGE;
	}

	command = WL_DM_CMD_GPR_BASE | WL_DM_CMD_TRANSFER | WL_DM_CMD_WRITE |
		  WL_DM_GPR_REGNO(reg);

	return dm_command(link, command, value, true, NULL);
}

enum wl_status wl_dm_gpr_read(wl_linke_t *link, unsigned reg, uint32_t *value) {
	uint32_t command;

	if (reg > 31 || value == NULL) {
		return WL_ERR_USAGE;
	}

	command =
	    WL_DM_CMD_GPR_BASE | WL_DM_CMD_TRANSFER | WL_DM_GPR_REGNO(reg);

	return dm_command(link, command, 0, false, value);
}

enum wl_status wl_dm_pc_read(wl_linke_t *link, uint32_t *value) {
	uint32_t command;

	if (value == NULL) {
		return WL_ERR_USAGE;
	}

	/*
	 * dpc is not reachable through the programmer's 8-bit DMI register
	 * command.  It is an abstract-command register number, exactly as PC
	 * is read by minichlink: cmdtype 0, aarsize 2, transfer, regno
	 * 0x7b1.  The 0x08 command would truncate the address to 0xb1.
	 */
	command = WL_DM_CMD_GPR_BASE | WL_DM_CMD_TRANSFER | WL_DMI_DPC;

	return dm_command(link, command, 0, false, value);
}

enum wl_status wl_dm_pc_write(wl_linke_t *link, uint32_t value) {
	uint32_t command = WL_DM_CMD_GPR_BASE | WL_DM_CMD_TRANSFER |
			   WL_DM_CMD_WRITE | WL_DMI_DPC;

	return dm_command(link, command, value, true, NULL);
}

static enum wl_status dm_csr_access(wl_linke_t *link,
				    uint32_t regno,
				    uint32_t value,
				    bool write,
				    uint32_t *result) {
	uint32_t command = WL_DM_CMD_GPR_BASE | WL_DM_CMD_TRANSFER | regno;

	if (write) {
		command |= WL_DM_CMD_WRITE;
	}

	return dm_command(link, command, value, write, result);
}

enum wl_status wl_dm_dcsr_read(wl_linke_t *link, uint32_t *value) {
	return dm_csr_access(link, WL_DMI_DCSR, 0, false, value);
}

enum wl_status wl_dm_dcsr_write(wl_linke_t *link, uint32_t value) {
	return dm_csr_access(link, WL_DMI_DCSR, value, true, NULL);
}

enum wl_status wl_dm_csr_read(wl_linke_t *link, uint32_t csr, uint32_t *value) {
	return dm_csr_access(link, csr, 0, false, value);
}

enum wl_status wl_dm_csr_write(wl_linke_t *link, uint32_t csr, uint32_t value) {
	return dm_csr_access(link, csr, value, true, NULL);
}

static enum wl_status
dm_load_program(wl_linke_t *link, const uint32_t *program, size_t words) {
	size_t i;
	enum wl_status status;

	if (words > WL_DM_PROGBUF_WORDS) {
		wl_error("internal error: %zu-word program does not fit the "
			 "%d-word program buffer",
			 words, WL_DM_PROGBUF_WORDS);
		return WL_ERR_USAGE;
	}

	/*
	 * Auto-execution must be off, or the debug module runs the program
	 * again on every program-buffer write.
	 */
	status = wl_linke_dmi_write(link, WL_DMI_DMABSTRACTAUTO, 0);

	if (status != WL_OK) {
		return status;
	}

	for (i = 0; i < words; i++) {
		status = wl_linke_dmi_write(
		    link, (uint8_t)(WL_DMI_DMPROGBUF0 + i), program[i]);

		if (status != WL_OK) {
			return status;
		}
	}

	return WL_OK;
}

/** x9 = address, then x8 = value with postexec, then read x8 back if asked. */
static enum wl_status dm_run(wl_linke_t *link,
			     const uint32_t *program,
			     size_t words,
			     uint32_t address,
			     uint32_t value,
			     uint32_t *result) {
	uint32_t command;
	enum wl_status status;

	status = dm_load_program(link, program, words);

	if (status != WL_OK) {
		return status;
	}

	status = wl_dm_gpr_write(link, 9, address);

	if (status != WL_OK) {
		return status;
	}

	command = WL_DM_CMD_GPR_BASE | WL_DM_CMD_TRANSFER | WL_DM_CMD_WRITE |
		  WL_DM_CMD_POSTEXEC | WL_DM_GPR_REGNO(8);

	status = dm_command(link, command, value, true, NULL);

	if (status != WL_OK) {
		return status;
	}

	if (result != NULL) {
		return wl_dm_gpr_read(link, 8, result);
	}

	return WL_OK;
}

enum wl_status wl_dm_run_write(wl_linke_t *link,
			       const uint32_t *program,
			       size_t words,
			       uint32_t address,
			       uint32_t value) {
	return dm_run(link, program, words, address, value, NULL);
}

enum wl_status wl_dm_run_read(wl_linke_t *link,
			      const uint32_t *program,
			      size_t words,
			      uint32_t address,
			      uint32_t *value) {
	if (value == NULL) {
		return WL_ERR_USAGE;
	}

	return dm_run(link, program, words, address, 0, value);
}

/* --- instruction encoding ----------------------------------------------- */

uint32_t wl_rv_store(unsigned size_log2, unsigned src, unsigned base) {
	/* S-type: imm[11:5] | rs2 | rs1 | funct3 | imm[4:0] | opcode */
	uint32_t funct3 = size_log2 & 0x7u; /* 0=sb, 1=sh, 2=sw */
	uint32_t opcode = 0x23u;
	uint32_t rs2 = (src & 0x1fu) << 20;
	uint32_t rs1 = (base & 0x1fu) << 15;

	return funct3 << 12 | rs2 | rs1 | opcode; /* offset 0 */
}

uint32_t wl_rv_load(unsigned funct3, unsigned dest, unsigned base) {
	/* I-type: imm[11:0] | rs1 | funct3 | rd | opcode */
	uint32_t rs1 = (base & 0x1fu) << 15;
	uint32_t rd = (dest & 0x1fu) << 7;
	uint32_t opcode = 0x03u;

	return (funct3 & 0x7u) << 12 | rs1 | rd | opcode; /* offset 0 */
}

uint32_t wl_rv_addi(unsigned dest, unsigned src, int32_t immediate) {
	/* I-type, opcode 0x13 */
	uint32_t imm = (uint32_t)immediate & 0xfffu;
	uint32_t rs1 = (src & 0x1fu) << 15;
	uint32_t rd = (dest & 0x1fu) << 7;

	return imm << 20 | rs1 | rd | 0x13u;
}

uint32_t wl_rv_bne(unsigned src1, unsigned src2, int32_t immediate) {
	/* B-type: imm[12|10:5] | rs2 | rs1 | funct3 | imm[4:1|11] | opcode */
	uint32_t value = (uint32_t)immediate;
	uint32_t imm12 = (value >> 12) & 0x1u;
	uint32_t imm11 = (value >> 11) & 0x1u;
	uint32_t imm10_5 = (value >> 5) & 0x3fu;
	uint32_t imm4_1 = (value >> 1) & 0xfu;
	uint32_t rs1 = (src1 & 0x1fu) << 15;
	uint32_t rs2 = (src2 & 0x1fu) << 20;

	return imm12 << 31 | imm10_5 << 25 | rs2 | rs1 | 0x1u << 12 |
	       imm4_1 << 8 | imm11 << 7 | 0x63u;
}

uint32_t wl_rv_ebreak(void) {
	return 0x00100073u;
}

/* --- memory access ------------------------------------------------------ */

/*
 * The programs below are the whole memory-access machinery.  They are built
 * with the encoder rather than written out as hex so that a reader can see
 * what they do -- `sw x8,0(x9); ebreak` -- and so that the encoding is checked
 * by a test against the ISA rather than by eye.
 *
 * The index is the access size as a log2: 0 = byte, 1 = halfword, 2 = word.
 */
static uint32_t write_programs[3][2];
static uint32_t read_programs[3][2];
static bool programs_ready;

static void build_programs(void) {
	unsigned size;

	if (programs_ready) {
		return;
	}

	for (size = 0; size < 3; size++) {
		/* sw/sh/sb x8, 0(x9); ebreak */
		write_programs[size][0] = wl_rv_store(size, 8, 9);
		write_programs[size][1] = wl_rv_ebreak();

		/* lw/lh/lbu x8, 0(x9); ebreak */
		read_programs[size][0] = wl_rv_load(
		    size == 2 ? 0x2u : (size == 1 ? 0x1u : 0x4u), 8, 9);
		read_programs[size][1] = wl_rv_ebreak();
	}

	programs_ready = true;
}

enum wl_status
wl_dm_write32(wl_linke_t *link, uint32_t address, uint32_t value) {
	build_programs();

	return wl_dm_run_write(link, write_programs[2], 2, address, value);
}

enum wl_status
wl_dm_write16(wl_linke_t *link, uint32_t address, uint16_t value) {
	build_programs();

	return wl_dm_run_write(link, write_programs[1], 2, address, value);
}

enum wl_status wl_dm_write8(wl_linke_t *link, uint32_t address, uint8_t value) {
	build_programs();

	return wl_dm_run_write(link, write_programs[0], 2, address, value);
}

static enum wl_status dm_read_sized(wl_linke_t *link,
				    unsigned size_log2,
				    uint32_t address,
				    uint32_t *value) {
	build_programs();

	return wl_dm_run_read(link, read_programs[size_log2], 2, address,
			      value);
}

enum wl_status
wl_dm_read32(wl_linke_t *link, uint32_t address, uint32_t *value) {
	return dm_read_sized(link, 2, address, value);
}

enum wl_status
wl_dm_read16(wl_linke_t *link, uint32_t address, uint16_t *value) {
	uint32_t word = 0;
	enum wl_status status;

	if (value == NULL) {
		return WL_ERR_USAGE;
	}

	status = dm_read_sized(link, 1, address, &word);

	if (status != WL_OK) {
		return status;
	}

	*value = (uint16_t)(word & 0xffffu);

	return WL_OK;
}

enum wl_status wl_dm_read_block(wl_linke_t *link,
				uint32_t address,
				void *buffer,
				size_t length) {
	uint8_t *out = (uint8_t *)buffer;
	enum wl_status status;
	uint32_t word;
	size_t i;

	if (buffer == NULL && length != 0) {
		return WL_ERR_USAGE;
	}

	/* Unaligned head: one byte per program-buffer round trip. */
	while (length > 0 && (address & 0x3u) != 0) {
		status = dm_read_sized(link, 0, address, &word);

		if (status != WL_OK) {
			return status;
		}

		*out++ = (uint8_t)(word & 0xffu);
		address++;
		length--;
	}

	while (length >= 4) {
		status = wl_dm_read32(link, address, &word);

		if (status != WL_OK) {
			return status;
		}

		for (i = 0; i < 4; i++) {
			*out++ = (uint8_t)((word >> (8u * i)) & 0xffu);
		}

		address += 4;
		length -= 4;
	}

	while (length > 0) {
		status = dm_read_sized(link, 0, address, &word);

		if (status != WL_OK) {
			return status;
		}

		*out++ = (uint8_t)(word & 0xffu);
		address++;
		length--;
	}

	return WL_OK;
}
