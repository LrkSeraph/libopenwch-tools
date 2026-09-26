/*
 * This file is part of the libopenwch-tools project.
 *
 * Copyright (C) 2025 libopenwch contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "dm.h"
#include "flash.h"
#include "gdb_server.h"
#include "linke.h"
#include "log.h"
#include "target.h"

#define GDB_PACKET_MAX 4096
#define GDB_MEMORY_MAX 1024
#define GDB_RV32E_GPRS 16u
#define GDB_REG_PC 16u

#define GDB_DMCONTROL_HALT 0x80000001u
#define GDB_DMCONTROL_RESUME 0x40000001u
#define GDB_DMSTATUS_ANYRUNNING (1u << 10)
#define GDB_DCSR_STEP (1u << 2)
#define GDB_MAX_BREAKPOINTS 8u

struct gdb_breakpoint {
	bool used;
	uint32_t address;
	uint32_t kind;
	uint8_t original[4];
};

struct gdb_conn {
	int fd;
	wl_linke_t *link;
	const wl_chip_t *chip;
	bool quit;
	struct gdb_breakpoint breakpoints[GDB_MAX_BREAKPOINTS];
};

/* --- low-level I/O ------------------------------------------------------- */

static int gdb_send_all(int fd, const void *buffer, size_t length) {
	const uint8_t *p = (const uint8_t *)buffer;

	while (length > 0) {
		ssize_t written = send(fd, p, length, 0);

		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}

			return -1;
		}

		p += written;
		length -= (size_t)written;
	}

	return 0;
}

static int gdb_recv_byte(int fd, uint8_t *byte) {
	for (;;) {
		ssize_t got = recv(fd, byte, 1, 0);

		if (got == 1) {
			return 0;
		}

		if (got == 0) {
			return -1;
		}

		if (errno != EINTR) {
			return -1;
		}
	}
}

static int hex_nibble(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}

	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}

	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}

	return -1;
}

static bool parse_hex_u32(const char *text, const char **end, uint32_t *value) {
	uint32_t result = 0;
	bool any = false;

	while (*text != '\0') {
		int nibble = hex_nibble(*text);

		if (nibble < 0) {
			break;
		}

		result = (result << 4) | (uint32_t)nibble;
		any = true;
		text++;
	}

	if (!any) {
		return false;
	}

	*value = result;

	if (end != NULL) {
		*end = text;
	}

	return true;
}

static bool parse_hex_bytes(const char *text, uint8_t *out, size_t count) {
	size_t i;

	for (i = 0; i < count; i++) {
		int high = hex_nibble(text[0]);
		int low = hex_nibble(text[1]);

		if (high < 0 || low < 0) {
			return false;
		}

		out[i] = (uint8_t)((high << 4) | low);
		text += 2;
	}

	return true;
}

static void format_hex_byte(char *out, uint8_t value) {
	static const char hex[] = "0123456789abcdef";

	out[0] = hex[value >> 4];
	out[1] = hex[value & 0xfu];
}

static void format_hex_u32_le(char *out, uint32_t value) {
	format_hex_byte(out + 0, (uint8_t)(value >> 0));
	format_hex_byte(out + 2, (uint8_t)(value >> 8));
	format_hex_byte(out + 4, (uint8_t)(value >> 16));
	format_hex_byte(out + 6, (uint8_t)(value >> 24));
}

/* --- packet framing ------------------------------------------------------ */

static void
gdb_send_packet(struct gdb_conn *conn, const char *payload, size_t length) {
	char framed[2 * GDB_PACKET_MAX + 8];
	size_t used = 0;
	uint8_t checksum = 0;
	size_t i;

	framed[used++] = '$';

	for (i = 0; i < length; i++) {
		uint8_t c = (uint8_t)payload[i];

		if (c == '$' || c == '#' || c == '}' || c == '*') {
			framed[used++] = '}';
			c ^= 0x20u;
		}

		framed[used++] = (char)c;
		checksum = (uint8_t)(checksum + (uint8_t)payload[i]);
	}

	framed[used++] = '#';
	framed[used++] = "0123456789abcdef"[checksum >> 4];
	framed[used++] = "0123456789abcdef"[checksum & 0xfu];
	framed[used] = '\0';

	if (gdb_send_all(conn->fd, framed, used) != 0) {
		conn->quit = true;
		return;
	}

	/* GDB normally acknowledges with '+'. */
	for (;;) {
		uint8_t reply;

		if (gdb_recv_byte(conn->fd, &reply) != 0) {
			conn->quit = true;
			return;
		}

		if (reply == '+') {
			return;
		}

		if (reply == '-') {
			/* Retry once. */
			if (gdb_send_all(conn->fd, framed, used) != 0) {
				conn->quit = true;
			}
			continue;
		}

		/* Ignore any other byte; GDB should not send one here. */
	}
}

