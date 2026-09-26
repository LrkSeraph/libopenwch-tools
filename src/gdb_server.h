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

#ifndef WCHLINK_GDB_SERVER_H
#define WCHLINK_GDB_SERVER_H

#include <stdint.h>

#include "linke.h"
#include "target.h"

/**
 * Run a minimal GDB Remote Serial Protocol server for @p chip.
 *
 * The target is attached and halted before the TCP socket is opened, so the
 * first GDB connection sees a stopped core.  Continue and single-step use the
 * RISC-V debug module's DMCONTROL halt/resume bits, not the LinkE ATTACH/RUN
 * reset sequence, so a running program can be halted and continued without
 * being restarted.
 */
enum wl_status
wl_gdb_server_run(const char *serial, const wl_chip_t *chip, uint16_t port);

#endif /* WCHLINK_GDB_SERVER_H */
