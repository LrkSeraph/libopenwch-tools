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

#ifndef WCHLINK_TRANSPORT_H
#define WCHLINK_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/*
 * How the protocol speaks to a programmer.
 *
 * There is exactly one real implementation -- libusb, in usb.c -- and one
 * simulated one, in the tests.  The simulation is not a toy: a flashing
 * sequence cannot be checked against anything else without the hardware, and a
 * flashing sequence that nobody has run is not worth shipping.  Putting the
 * seam here, rather than inside the protocol code, is what makes the same
 * bytes flow through both.
 */

struct wl_transport {
	/**
	 * Send one request and collect its reply.
	 *
	 * A programmer always answers, even when the reply is of no
	 * interest, so this reads as well as writes.
	 *
	 * @param reply      may be NULL when the reply is not wanted
	 * @param reply_len  may be NULL when the length is not wanted
	 * @return 0 on success, negative on a transport failure
	 */
	int (*command)(void *ctx,
		       const uint8_t *request,
		       size_t request_len,
		       uint8_t *reply,
		       size_t reply_max,
		       size_t *reply_len);

	/**
	 * Send a bulk payload on the data endpoint.
	 *
	 * @return 0 on success, negative on a transport failure
	 */
	int (*bulk_out)(void *ctx, const uint8_t *data, size_t length);
};

#endif /* WCHLINK_TRANSPORT_H */