static void gdb_send_text(struct gdb_conn *conn, const char *text) {
	gdb_send_packet(conn, text, strlen(text));
}

static int gdb_recv_packet(struct gdb_conn *conn, char *out, size_t out_max) {
	char raw[GDB_PACKET_MAX];
	size_t raw_len = 0;
	size_t out_len = 0;
	uint8_t checksum = 0;
	uint8_t received = 0;
	uint8_t c;

	for (;;) {
		if (gdb_recv_byte(conn->fd, &c) != 0) {
			return -1;
		}

		if (c == '$') {
			break;
		}

		if (c == 0x03) {
			/* Interrupt while idle.  The caller can poll again. */
			continue;
		}
	}

	for (;;) {
		if (gdb_recv_byte(conn->fd, &c) != 0) {
			return -1;
		}

		if (c == '#') {
			break;
		}

		if (raw_len + 1 < sizeof(raw)) {
			raw[raw_len++] = (char)c;
		}
	}

	for (int n = 0; n < 2; n++) {
		int nibble;

		if (gdb_recv_byte(conn->fd, &c) != 0) {
			return -1;
		}

		nibble = hex_nibble((char)c);

		if (nibble < 0) {
			return -1;
		}

		received = (uint8_t)((received << 4) | (uint8_t)nibble);
	}

	for (size_t i = 0; i < raw_len; i++) {
		checksum = (uint8_t)(checksum + (uint8_t)raw[i]);
	}

	if (checksum != received) {
		(void)gdb_send_all(conn->fd, "-", 1);
		return -1;
	}

	(void)gdb_send_all(conn->fd, "+", 1);

	for (size_t i = 0; i < raw_len; i++) {
		if (raw[i] == '}') {
			if (++i >= raw_len) {
				return -1;
			}

			c = (uint8_t)raw[i] ^ 0x20u;
		} else {
			c = (uint8_t)raw[i];
		}

		if (out_len + 1 < out_max) {
			out[out_len++] = (char)c;
		}
	}

	out[out_len] = '\0';

	return (int)out_len;
}

/* --- target state -------------------------------------------------------- */

static enum wl_status gdb_dm_halt(wl_linke_t *link) {
	return wl_linke_dmi_write(link, WL_DMI_DMCONTROL, GDB_DMCONTROL_HALT);
}

static bool gdb_dm_is_halted(wl_linke_t *link) {
	uint32_t status = 0;

	if (wl_linke_dmi_read(link, WL_DMI_DMSTATUS, &status) != WL_OK) {
		return true;
	}

	return (status & GDB_DMSTATUS_ANYRUNNING) == 0;
}

static enum wl_status gdb_dm_wait_halted(wl_linke_t *link) {
	unsigned i;

	for (i = 0; i < 2000u; i++) {
		if (gdb_dm_is_halted(link)) {
			return WL_OK;
		}

		{
			struct timespec delay = {0, 1000000L};

			(void)nanosleep(&delay, NULL);
		}
	}

	wl_error("target did not halt");
	return WL_ERR_TARGET;
}

static enum wl_status gdb_resume(wl_linke_t *link) {
	return wl_linke_dmi_write(link, WL_DMI_DMCONTROL, GDB_DMCONTROL_RESUME);
}

static enum wl_status gdb_step(wl_linke_t *link) {
	uint32_t dcsr = 0;
	enum wl_status status;

	status = wl_dm_dcsr_read(link, &dcsr);

	if (status != WL_OK) {
		return status;
	}

	dcsr |= GDB_DCSR_STEP;
	status = wl_dm_dcsr_write(link, dcsr);

	if (status != WL_OK) {
		return status;
	}

	status = gdb_resume(link);

	if (status == WL_OK) {
		status = gdb_dm_wait_halted(link);
	}

	(void)wl_dm_dcsr_write(link, dcsr & ~GDB_DCSR_STEP);

	return status;
}

static bool gdb_addr_is_flash(const struct gdb_conn *conn, uint32_t address) {
	return wl_flash_range_ok(conn->chip, address, 1u);
}

static uint32_t gdb_normalise_flash(const struct gdb_conn *conn,
				    uint32_t address) {
	if (conn->chip->flash_base == 0x08000000u &&
	    (address & 0xff000000u) == 0u) {
		return address | 0x08000000u;
	}

	return address;
}

static enum wl_status gdb_mem_read(struct gdb_conn *conn,
				   uint32_t address,
				   uint8_t *buffer,
				   size_t length) {
	return wl_dm_read_block(conn->link, address, buffer, length);
}

