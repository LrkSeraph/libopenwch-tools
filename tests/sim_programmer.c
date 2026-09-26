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

#include <stdlib.h>
#include <string.h>

#include "dm.h"
#include "sim_programmer.h"

/* --- the flash controller's addresses ------------------------------------ */

#define SIM_FLASH_CTRL 0x40022000u
#define SIM_FLASH_KEYR (SIM_FLASH_CTRL + 0x04u)
#define SIM_FLASH_STATR (SIM_FLASH_CTRL + 0x0cu)
#define SIM_FLASH_CTLR (SIM_FLASH_CTRL + 0x10u)
#define SIM_FLASH_ADDR (SIM_FLASH_CTRL + 0x14u)
#define SIM_FLASH_MODEKEYR (SIM_FLASH_CTRL + 0x24u)

#define SIM_KEY1 0x45670123u
#define SIM_KEY2 0xcdef89abu

#define SIM_CTLR_PG 0x00000001u
#define SIM_CTLR_PER 0x00000002u
#define SIM_CTLR_STRT 0x00000040u
#define SIM_CTLR_LOCK 0x00000080u
#define SIM_CTLR_PAGE_PG 0x00010000u
#define SIM_CTLR_PAGE_ER 0x00020000u
#define SIM_CTLR_BUF_LOAD 0x00040000u
#define SIM_CTLR_BUF_RST 0x00080000u

#define SIM_STATR_BSY 0x00000001u
#define SIM_STATR_WRPRTERR 0x00000010u

/** How many status reads report BSY after an operation starts. */
#define SIM_BUSY_READS 2

/** How many instructions a program buffer may run before it is called stuck. */
#define SIM_MAX_STEPS 256

struct wl_sim {
	const wl_chip_t *chip;

	uint8_t *flash; /**< flash_size bytes, offsets from the flash base */
	size_t flash_size;
	uint8_t *ram;
	size_t ram_size;

	/* What the programmer is doing. */
	bool configured;
	uint8_t chip_id;
	uint8_t interface_speed;
	bool attached;

	/* Debug module state. */
	uint32_t dmi[128];
	uint32_t gpr[32];
	uint32_t progbuf[8];
	uint32_t pc;
	uint32_t dpc;
	uint32_t dcsr;
	uint32_t cmderr;

	/* Flash controller state. */
	uint32_t key_stage;	 /**< which normal unlock key came last */
	uint32_t mode_key_stage; /**< which fast-mode key came last */
	bool unlocked;
	bool mode_unlocked;
	uint32_t ctlr;
	uint32_t addr;
	uint8_t page_buf[64];
	unsigned busy_reads;

	/* Counters the tests assert on. */
	unsigned erases;
	unsigned programs;
	unsigned program_errors;
	unsigned illegal;
};

/* --- the target's memory ------------------------------------------------- */

static bool sim_is_flash(uint32_t address) {
	return (address & 0xff000000u) == 0x08000000u ||
	       (address & 0xff000000u) == 0x00000000u;
}

static size_t sim_flash_offset(uint32_t address) {
	/* Both the 0x08000000 mapping and its 0x00000000 alias land here. */
	return address & 0x00ffffffu;
}

static bool sim_is_ram(const struct wl_sim *sim, uint32_t address) {
	return address >= sim->chip->ram_offset &&
	       address < sim->chip->ram_offset + sim->ram_size;
}

static bool sim_is_ctrl(const struct wl_sim *sim, uint32_t address) {
	(void)sim;

	return address >= SIM_FLASH_CTRL && address < SIM_FLASH_CTRL + 0x400u;
}

static void sim_flash_program_page(struct wl_sim *sim) {
	size_t offset = sim_flash_offset(sim->addr);
	size_t i;

	if (!sim->mode_unlocked) {
		sim->program_errors++;
		return;
	}

	for (i = 0; i < 64u && offset + i < sim->flash_size; i++) {
		uint8_t old = sim->flash[offset + i];
		uint8_t wanted = sim->page_buf[i];

		if ((old & wanted) != wanted) {
			sim->program_errors++;
		}

		sim->flash[offset + i] = (uint8_t)(old & wanted);
	}

	sim->programs++;
	sim->busy_reads = SIM_BUSY_READS;
}

