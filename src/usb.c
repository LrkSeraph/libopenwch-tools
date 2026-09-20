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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * libusb's header lives in <libusb-1.0/libusb.h> when the -dev package is
 * installed, but on a host that only has the runtime library it may have to be
 * supplied another way -- see the LIBUSB_CFLAGS note in the Makefile.  Accept
 * both spellings so the build does not care which one is in reach.
 */
#if defined(__has_include)
#if __has_include(<libusb-1.0/libusb.h>)
#include <libusb-1.0/libusb.h>
#else
#include <libusb.h>
#endif
#else
#include <libusb-1.0/libusb.h>
#endif

#include "log.h"
#include "usb.h"

/** The open handle.  Opaque outside this file; see usb.h. */
struct wl_usb_link {
	libusb_device_handle *handle;
	bool claimed;
};

static libusb_context *context;

const char *wl_usb_mode_name(enum wl_usb_mode mode) {
	switch (mode) {
	case WL_USB_MODE_RV:
		return "RISC-V debug";
	case WL_USB_MODE_ARM:
		return "ARM/SWD";
	case WL_USB_MODE_IAP:
		return "USB ISP bootloader";
	default:
		return "unknown";
	}
}

bool wl_usb_mode_is_usable(enum wl_usb_mode mode) {
	return mode == WL_USB_MODE_RV;
}

int wl_usb_global_init(void) {
	int rc;

	if (context != NULL) {
		return 0;
	}

	rc = libusb_init(&context);
	if (rc < 0) {
		context = NULL;
		return rc;
	}

	return 0;
}

void wl_usb_global_exit(void) {
	if (context != NULL) {
		libusb_exit(context);
		context = NULL;
	}
}

static enum wl_usb_mode classify(uint16_t vid, uint16_t pid) {
	if (vid == WL_USB_VID_WCH && pid == WL_USB_PID_LINK_RV) {
		return WL_USB_MODE_RV;
	}

	if (vid == WL_USB_VID_WCH && pid == WL_USB_PID_LINK_ARM) {
		return WL_USB_MODE_ARM;
	}

	if (vid == WL_USB_VID_IAP && pid == WL_USB_PID_IAP) {
		return WL_USB_MODE_IAP;
	}

	if (vid == WL_USB_VID_WCH && pid == WL_USB_PID_IAP) {
		return WL_USB_MODE_IAP;
	}

	return WL_USB_MODE_UNKNOWN;
}

/*
 * Read a string descriptor, tolerating every way it can fail.  An unreadable
 * serial number is not an error: it only means we cannot tell two programmers
 * apart, and the caller decides what to do about that.
 */
static void read_string(libusb_device_handle *handle,
			uint8_t index,
			char *out,
			size_t out_len) {
	int rc;

	out[0] = '\0';

	if (index == 0) {
		return;
	}

	rc = libusb_get_string_descriptor_ascii(
	    handle, index, (unsigned char *)out, (int)out_len);
	if (rc < 0) {
		out[0] = '\0';
		return;
	}

	out[out_len - 1] = '\0';
}

int wl_usb_scan(wl_usb_info_t *links, size_t max_links, size_t *found) {
	libusb_device **list = NULL;
	ssize_t count;
	ssize_t i;
	size_t n = 0;

	*found = 0;

	if (context == NULL) {
		/*
		 * Not a libusb-defined condition: libusb 1.0 has no
		 * "not initialised" code, so callers get OTHER and the text
		 * below explains it.  Reaching here is a bug in the caller.
		 */
		wl_error("libusb was not initialised; call "
			 "wl_usb_global_init() first");
		return LIBUSB_ERROR_OTHER;
	}

	count = libusb_get_device_list(context, &list);

	if (count < 0) {
		return (int)count;
	}

	for (i = 0; i < count; i++) {
		struct libusb_device_descriptor desc;
		libusb_device_handle *handle = NULL;
		enum wl_usb_mode mode;
		wl_usb_info_t *info;
		int rc;

		rc = libusb_get_device_descriptor(list[i], &desc);

		if (rc < 0) {
			continue;
		}

		mode = classify(desc.idVendor, desc.idProduct);

		if (mode == WL_USB_MODE_UNKNOWN) {
			continue;
		}

		if (n >= max_links) {
			wl_warn("more than %zu programmers attached; ignoring "
				"the rest",
				max_links);
			break;
		}

		info = &links[n];
		memset(info, 0, sizeof(*info));
		info->mode = mode;
		info->vid = desc.idVendor;
		info->pid = desc.idProduct;
		info->bus = libusb_get_bus_number(list[i]);
		info->address = libusb_get_device_address(list[i]);

		/*
		 * Opening here is what tells "no programmer" apart from "no
		 * permission".  The handle is closed again immediately: the
		 * caller opens what it chooses through wl_usb_open(), so that
		 * this pass never holds two devices open.
		 */
		if (libusb_open(list[i], &handle) == 0) {
			info->accessible = true;
			read_string(handle, desc.iSerialNumber, info->serial,
				    sizeof(info->serial));
			read_string(handle, desc.iProduct, info->product,
				    sizeof(info->product));
			libusb_close(handle);
		}

		n++;
	}

	libusb_free_device_list(list, 1);

	*found = n;

	return 0;
}