static enum wl_status gdb_mem_patch(struct gdb_conn *conn,
				    uint32_t address,
				    const uint8_t *bytes,
				    size_t length) {
	if (gdb_addr_is_flash(conn, address)) {
		uint32_t page_size = conn->chip->erase_size;
		uint32_t page_start;
		uint8_t *page;
		enum wl_status status;

		if (page_size == 0u || page_size > 4096u) {
			return WL_ERR_USAGE;
		}

		page = malloc(page_size);

		if (page == NULL) {
			return WL_ERR_IO;
		}

		page_start = address & ~(page_size - 1u);
		status =
		    wl_dm_read_block(conn->link, page_start, page, page_size);

		if (status == WL_OK) {
			memcpy(page + (address - page_start), bytes, length);
			status = wl_flash_write(
			    conn->link, conn->chip,
			    gdb_normalise_flash(conn, page_start), page,
			    page_size, NULL);
		}

		free(page);
		return status;
	}

	if (length == 2u) {
		return wl_dm_write16(
		    conn->link, address,
		    (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8)));
	}

	if (length == 4u) {
		uint32_t word = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
				((uint32_t)bytes[2] << 16) |
				((uint32_t)bytes[3] << 24);

		return wl_dm_write32(conn->link, address, word);
	}

	return WL_ERR_USAGE;
}

static bool
gdb_breakpoint_insert(struct gdb_conn *conn, uint32_t address, uint32_t kind) {
	struct gdb_breakpoint *bp = NULL;
	uint8_t replacement[4] = {0x02u, 0x90u, 0x00u, 0x00u};
	size_t i;

	if ((kind != 2u && kind != 4u) || (address % kind) != 0u) {
		return false;
	}

	for (i = 0; i < GDB_MAX_BREAKPOINTS; i++) {
		if (!conn->breakpoints[i].used) {
			bp = &conn->breakpoints[i];
			break;
		}
	}

	if (bp == NULL) {
		return false;
	}

	if (kind == 4u) {
		replacement[0] = 0x73u;
		replacement[1] = 0x00u;
		replacement[2] = 0x10u;
		replacement[3] = 0x00u;
	}

	if (gdb_mem_read(conn, address, bp->original, kind) != WL_OK) {
		return false;
	}

	if (gdb_mem_patch(conn, address, replacement, kind) != WL_OK) {
		return false;
	}

	bp->used = true;
	bp->address = address;
	bp->kind = kind;

	return true;
}

static bool
gdb_breakpoint_remove(struct gdb_conn *conn, uint32_t address, uint32_t kind) {
	size_t i;

	for (i = 0; i < GDB_MAX_BREAKPOINTS; i++) {
		struct gdb_breakpoint *bp = &conn->breakpoints[i];

		if (bp->used && bp->address == address && bp->kind == kind) {
			if (gdb_mem_patch(conn, address, bp->original,
					  bp->kind) != WL_OK) {
				return false;
			}

			bp->used = false;
			return true;
		}
	}

	return false;
}

static void gdb_breakpoints_clear(struct gdb_conn *conn) {
	size_t i;

	for (i = 0; i < GDB_MAX_BREAKPOINTS; i++) {
		struct gdb_breakpoint *bp = &conn->breakpoints[i];

		if (bp->used) {
			(void)gdb_mem_patch(conn, bp->address, bp->original,
					    bp->kind);
			bp->used = false;
		}
	}
}

static void gdb_send_output(struct gdb_conn *conn, const char *text) {
	size_t length = strlen(text);
	size_t offset = 0;

	while (offset < length) {
		char chunk[GDB_PACKET_MAX];
		size_t count = length - offset;
		size_t used = 0;
		size_t i;

		if (count > (GDB_PACKET_MAX - 1u) / 2u) {
			count = (GDB_PACKET_MAX - 1u) / 2u;
		}

		chunk[used++] = 'O';

		for (i = 0; i < count; i++) {
			format_hex_byte(chunk + used,
					(uint8_t)text[offset + i]);
			used += 2;
		}

		gdb_send_packet(conn, chunk, used);
		offset += count;
	}
}

static bool gdb_decode_hex_string(const char *hex, char *out, size_t out_max) {
	size_t length = strlen(hex);
	size_t i;

	if ((length & 1u) != 0u || out_max == 0u) {
		return false;
	}

	if (length / 2u >= out_max) {
		return false;
	}

	for (i = 0; i < length; i += 2) {
		int high = hex_nibble(hex[i]);
		int low = hex_nibble(hex[i + 1]);

		if (high < 0 || low < 0) {
			return false;
		}

		out[i / 2u] = (char)((high << 4) | low);
	}

	out[length / 2u] = '\0';

	return true;
}