static void
sim_ctrl_write(struct wl_sim *sim, uint32_t address, uint32_t value) {
	switch (address) {
	case SIM_FLASH_KEYR:
		if (value == SIM_KEY1) {
			sim->key_stage = 1;
		} else if (value == SIM_KEY2 && sim->key_stage == 1) {
			sim->unlocked = true;
			sim->key_stage = 0;
		} else {
			/* A wrong key, or the right one out of order,
 * leaves the controller locked -- as the real one
 * does, which is what makes the tool's read-back of
 * the LOCK bit meaningful. */
			sim->key_stage = 0;
		}
		return;

	case SIM_FLASH_MODEKEYR:
		if (value == SIM_KEY1) {
			sim->mode_key_stage = 1;
		} else if (value == SIM_KEY2 && sim->mode_key_stage == 1) {
			sim->mode_unlocked = true;
			sim->mode_key_stage = 0;
		} else {
			sim->mode_key_stage = 0;
		}
		return;

	case SIM_FLASH_ADDR:
		sim->addr = value;
		return;

	case SIM_FLASH_CTLR:
		sim->ctlr = value;

		/* Fast buffer reset. */
		if ((value & SIM_CTLR_BUF_RST) != 0) {
			if (sim->mode_unlocked) {
				memset(sim->page_buf, 0xff,
				       sizeof(sim->page_buf));
			} else {
				sim->program_errors++;
			}

			sim->busy_reads = SIM_BUSY_READS;
			return;
		}

		if ((value & SIM_CTLR_STRT) == 0) {
			return;
		}

		if ((value & SIM_CTLR_PAGE_ER) != 0) {
			size_t offset = sim_flash_offset(sim->addr);
			size_t i;

			if (!sim->mode_unlocked) {
				sim->program_errors++;
				return;
			}

			for (i = 0; i < 64u && offset + i < sim->flash_size;
			     i++) {
				sim->flash[offset + i] = 0xffu;
			}

			sim->erases++;
			sim->busy_reads = SIM_BUSY_READS;
		} else if ((value & SIM_CTLR_PAGE_PG) != 0) {
			sim_flash_program_page(sim);
		} else if ((value & SIM_CTLR_PER) != 0) {
			size_t offset = sim_flash_offset(sim->addr);
			size_t i;

			for (i = 0; i < 1024u && offset + i < sim->flash_size;
			     i++) {
				sim->flash[offset + i] = 0xffu;
			}

			sim->erases++;
			sim->busy_reads = SIM_BUSY_READS;
		} else {
			/*
 * A normal halfword program is triggered by the
 * store that follows, not by STRT.
 */
			sim->busy_reads = SIM_BUSY_READS;
		}
		return;

	default:
		return;
	}
}

static uint32_t sim_ctrl_read(struct wl_sim *sim, uint32_t address) {
	switch (address) {
	case SIM_FLASH_STATR: {
		uint32_t status = 0;

		if (sim->busy_reads > 0) {
			sim->busy_reads--;
			status |= SIM_STATR_BSY;
		}

		if (sim->program_errors > 0) {
			status |= SIM_STATR_WRPRTERR;
		}

		return status;
	}

	case SIM_FLASH_CTLR:
		return sim->unlocked ? (sim->ctlr & ~SIM_CTLR_LOCK)
				     : (sim->ctlr | SIM_CTLR_LOCK);

	case SIM_FLASH_ADDR:
		return sim->addr;

	case SIM_FLASH_KEYR:
	case SIM_FLASH_MODEKEYR:
		return 0;

	default:
		return 0;
	}
}

/**
 * One memory access from the simulated core.
 *
 * @param size_log2  0 = byte, 1 = halfword, 2 = word
 */
