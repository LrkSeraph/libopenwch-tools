# libopenwch-tools

Host-side tools for WCH (Nanjing Qinheng) RISC-V microcontrollers.  The first
one is **wchlink**, a command-line flasher for the **WCH-LinkE** programmer.

This is a separate repository from
[libopenwch](https://github.com/LrkSeraph/libopenwch), and deliberately so: the
library is RISC-V target code built with a RISC-V toolchain, while this is a PC
program built with the host toolchain and `libusb`.  libopenwch references this
repository as a git submodule and delegates `make flash` to it; it contains no
USB code of its own.

> **Status: milestone 1 of 4.**  Device discovery and the `info` command work.
> **Flashing does not work yet** — the protocol is milestone 2 and 3 work, and
> the commands are wired up but report that plainly rather than pretending.
> Nothing here has been tested against a real WCH-LinkE.

## Building

```sh
make
```

The binary lands at `build/wchlink`.  `make install` puts it in
`$(PREFIX)/bin` and installs the udev rules alongside.

libusb is found through `pkg-config`.  If you only have the runtime library
(the `-dev` package is what provides `libusb-1.0.so` and the header), override
both variables:

```sh
make LIBUSB_CFLAGS=-I/path/to/include LIBUSB_LIBS=-l:libusb-1.0.so.0
```

`-l:libusb-1.0.so.0` is the usual form when only `libusb-1.0.so.0` is present.
A blank `LIBUSB_LIBS` falls back to `-lusb-1.0` so the failure is a clear linker
error rather than an empty link line.

## Permissions

On Linux the programmer is only reachable by an unprivileged user with udev
rules installed:

```sh
sudo make install-udev-rules      # installs udev/99-wchlink.rules
# then replug the programmer
```

Without them, libusb sees the device but cannot open it, and `wchlink` says so
rather than claiming no programmer is attached.

## Usage

```
wchlink info                 report the programmer and, with --chip, the target
wchlink chips                list the parts this tool knows about
wchlink flash <file.bin>     write a binary image to flash      (milestone 3)
wchlink read  <file.bin>     read target memory back            (milestone 2)
wchlink reset                reset the target                   (milestone 3)
wchlink unbrick              clear all code flash              (milestone 3)
wchlink terminal             single-wire debug terminal         (milestone 4)
```

Options: `--serial`, `--chip`, `--address`, `--size`, `--verify`, `--verbose`,
`--quiet`, `--version`, `--help`.

Exit codes: `0` success, `1` USB/programmer/target error, `2` bad command line,
`3` no usable programmer.

## Supported parts

`wchlink chips` prints the table.  The memory figures are taken from
libopenwch's `ld/devices.data` so the two cannot drift apart.  Neither the
table nor the flashing path has been checked against hardware; assume the list
is a plan, not a promise.

## Roadmap

| | |
|---|---|
| **M1** | Skeleton, host build, USB discovery, `info`, chip table, CLI — **done** |
| **M2** | Halt/resume, debug-register access, memory read-back |
| **M3** | Flash erase/write/verify, `reset`, `unbrick` |
| **M4** | Single-wire debug terminal |

A GDB stub is **not** in scope.  Milestones 2 to 4 cannot be written honestly
without a programmer to test against.

## Tests

```sh
make test
```

Covers the chip table and the command-line contract.  Both run with no hardware
attached.  Anything that touches a real programmer is not covered, because it
cannot be.

## Licence

LGPL-3.0-or-later; see `LICENSE`.  Note that a standalone program is more
conventionally GPL-3.0-or-later (the LGPL's benefit is for libraries that get
linked into proprietary code, which does not apply here), and the sibling
libopenwch project puts its host-side scripts under GPL-3.0.  The licence was
chosen to match libopenwch and can be changed if you prefer.

Implementation is clean-room.  minichlink, wlink and riscv-openocd-wch were
consulted for **protocol facts** — USB identifiers, register numbers, command
shapes — and no source was copied.  See `NOTICE`.

The protocol is WCH's and is not officially documented; the community worked it
out.  Nothing here is endorsed by or derived from WCH's own tools beyond the
public facts of the interface.