static void gdb_monitor_help(struct gdb_conn *conn) {
	gdb_send_output(conn, "wchlink monitor commands:\n"
			      "  help                     this text\n"
			      "  reset                    reset and halt\n"
			      "  halt                     halt the core\n"
			      "  resume                   resume the core\n"
			      "  info                     dmstatus/dcsr/pc\n"
			      "  regs                     x0-x15 and pc\n"
			      "  read32 <addr>            read one word\n"
			      "  write32 <addr> <value>   write one word\n"
			      "  getcsr <csr>             read a CSR\n"
			      "  setcsr <csr> <value>     write a CSR\n");
}

static void gdb_monitor_regs(struct gdb_conn *conn) {
	char line[64];
	unsigned reg;

	for (reg = 0; reg < GDB_RV32E_GPRS; reg++) {
		uint32_t value = 0;

		if (wl_dm_gpr_read(conn->link, reg, &value) != WL_OK) {
			gdb_send_output(conn, "register read failed\n");
			return;
		}

		snprintf(line, sizeof(line), "x%-2u = 0x%08x\n", reg, value);
		gdb_send_output(conn, line);
	}

	{
		uint32_t pc = 0;

		if (wl_dm_pc_read(conn->link, &pc) == WL_OK) {
			snprintf(line, sizeof(line), "pc  = 0x%08x\n", pc);
			gdb_send_output(conn, line);
		}
	}
}

static void gdb_monitor_info(struct gdb_conn *conn) {
	char line[96];
	uint32_t status = 0;
	uint32_t dcsr = 0;
	uint32_t pc = 0;

	if (wl_linke_dmi_read(conn->link, WL_DMI_DMSTATUS, &status) == WL_OK) {
		snprintf(line, sizeof(line), "dmstatus = 0x%08x\n", status);
		gdb_send_output(conn, line);
	}

	if (wl_dm_dcsr_read(conn->link, &dcsr) == WL_OK) {
		snprintf(line, sizeof(line), "dcsr     = 0x%08x\n", dcsr);
		gdb_send_output(conn, line);
	}

	if (wl_dm_pc_read(conn->link, &pc) == WL_OK) {
		snprintf(line, sizeof(line), "pc       = 0x%08x\n", pc);
		gdb_send_output(conn, line);
	}
}

static void gdb_monitor_command(struct gdb_conn *conn, char *command) {
	char *save = NULL;
	char *word;
	char *arg1;
	char *arg2;

	word = strtok_r(command, " \t\r\n", &save);

	if (word == NULL || strcmp(word, "help") == 0) {
		gdb_monitor_help(conn);
		return;
	}

	if (strcmp(word, "reset") == 0) {
		struct timespec delay = {0, 10000000L};

		/*
		 * 0x80000003 asserts ndmreset and haltreq.  As minichlink's own
		 * sequence shows, ndmreset must then be cleared while haltreq stays
		 * set, otherwise the core is held in reset and abstract commands
		 * report "target is not halted".
		 */
		(void)wl_linke_dmi_write(conn->link, WL_DMI_DMCONTROL,
					 0x80000003u);
		(void)nanosleep(&delay, NULL);
		(void)wl_linke_dmi_write(conn->link, WL_DMI_DMCONTROL,
					 0x80000001u);
		(void)wl_linke_dmi_write(conn->link, WL_DMI_DMCONTROL,
					 0x80000001u);
		(void)gdb_dm_wait_halted(conn->link);
		gdb_send_output(conn, "reset/halt\n");
		return;
	}

	if (strcmp(word, "halt") == 0) {
		(void)gdb_dm_halt(conn->link);
		(void)gdb_dm_wait_halted(conn->link);
		gdb_send_output(conn, "halted\n");
		return;
	}

	if (strcmp(word, "resume") == 0 || strcmp(word, "continue") == 0) {
		(void)gdb_resume(conn->link);
		gdb_send_output(conn, "running\n");
		return;
	}

	if (strcmp(word, "info") == 0 || strcmp(word, "status") == 0) {
		gdb_monitor_info(conn);
		return;
	}

	if (strcmp(word, "regs") == 0) {
		gdb_monitor_regs(conn);
		return;
	}

	arg1 = strtok_r(NULL, " \t\r\n", &save);

	if (strcmp(word, "read32") == 0 && arg1 != NULL) {
		uint32_t address;
		uint32_t value = 0;
		char line[64];

		if (parse_hex_u32(arg1, NULL, &address) &&
		    wl_dm_read32(conn->link, address, &value) == WL_OK) {
			snprintf(line, sizeof(line), "0x%08x = 0x%08x\n",
				 address, value);
			gdb_send_output(conn, line);
		} else {
			gdb_send_output(conn, "read32 failed\n");
		}
		return;
	}

	arg2 = strtok_r(NULL, " \t\r\n", &save);

	if (strcmp(word, "write32") == 0 && arg1 != NULL && arg2 != NULL) {
		uint32_t address;
		uint32_t value;

		if (parse_hex_u32(arg1, NULL, &address) &&
		    parse_hex_u32(arg2, NULL, &value) &&
		    wl_dm_write32(conn->link, address, value) == WL_OK) {
			gdb_send_output(conn, "OK\n");
		} else {
			gdb_send_output(conn, "write32 failed\n");
		}
		return;
	}

	if (strcmp(word, "getcsr") == 0 && arg1 != NULL) {
		uint32_t csr;
		uint32_t value = 0;
		char line[64];

		if (parse_hex_u32(arg1, NULL, &csr) &&
		    wl_dm_csr_read(conn->link, csr, &value) == WL_OK) {
			snprintf(line, sizeof(line), "csr 0x%03x = 0x%08x\n",
				 csr, value);
			gdb_send_output(conn, line);
		} else {
			gdb_send_output(conn, "getcsr failed\n");
		}
		return;
	}

	if (strcmp(word, "setcsr") == 0 && arg1 != NULL && arg2 != NULL) {
		uint32_t csr;
		uint32_t value;

		if (parse_hex_u32(arg1, NULL, &csr) &&
		    parse_hex_u32(arg2, NULL, &value) &&
		    wl_dm_csr_write(conn->link, csr, value) == WL_OK) {
			gdb_send_output(conn, "OK\n");
		} else {
			gdb_send_output(conn, "setcsr failed\n");
		}
		return;
	}

	gdb_send_output(conn, "unknown monitor command\n");
}