static void sim_access(struct wl_sim *sim,
		       uint32_t address,
		       unsigned size_log2,
		       bool write,
		       uint32_t *value) {
	size_t width = (size_t)1u << size_log2;

	if (sim_is_ctrl(sim, address)) {
		if (write) {
			sim_ctrl_write(sim, address & ~0x3u, *value);
		} else {
			*value = sim_ctrl_read(sim, address & ~0x3u);
		}

		return;
	}

	if (sim_is_flash(address)) {
		size_t offset = sim_flash_offset(address);
		size_t i;

		if (offset + width > sim->flash_size) {
			/* Beyond the part: reads as zero, writes are lost.  A
			 * real target would fault here; the simulator does not
			 * need to, because the tool checks the range first. */
			return;
		}

		if (!write) {
			uint32_t result = 0;

			for (i = 0; i < width; i++) {
				result |= (uint32_t)sim->flash[offset + i]
					  << (8u * i);
			}

			if (size_log2 == 1) {
				result = (uint32_t)(int32_t)(int16_t)result;
			}

			*value = result;
			return;
		}

		/*
		 * Fast page program: writing a word while PAGE_PG is set loads
		 * it into the 64-byte buffer.  The page is committed later by
		 * the PAGE_PG|STRT control write, not by this store.
		 */
		if (sim->mode_unlocked && (sim->ctlr & SIM_CTLR_PAGE_PG) != 0 &&
		    size_log2 == 2) {
			size_t page_off = offset & 0x3fu;

			if (page_off + 4u <= sizeof(sim->page_buf)) {
				for (i = 0; i < 4u; i++) {
					sim->page_buf[page_off + i] =
					    (uint8_t)((*value >> (8u * i)) &
						      0xffu);
				}

				sim->busy_reads = SIM_BUSY_READS;
				return;
			}

			sim->program_errors++;
			return;
		}

		/*
		 * Normal halfword programming is only possible with the
		 * controller unlocked and PG set, and it can only clear bits.
		 * Anything else is remembered: the tests assert that the count
		 * is zero, which is what proves the erase happened first.
		 */
		if (!sim->unlocked || (sim->ctlr & SIM_CTLR_PG) == 0) {
			sim->program_errors++;
			return;
		}

		for (i = 0; i < width; i++) {
			uint8_t wanted =
			    (uint8_t)((*value >> (8u * i)) & 0xffu);
			uint8_t old = sim->flash[offset + i];

			if ((old & wanted) != wanted) {
				sim->program_errors++;
			}

			sim->flash[offset + i] = (uint8_t)(old & wanted);
		}

		sim->programs++;
		sim->busy_reads = SIM_BUSY_READS;
		return;
	}

	if (sim_is_ram(sim, address)) {
		size_t offset = address - sim->chip->ram_offset;
		size_t i;

		if (offset + width > sim->ram_size) {
			return;
		}

		if (write) {
			for (i = 0; i < width; i++) {
				sim->ram[offset + i] =
				    (uint8_t)((*value >> (8u * i)) & 0xffu);
			}
		} else {
			uint32_t result = 0;

			for (i = 0; i < width; i++) {
				result |= (uint32_t)sim->ram[offset + i]
					  << (8u * i);
			}

			if (size_log2 == 1) {
				result = (uint32_t)(int32_t)(int16_t)result;
			}

			*value = result;
		}

		return;
	}

	/* Nothing else is modelled; reads answer zero. */
	if (!write) {
		*value = 0;
	}
}

/* --- running a program buffer program ------------------------------------ */

static int32_t sign_extend(uint32_t value, unsigned bits) {
	uint32_t mask = 1u << (bits - 1u);

	return (int32_t)((value ^ mask) - mask);
}