int wl_usb_open(const wl_usb_info_t *info, wl_usb_link_t **out) {
	libusb_device **list = NULL;
	libusb_device_handle *handle = NULL;
	ssize_t count;
	ssize_t i;
	wl_usb_link_t *link;
	int rc = LIBUSB_ERROR_NO_DEVICE;

	if (out == NULL || info == NULL) {
		return LIBUSB_ERROR_INVALID_PARAM;
	}

	*out = NULL;

	if (context == NULL) {
		wl_error("libusb was not initialised; call "
			 "wl_usb_global_init() first");
		return LIBUSB_ERROR_OTHER;
	}

	count = libusb_get_device_list(context, &list);

	if (count < 0) {
		return (int)count;
	}

	for (i = 0; i < count; i++) {
		if (libusb_get_bus_number(list[i]) != info->bus ||
		    libusb_get_device_address(list[i]) != info->address) {
			continue;
		}

		rc = libusb_open(list[i], &handle);

		break;
	}

	libusb_free_device_list(list, 1);

	if (rc != 0) {
		return rc;
	}

	/*
	 * Bulk transfers need the interface claimed, and on Linux the kernel
	 * may already hold it: the HID driver binds to these programmers on
	 * some distributions.  Ask libusb to detach it automatically, which is
	 * the one call that works on every platform that supports it, and
	 * treat the claim itself as the authority on whether we can talk.
	 */
#if defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000102
	(void)libusb_set_auto_detach_kernel_driver(handle, 1);
#endif

	rc = libusb_claim_interface(handle, WL_USB_INTERFACE);

	if (rc < 0) {
		wl_error("cannot claim the programmer's interface %d: %s",
			 WL_USB_INTERFACE, wl_usb_strerror(rc));
		wl_error("another program may be using it -- close any vendor "
			 "flash tool, or reload the kernel driver");
		libusb_close(handle);
		return rc;
	}

	link = calloc(1, sizeof(*link));

	if (link == NULL) {
		(void)libusb_release_interface(handle, WL_USB_INTERFACE);
		libusb_close(handle);
		return LIBUSB_ERROR_NO_MEM;
	}

	link->handle = handle;
	link->claimed = true;

	*out = link;

	return 0;
}

void wl_usb_close(wl_usb_link_t *link) {
	if (link == NULL) {
		return;
	}

	if (link->handle != NULL) {
		if (link->claimed) {
			(void)libusb_release_interface(link->handle,
						       WL_USB_INTERFACE);
		}

		libusb_close(link->handle);
	}

	free(link);
}

bool wl_usb_error_is_access(int code) {
	return code == LIBUSB_ERROR_ACCESS;
}

const char *wl_usb_strerror(int code) {
	if (code < 0) {
		return libusb_error_name(code);
	}

	return "success";
}

/* --- the transport ------------------------------------------------------- */

/*
 * Endpoints, from the programmer's descriptors: commands go out on 0x01, the
 * replies come back on 0x81, and bulk data for a write command goes out on
 * 0x02.  A reply is always collected, because the programmer will not accept
 * the next request until its answer has been read.
 */
#define WL_USB_EP_COMMAND 0x01
#define WL_USB_EP_DATA 0x02
#define WL_USB_EP_REPLY 0x81

/** Per-transfer timeout.  A flash erase is the longest thing behind one. */
#define WL_USB_TIMEOUT_MS 5000

static int usb_transport_command(void *ctx,
				 const uint8_t *request,
				 size_t request_len,
				 uint8_t *reply,
				 size_t reply_max,
				 size_t *reply_len) {
	wl_usb_link_t *link = (wl_usb_link_t *)ctx;
	uint8_t scratch[64];
	int transferred = 0;
	int rc;

	if (link == NULL || link->handle == NULL) {
		return LIBUSB_ERROR_NO_DEVICE;
	}

	rc = libusb_bulk_transfer(link->handle, WL_USB_EP_COMMAND,
				  (unsigned char *)request, (int)request_len,
				  &transferred, WL_USB_TIMEOUT_MS);

	if (rc < 0) {
		return rc;
	}

	if (reply == NULL || reply_max == 0) {
		reply = scratch;
		reply_max = sizeof(scratch);
	}

	rc = libusb_bulk_transfer(link->handle, WL_USB_EP_REPLY, reply,
				  (int)reply_max, &transferred,
				  WL_USB_TIMEOUT_MS);

	if (rc < 0) {
		return rc;
	}

	if (reply_len != NULL) {
		*reply_len = (size_t)transferred;
	}

	return 0;
}

static int
usb_transport_bulk_out(void *ctx, const uint8_t *data, size_t length) {
	wl_usb_link_t *link = (wl_usb_link_t *)ctx;
	int transferred = 0;
	int rc;

	if (link == NULL || link->handle == NULL) {
		return LIBUSB_ERROR_NO_DEVICE;
	}

	rc = libusb_bulk_transfer(link->handle, WL_USB_EP_DATA,
				  (unsigned char *)data, (int)length,
				  &transferred, WL_USB_TIMEOUT_MS);

	if (rc < 0) {
		return rc;
	}

	if ((size_t)transferred != length) {
		return LIBUSB_ERROR_IO;
	}

	return 0;
}

static const struct wl_transport usb_transport = {
    usb_transport_command,
    usb_transport_bulk_out,
};

const struct wl_transport *wl_usb_transport(void) {
	return &usb_transport;
}