static void gdb_handle_qRcmd(struct gdb_conn *conn, const char *payload) {
	char command[256];

	if (!gdb_decode_hex_string(payload + 6, command, sizeof(command))) {
		gdb_send_text(conn, "E01");
		return;
	}

	gdb_monitor_command(conn, command);
	gdb_send_text(conn, "OK");
}

static void gdb_send_stop(struct gdb_conn *conn, unsigned signal) {
	char reply[32];

	snprintf(reply, sizeof(reply), "T%02xthread:1;", signal);
	gdb_send_text(conn, reply);
}

static void gdb_wait_for_stop(struct gdb_conn *conn) {
	for (;;) {
		struct pollfd pfd = {conn->fd, POLLIN, 0};
		int ready = poll(&pfd, 1, 10);

		if (ready > 0 && (pfd.revents & POLLIN) != 0) {
			uint8_t c;

			if (gdb_recv_byte(conn->fd, &c) == 0 && c == 0x03) {
				(void)gdb_dm_halt(conn->link);
				(void)gdb_dm_wait_halted(conn->link);
				gdb_send_stop(conn, 2);
				return;
			}
		}

		if (gdb_dm_is_halted(conn->link)) {
			gdb_send_stop(conn, 5);
			return;
		}
	}
}

/* --- target XML and memory map ------------------------------------------ */

static size_t build_target_xml(char *out, size_t out_max) {
	size_t used = 0;
	unsigned reg;

	used += (size_t)snprintf(out + used, out_max - used,
				 "<?xml version=\"1.0\"?>"
				 "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
				 "<target>"
				 "<architecture>riscv:rv32</architecture>"
				 "<feature name=\"org.gnu.gdb.riscv.cpu\">");

	for (reg = 0; reg < GDB_RV32E_GPRS; reg++) {
		used += (size_t)snprintf(
		    out + used, out_max - used,
		    "<reg name=\"x%u\" bitsize=\"32\" type=\"uint32\" "
		    "regnum=\"%u\"/>",
		    reg, reg);
	}

	used += (size_t)snprintf(out + used, out_max - used,
				 "<reg name=\"pc\" bitsize=\"32\" "
				 "type=\"code_ptr\" regnum=\"%u\"/>"
				 "</feature></target>",
				 GDB_REG_PC);

	return used;
}

static size_t
build_memory_map(char *out, size_t out_max, const wl_chip_t *chip) {
	uint32_t block = chip->erase_size != 0 ? chip->erase_size : 4096u;

	return (size_t)snprintf(
	    out, out_max,
	    "<?xml version=\"1.0\"?>"
	    "<!DOCTYPE memory-map PUBLIC \"+//IDN gnu.org//DTD GDB Memory Map "
	    "V1.0//EN\" "
	    "\"http://sourceware.org/gdb/gdb-memory-map.dtd\">"
	    "<memory-map>"
	    "<memory type=\"flash\" start=\"0x%08x\" length=\"0x%x\">"
	    "<property name=\"blocksize\">%u</property></memory>"
	    "<memory type=\"ram\" start=\"0x%08x\" length=\"0x%x\"/>"
	    "</memory-map>",
	    chip->flash_base, chip->flash_size, block, chip->ram_offset,
	    chip->ram_size);
}