/** Run the program buffer until ebreak.  Returns false on a bad instruction. */
static bool sim_run_progbuf(struct wl_sim *sim) {
	unsigned steps;

	for (steps = 0; steps < SIM_MAX_STEPS; steps++) {
		uint32_t instruction = sim->progbuf[sim->pc / 4u];
		uint32_t opcode = instruction & 0x7fu;
		unsigned rd = (instruction >> 7) & 0x1fu;
		unsigned funct3 = (instruction >> 12) & 0x7u;
		unsigned rs1 = (instruction >> 15) & 0x1fu;
		unsigned rs2 = (instruction >> 20) & 0x1fu;
		uint32_t value;

		switch (opcode) {
		case 0x23: { /* store */
			uint32_t imm = ((instruction >> 25) << 5) |
				       ((instruction >> 7) & 0x1fu);
			unsigned size = funct3 & 0x3u;

			value = sim->gpr[rs2];
			sim_access(sim, sim->gpr[rs1] + imm, size, true,
				   &value);
			sim->pc += 4;
			break;
		}

		case 0x03: { /* load */
			uint32_t imm = instruction >> 20;
			unsigned size = funct3 & 0x3u;

			value = 0;
			sim_access(sim, sim->gpr[rs1] + imm, size, false,
				   &value);

			if (funct3 == 0x4u) { /* lbu: zero-extend a byte */
				value &= 0xffu;
			}

			sim->gpr[rd] = rd == 0 ? 0 : value;
			sim->pc += 4;
			break;
		}

		case 0x13: { /* addi */
			int32_t imm = sign_extend(instruction >> 20, 12);

			value = (uint32_t)((int32_t)sim->gpr[rs1] + imm);
			sim->gpr[rd] = rd == 0 ? 0 : value;
			sim->pc += 4;
			break;
		}

		case 0x63: { /* bne */
			uint32_t imm = ((instruction >> 31) << 12) |
				       (((instruction >> 25) & 0x3fu) << 5) |
				       (((instruction >> 8) & 0xfu) << 1) |
				       ((instruction >> 7) & 0x1u);

			if (sim->gpr[rs1] != sim->gpr[rs2]) {
				sim->pc += sign_extend(imm, 13);
			} else {
				sim->pc += 4;
			}

			break;
		}

		case 0x73: /* ebreak: back to the debug module */
			if ((instruction >> 20) == 0x001u) {
				return true;
			}

			sim->illegal++;
			return false;

		default:
			sim->illegal++;
			return false;
		}

		if (sim->pc / 4u >= 8u) {
			sim->illegal++;
			return false;
		}
	}

	sim->illegal++;
	return false;
}

/* --- the debug module ---------------------------------------------------- */

static void sim_execute_command(struct wl_sim *sim, uint32_t command) {
	unsigned cmdtype = (command >> 24) & 0x7fu;
	unsigned aarsize = (command >> 20) & 0x7u;
	bool postexec = (command >> 18) & 0x1u;
	bool transfer = (command >> 17) & 0x1u;
	bool write = (command >> 16) & 0x1u;
	unsigned regno = command & 0xffffu;
	unsigned gpr;

	if (cmdtype != 0) {
		sim->cmderr = 2; /* unsupported command */
		return;
	}

	if (transfer) {
		uint32_t value;

		if (regno == WL_DMI_DPC || regno == WL_DMI_DCSR) {
			/* dpc/dcsr are abstract-command register numbers, not
			 * 8-bit DMI addresses. */
			if (write) {
				if (regno == WL_DMI_DPC) {
					sim->dpc = sim->dmi[WL_DMI_DATA0];
				} else {
					sim->dcsr = sim->dmi[WL_DMI_DATA0];
				}
			} else {
				sim->dmi[WL_DMI_DATA0] =
				    regno == WL_DMI_DPC ? sim->dpc : sim->dcsr;
			}
		} else if (regno >= 0x1000u && regno <= 0x101fu) {
			gpr = regno - 0x1000u;

			if (write) {
				sim->gpr[gpr] =
				    gpr == 0 ? 0 : sim->dmi[WL_DMI_DATA0];
			} else {
				value = sim->gpr[gpr];

				if (aarsize == 0) {
					value &= 0xffu;
				} else if (aarsize == 1) {
					value &= 0xffffu;
				}

				sim->dmi[WL_DMI_DATA0] = value;
			}
		} else {
			sim->cmderr = 2; /* unsupported register */
			return;
		}
	}

	if (postexec) {
		sim->pc = 0;
		(void)sim_run_progbuf(sim);
	}
}

static void sim_dmi_write(struct wl_sim *sim, uint8_t reg, uint32_t value) {
	if (reg < 128u) {
		sim->dmi[reg] = value;
	}

	switch (reg) {
	case WL_DMI_DMABSTRACTCS:
		/* Write-1-to-clear, as the real register is. */
		sim->cmderr &= ~((value >> 8) & 0x7u);
		break;

	case WL_DMI_DMPROGBUF0:
	case WL_DMI_DMPROGBUF1:
	case WL_DMI_DMPROGBUF2:
	case WL_DMI_DMPROGBUF3:
	case WL_DMI_DMPROGBUF4:
	case WL_DMI_DMPROGBUF5:
	case WL_DMI_DMPROGBUF6:
	case WL_DMI_DMPROGBUF7:
		sim->progbuf[reg - WL_DMI_DMPROGBUF0] = value;
		break;

	case WL_DMI_DMCOMMAND:
		sim_execute_command(sim, value);
		break;

	default:
		break;
	}
}

static uint32_t sim_dmi_read(struct wl_sim *sim, uint8_t reg) {
	if (reg == WL_DMI_DMABSTRACTCS) {
		/* Never busy: the command has already run by the time the
		 * programmer answers, which is how the real one behaves. */
		return sim->cmderr << 8;
	}

	if (reg < 128u) {
		return sim->dmi[reg];
	}

	return 0;
}

/* --- the transport ------------------------------------------------------- */

#define SIM_REPLY_MAX 64

/** Reject a request the way a programmer that cannot satisfy it does. */
static size_t sim_refuse(uint8_t *reply, size_t reply_max) {
	if (reply_max >= 4) {
		reply[0] = 0x81;
		reply[1] = 0x55;
		reply[2] = 0x01;
		reply[3] = 0x00;

		return 4;
	}

	return 0;
}

static int sim_command(void *ctx,
		       const uint8_t *request,
		       size_t request_len,
		       uint8_t *reply,
		       size_t reply_max,
		       size_t *reply_len) {
	struct wl_sim *sim = (struct wl_sim *)ctx;
	uint8_t command;
	size_t length = 0;

	if (sim == NULL || request_len < 3u || request[0] != 0x81u) {
		return -1;
	}

	command = request[1];

	memset(reply, 0, reply_max);

	switch (command) {
	case 0x0du: { /* control */
		uint8_t sub = request_len > 3u ? request[3] : 0;

		if (sub == 0x01u) { /* status and version */
			reply[0] = 0x82;
			reply[1] = 0x0d;
			reply[2] = 0x04;
			reply[3] = 0x02; /* version 2.7 */
			reply[4] = 0x07;
			reply[5] = 18; /* WCH-LinkE */
			reply[6] = 0x00;
			length = 7;
		} else if (sub == 0x02u) { /* attach and report family/model */
			uint16_t model =
			    sim->chip != NULL ? sim->chip->model_id : 0u;

			sim->attached = true;

			reply[0] = 0x82;
			reply[1] = 0x0d;
			reply[2] = 0x05;
			reply[3] = sim->chip != NULL ? sim->chip->linke_id : 0u;
			reply[4] = (uint8_t)(model >> 8);
			reply[5] = (uint8_t)model;
			reply[6] = 0x05;
			reply[7] = 0x00;
			length = 8;
		} else {
			if (sub == 0xffu) {
				sim->attached = false;
			}

			reply[0] = 0x82;
			reply[1] = 0x0d;
			reply[2] = 0x05;
			reply[3] = 0x09;
			length = 4;
		}

		break;
	}

	case 0x0cu: { /* chip type and interface speed */
		if (request_len < 5u) {
			length = sim_refuse(reply, reply_max);
			break;
		}

		sim->chip_id = request[3];
		sim->interface_speed = request[4];
		sim->configured = true;

		reply[0] = 0x82;
		reply[1] = 0x0c;
		reply[2] = 0x02;
		reply[3] = request[3];
		length = 4;
		break;
	}

	case 0x0bu: /* run */
		sim->attached = false;

		reply[0] = 0x82;
		reply[1] = 0x0b;
		reply[2] = 0x01;
		length = 3;
		break;

	case 0x08u: { /* debug-module register access */
		uint8_t reg;
		uint32_t value;
		uint8_t operation;

		if (request_len < 9u) {
			length = sim_refuse(reply, reply_max);
			break;
		}

		reg = request[3];
		value = ((uint32_t)request[4] << 24) |
			((uint32_t)request[5] << 16) |
			((uint32_t)request[6] << 8) | (uint32_t)request[7];
		operation = request[8];

		if (!sim->configured) {
			length = sim_refuse(reply, reply_max);
			break;
		}

		if (operation == 0x02u) {
			sim_dmi_write(sim, reg, value);
		} else if (operation != 0x01u) {
			length = sim_refuse(reply, reply_max);
			break;
		}

		value = sim_dmi_read(sim, reg);

		reply[0] = 0x82;
		reply[1] = 0x08;
		reply[2] = 0x06;
		reply[3] = reg;
		reply[4] = (uint8_t)(value >> 24);
		reply[5] = (uint8_t)(value >> 16);
		reply[6] = (uint8_t)(value >> 8);
		reply[7] = (uint8_t)value;
		reply[8] = 0x00;
		length = 9;
		break;
	}

	default:
		length = sim_refuse(reply, reply_max);
		break;
	}

	if (reply_len != NULL) {
		*reply_len = length;
	}

	return 0;
}