static void gdb_qxfer_reply(struct gdb_conn *conn,
			    const char *data,
			    size_t length,
			    uint32_t offset,
			    uint32_t requested) {
	char reply[GDB_PACKET_MAX + 2];
	size_t room = requested;
	size_t count;

	if (offset >= length) {
		gdb_send_text(conn, "l");
		return;
	}

	if (room > GDB_PACKET_MAX - 1) {
		room = GDB_PACKET_MAX - 1;
	}

	count = length - offset;

	if (count > room) {
		count = room;
	}

	if (count == 0) {
		gdb_send_text(conn, "l");
		return;
	}

	reply[0] = (offset + count >= length) ? 'l' : 'm';
	memcpy(reply + 1, data + offset, count);
	gdb_send_packet(conn, reply, count + 1);
}

static void gdb_handle_qxfer(struct gdb_conn *conn, const char *payload) {
	const char *object_start = payload + 6;
	const char *colon1;
	const char *colon2;
	const char *last_colon;
	const char *annex_start;
	size_t object_len;
	size_t annex_len;
	char object[64];
	char annex[64];
	uint32_t offset = 0;
	uint32_t length = 0;
	char xml[GDB_PACKET_MAX];

	colon1 = strchr(object_start, ':');
	colon2 = colon1 != NULL ? strchr(colon1 + 1, ':') : NULL;
	last_colon = colon2 != NULL ? strrchr(colon2 + 1, ':') : NULL;

	if (colon1 == NULL || colon2 == NULL || last_colon == NULL) {
		gdb_send_text(conn, "");
		return;
	}

	object_len = (size_t)(colon1 - object_start);

	if (object_len >= sizeof(object)) {
		gdb_send_text(conn, "");
		return;
	}

	memcpy(object, object_start, object_len);
	object[object_len] = '\0';

	/* Operation must be "read"; annex is between colon2 and last_colon. */
	if (strncmp(colon1 + 1, "read:", 5) != 0) {
		gdb_send_text(conn, "");
		return;
	}

	annex_start = colon2 + 1;
	annex_len = (size_t)(last_colon - annex_start);

	if (annex_len >= sizeof(annex)) {
		gdb_send_text(conn, "");
		return;
	}

	memcpy(annex, annex_start, annex_len);
	annex[annex_len] = '\0';

	if (!parse_hex_u32(last_colon + 1, &annex_start, &offset) ||
	    *annex_start != ',') {
		gdb_send_text(conn, "");
		return;
	}

	if (!parse_hex_u32(annex_start + 1, NULL, &length)) {
		gdb_send_text(conn, "");
		return;
	}

	if (strcmp(object, "features") == 0 &&
	    strcmp(annex, "target.xml") == 0) {
		size_t len = build_target_xml(xml, sizeof(xml));

		gdb_qxfer_reply(conn, xml, len, offset, length);
		return;
	}

	if (strcmp(object, "memory-map") == 0 && annex[0] == '\0') {
		size_t len = build_memory_map(xml, sizeof(xml), conn->chip);

		gdb_qxfer_reply(conn, xml, len, offset, length);
		return;
	}

	gdb_send_text(conn, "");
}

/* --- register and memory packets ---------------------------------------- */

static void gdb_handle_g(struct gdb_conn *conn) {
	char reply[8 * (GDB_RV32E_GPRS + 1) + 1];
	size_t used = 0;
	unsigned reg;

	for (reg = 0; reg < GDB_RV32E_GPRS; reg++) {
		uint32_t value = 0;

		if (wl_dm_gpr_read(conn->link, reg, &value) != WL_OK) {
			gdb_send_text(conn, "E01");
			return;
		}

		format_hex_u32_le(reply + used, value);
		used += 8;
	}

	{
		uint32_t pc = 0;

		if (wl_dm_pc_read(conn->link, &pc) != WL_OK) {
			gdb_send_text(conn, "E01");
			return;
		}

		format_hex_u32_le(reply + used, pc);
		used += 8;
	}

	reply[used] = '\0';
	gdb_send_packet(conn, reply, used);
}

static void gdb_handle_p(struct gdb_conn *conn, const char *payload) {
	uint32_t reg;
	uint32_t value;

	if (!parse_hex_u32(payload + 1, NULL, &reg)) {
		gdb_send_text(conn, "E01");
		return;
	}

	if (reg > GDB_REG_PC) {
		gdb_send_text(conn, "E01");
		return;
	}

	/* A read request is "p<reg>" with no '='. */
	const char *eq = strchr(payload, '=');

	if (eq == NULL) {
		char reply[9];

		if (reg == GDB_REG_PC) {
			if (wl_dm_pc_read(conn->link, &value) != WL_OK) {
				gdb_send_text(conn, "E01");
				return;
			}
		} else {
			if (wl_dm_gpr_read(conn->link, reg, &value) != WL_OK) {
				gdb_send_text(conn, "E01");
				return;
			}
		}

		format_hex_u32_le(reply, value);
		reply[8] = '\0';
		gdb_send_text(conn, reply);
		return;
	}

	/* Write request is "P<reg>=<hex>". */
	if (!parse_hex_u32(eq + 1, NULL, &value)) {
		gdb_send_text(conn, "E01");
		return;
	}

	if (reg == GDB_REG_PC) {
		if (wl_dm_pc_write(conn->link, value) != WL_OK) {
			gdb_send_text(conn, "E01");
			return;
		}
	} else if (wl_dm_gpr_write(conn->link, reg, value) != WL_OK) {
		gdb_send_text(conn, "E01");
		return;
	}

	gdb_send_text(conn, "OK");
}