static int sim_bulk_out(void *ctx, const uint8_t *data, size_t length) {
	(void)ctx;
	(void)data;
	(void)length;

	/* This tool writes memory through the program buffer, not through the
	 * programmer's bulk payload endpoint, so nothing should arrive here. */
	return -1;
}

static const struct wl_transport sim_transport = {
    sim_command,
    sim_bulk_out,
};

/* --- construction -------------------------------------------------------- */

struct wl_sim *wl_sim_new(const wl_chip_t *chip) {
	struct wl_sim *sim;

	if (chip == NULL || chip->flash_size == 0 || chip->ram_size == 0) {
		return NULL;
	}

	sim = calloc(1, sizeof(*sim));

	if (sim == NULL) {
		return NULL;
	}

	sim->chip = chip;
	sim->dpc = chip->flash_base;
	sim->flash_size = chip->flash_size;
	sim->ram_size = chip->ram_size;
	sim->flash = malloc(sim->flash_size);
	sim->ram = calloc(1, sim->ram_size);

	if (sim->flash == NULL || sim->ram == NULL) {
		wl_sim_free(sim);
		return NULL;
	}

	/* An unprogrammed part: every byte erased. */
	memset(sim->flash, 0xff, sim->flash_size);
	memset(sim->page_buf, 0xff, sizeof(sim->page_buf));

	return sim;
}

void wl_sim_free(struct wl_sim *sim) {
	if (sim == NULL) {
		return;
	}

	free(sim->flash);
	free(sim->ram);
	free(sim);
}

const struct wl_transport *wl_sim_transport(void) {
	return &sim_transport;
}

void *wl_sim_context(struct wl_sim *sim) {
	return sim;
}

const uint8_t *wl_sim_flash(const struct wl_sim *sim) {
	return sim->flash;
}

void wl_sim_flash_fill(struct wl_sim *sim,
		       uint32_t offset,
		       uint8_t value,
		       size_t length) {
	if (offset >= sim->flash_size) {
		return;
	}

	if (offset + length > sim->flash_size) {
		length = sim->flash_size - offset;
	}

	memset(sim->flash + offset, value, length);
}

void wl_sim_flash_poke(struct wl_sim *sim, uint32_t offset, uint8_t value) {
	if (offset < sim->flash_size) {
		sim->flash[offset] = value;
	}
}

unsigned wl_sim_erase_count(const struct wl_sim *sim) {
	return sim->erases;
}

unsigned wl_sim_program_count(const struct wl_sim *sim) {
	return sim->programs;
}

unsigned wl_sim_program_errors(const struct wl_sim *sim) {
	return sim->program_errors;
}

unsigned wl_sim_illegal_instructions(const struct wl_sim *sim) {
	return sim->illegal;
}

bool wl_sim_halted(const struct wl_sim *sim) {
	return sim->attached;
}