static void gdb_handle_m(struct gdb_conn *conn, const char *payload) {
	uint32_t address;
	uint32_t length;
	const char *end;
	uint8_t buffer[GDB_MEMORY_MAX];
	size_t i;
	size_t used = 0;

	if (!parse_hex_u32(payload + 1, &end, &address) || *end != ',') {
		gdb_send_text(conn, "E01");
		return;
	}

	if (!parse_hex_u32(end + 1, NULL, &length)) {
		gdb_send_text(conn, "E01");
		return;
	}

	if (length > sizeof(buffer)) {
		gdb_send_text(conn, "E01");
		return;
	}

	if (wl_dm_read_block(conn->link, address, buffer, length) != WL_OK) {
		gdb_send_text(conn, "E01");
		return;
	}

	{
		char *reply = malloc(length * 2u + 1u);

		if (reply == NULL) {
			gdb_send_text(conn, "E01");
			return;
		}

		for (i = 0; i < length; i++) {
			format_hex_byte(reply + used, buffer[i]);
			used += 2;
		}

		reply[used] = '\0';
		gdb_send_packet(conn, reply, used);
		free(reply);
	}
}

static void gdb_handle_M(struct gdb_conn *conn, const char *payload) {
	uint32_t address;
	uint32_t length;
	const char *end;
	uint8_t buffer[GDB_MEMORY_MAX];
	size_t i;

	if (!parse_hex_u32(payload + 1, &end, &address) || *end != ',') {
		gdb_send_text(conn, "E01");
		return;
	}

	if (!parse_hex_u32(end + 1, &end, &length) || *end != ':') {
		gdb_send_text(conn, "E01");
		return;
	}

	if (length > sizeof(buffer)) {
		gdb_send_text(conn, "E01");
		return;
	}

	if (!parse_hex_bytes(end + 1, buffer, length)) {
		gdb_send_text(conn, "E01");
		return;
	}

	for (i = 0; i < length; i++) {
		if (wl_dm_write8(conn->link, address + i, buffer[i]) != WL_OK) {
			gdb_send_text(conn, "E01");
			return;
		}
	}

	gdb_send_text(conn, "OK");
}

static void gdb_handle_query(struct gdb_conn *conn, const char *payload) {
	if (strcmp(payload, "qSupported") == 0) {
		gdb_send_text(conn, "PacketSize=1000;"
				    "qXfer:features:read+;"
				    "qXfer:memory-map:read+;"
				    "vContSupported+");
		return;
	}

	if (strcmp(payload, "qAttached") == 0) {
		gdb_send_text(conn, "1");
		return;
	}

	if (strcmp(payload, "qC") == 0) {
		gdb_send_text(conn, "QC1");
		return;
	}

	if (strcmp(payload, "qfThreadInfo") == 0) {
		gdb_send_text(conn, "m1");
		return;
	}

	if (strcmp(payload, "qsThreadInfo") == 0) {
		gdb_send_text(conn, "l");
		return;
	}

	if (strcmp(payload, "qTStatus") == 0) {
		gdb_send_text(conn, "");
		return;
	}

	if (strncmp(payload, "qRcmd,", 6) == 0) {
		gdb_handle_qRcmd(conn, payload);
		return;
	}

	if (strncmp(payload, "qXfer:", 6) == 0) {
		gdb_handle_qxfer(conn, payload);
		return;
	}

	gdb_send_text(conn, "");
}

static bool gdb_handle_packet(struct gdb_conn *conn, const char *payload) {
	switch (payload[0]) {
	case '?':
		gdb_send_stop(conn, 5);
		return true;
	case 'g':
		gdb_handle_g(conn);
		return true;
	case 'p':
	case 'P':
		gdb_handle_p(conn, payload);
		return true;
	case 'm':
		gdb_handle_m(conn, payload);
		return true;
	case 'M':
		gdb_handle_M(conn, payload);
		return true;
	case 'c':
		if (gdb_resume(conn->link) == WL_OK) {
			gdb_wait_for_stop(conn);
		} else {
			gdb_send_text(conn, "E01");
		}
		return true;
	case 's':
		if (gdb_step(conn->link) == WL_OK) {
			gdb_send_stop(conn, 5);
		} else {
			gdb_send_text(conn, "E01");
		}
		return true;
	case 'Z':
	case 'z': {
		bool insert = payload[0] == 'Z';
		uint32_t address;
		uint32_t kind;
		const char *end;

		if (payload[1] != '0' ||
		    !parse_hex_u32(payload + 3, &end, &address) ||
		    *end != ',') {
			gdb_send_text(conn, "");
			return true;
		}

		if (!parse_hex_u32(end + 1, NULL, &kind)) {
			gdb_send_text(conn, "");
			return true;
		}

		if (insert) {
			gdb_send_text(conn,
				      gdb_breakpoint_insert(conn, address, kind)
					  ? "OK"
					  : "");
		} else {
			gdb_send_text(conn,
				      gdb_breakpoint_remove(conn, address, kind)
					  ? "OK"
					  : "");
		}
		return true;
	}
	case 'D':
		gdb_breakpoints_clear(conn);
		(void)gdb_resume(conn->link);
		gdb_send_text(conn, "OK");
		conn->quit = true;
		return false;
	case 'k':
		gdb_breakpoints_clear(conn);
		conn->quit = true;
		return false;
	case 'H':
		gdb_send_text(conn, "OK");
		return true;
	case 'q':
		gdb_handle_query(conn, payload);
		return true;
	case 'v':
		if (strcmp(payload, "vCont?") == 0) {
			gdb_send_text(conn, "vCont;c;C;s;S");
			return true;
		}

		if (strncmp(payload, "vCont;", 6) == 0) {
			if (payload[6] == 's' || payload[6] == 'S') {
				if (gdb_step(conn->link) == WL_OK) {
					gdb_send_stop(conn, 5);
				} else {
					gdb_send_text(conn, "E01");
				}
			} else {
				if (gdb_resume(conn->link) == WL_OK) {
					gdb_wait_for_stop(conn);
				} else {
					gdb_send_text(conn, "E01");
				}
			}
			return true;
		}

		gdb_send_text(conn, "");
		return true;
	default:
		gdb_send_text(conn, "");
		return true;
	}
}

/* --- server -------------------------------------------------------------- */

static int gdb_listen(uint16_t port) {
	int fd;
	int one = 1;
	struct sockaddr_in address;

	fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0) {
		return -1;
	}

	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(port);

	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		close(fd);
		return -1;
	}

	if (listen(fd, 1) != 0) {
		close(fd);
		return -1;
	}

	return fd;
}

enum wl_status
wl_gdb_server_run(const char *serial, const wl_chip_t *chip, uint16_t port) {
	wl_linke_t link;
	struct gdb_conn conn;
	int listen_fd;
	int client_fd;
	enum wl_status status;

	if (chip == NULL) {
		return WL_ERR_USAGE;
	}

	status = wl_linke_open(&link, serial);

	if (status != WL_OK) {
		return status;
	}

	status = wl_linke_set_interface(&link, chip);

	if (status != WL_OK) {
		wl_linke_close(&link);
		return status;
	}

	/*
 * LinkE attach is the only door into the target's debug module; it
 * starts from the reset/halt path.  After that point, halt/resume use
 * DMCONTROL so a running application is not reset again.
 */
	status = wl_linke_halt(&link);

	if (status != WL_OK) {
		wl_linke_close(&link);
		return status;
	}

	(void)gdb_dm_halt(&link);
	(void)gdb_dm_wait_halted(&link);

	listen_fd = gdb_listen(port);

	if (listen_fd < 0) {
		wl_error("cannot listen on 127.0.0.1:%u: %s", port,
			 strerror(errno));
		wl_linke_close(&link);
		return WL_ERR_IO;
	}

	wl_info("gdb: listening on 127.0.0.1:%u", port);
	wl_info("gdb: use `target extended-remote :%u`", port);

	client_fd = accept(listen_fd, NULL, NULL);
	close(listen_fd);

	if (client_fd < 0) {
		wl_error("cannot accept GDB connection: %s", strerror(errno));
		wl_linke_close(&link);
		return WL_ERR_IO;
	}

	memset(&conn, 0, sizeof(conn));
	conn.fd = client_fd;
	conn.link = &link;
	conn.chip = chip;

	while (!conn.quit) {
		char packet[GDB_PACKET_MAX];

		if (gdb_recv_packet(&conn, packet, sizeof(packet)) < 0) {
			break;
		}

		(void)gdb_handle_packet(&conn, packet);
	}

	gdb_breakpoints_clear(&conn);
	close(client_fd);
	wl_linke_close(&link);

	return WL_OK;
}
