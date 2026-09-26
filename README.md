# libopenwch-tools

Host-side tools for WCH (Nanjing Qinheng) RISC-V microcontrollers.  The first
one is **wchlink**, a command-line flasher for the **WCH-LinkE** programmer.

This is a separate repository from
[libopenwch](https://github.com/LrkSeraph/libopenwch), and deliberately so: the
library is RISC-V target code built with a RISC-V toolchain, while this is a PC
program built with the host toolchain and `libusb`.  libopenwch references this
repository as a git submodule and delegates `make flash` to it; it contains no
USB code of its own.

> **Status: milestones 1-3 implemented for CH32V00x; hardware run pending.**
> USB discovery, `info`, halt/resume, debug-register access, memory read-back,
> CH32V00x flash erase/write/verify, `reset`, and `unbrick` are implemented.
> The full flash sequence passes against the simulated programmer; it has not
> yet been exercised on a real CH32V00x.  A WCH-LinkE that enumerates in
> ARM/SWD mode is switched to RISC-V debug mode automatically before use.

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

## WCH-LinkE modes

A WCH-LinkE changes personality depending on what it was last doing:

| USB ID | Mode | Used for |
| --- | --- | --- |
| `1a86:8010` | RISC-V debug | normal target access and flashing |
| `1a86:8012` | ARM/SWD | ARM debugging; needs a switch before RISC-V use |
| `4348:55e0`, `1a86:55e0` | USB ISP/IAP bootloader | firmware recovery |

`wchlink` looks for a RISC-V programmer first.  If it finds an accessible
ARM/SWD device instead, it sends the WCH-LinkE mode-switch command, waits for
the device to re-enumerate as `1a86:8010`, and then continues with the command
that was requested.  An accessible IAP device is ejected from the bootloader
in the same way.  Neither operation needs a vendor GUI, and no target is
accessed until a RISC-V debug programmer is present.

Root/permission problems are reported separately: a programmer that is present
but cannot be opened is never silently reported as "not found".

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
wchlink flash <file.bin>     write a binary image to flash
wchlink read  <file.bin>     read target memory into a file
wchlink reset                reset the target and let it run
wchlink unbrick              hold the target in reset and power-cycle it
wchlink terminal             single-wire debug terminal         (milestone 4)
```

Options: `--serial`, `--chip`, `--address`, `--size`, `--verify`, `--no-verify`,
`--verbose`, `--quiet`, `--version`, `--help`.

Exit codes: `0` success, `1` USB/programmer/target error, `2` bad command line,
`3` no usable programmer.

```sh
# write an image and read it back (the default), for a 16K part
wchlink flash firmware.bin --chip ch32v003

# write it somewhere other than the start of flash
wchlink flash config.bin --chip ch32v003 --address 0x08003f00

# take a copy of what is in the part
wchlink read dump.bin --chip ch32v003 --address 0x08000000 --size 1024

# put the part back into reset and let it run
wchlink reset
```

## Flashing

`flash` erases, programs and verifies.  It works a page at a time, and a page
the image only partly covers is read back and merged first, so writing a few
bytes at an offset leaves the rest of the page alone.

The part is halted for the whole operation and reset when it is done, so the
image starts running on its own.

**Flashing is implemented for the CH32V00x family only** (CH32V003, 002, 004,
005, 006, 007).  The other families in the table are refused with an
explanation rather than written to with a sequence that has not been checked:
the CH5xx parts put their erase and program routines in ROM and reach them
through an interface that is not publicly documented, and getting that wrong is
how a part ends up bricked.

**None of this has been run against a real part yet.**  There is no WCH-LinkE
here, so what has been verified is the logic: see Tests below.

## Roadmap

| | |
|---|---|
| **M1** | Skeleton, host build, USB discovery, `info`, chip table, CLI — **done** |
| **M2** | Halt/resume, debug-register access, memory read-back — **done** |
| **M3** | Flash erase/write/verify, `reset`, `unbrick` — **done for CH32V00x; needs hardware** |
| **M3b** | Flash for the CH5xx families |
| **M4** | Single-wire debug terminal |

A GDB stub is **not** in scope.

## Tests

```sh
make test
```

Three suites, none of which needs hardware:

* `target_test` — the chip table: name matching, order codes, memory figures.
* `cli_test` — the binary's argument handling and exit codes, as a black box.
* `flash_test` — a whole flash sequence, driven through a **simulated
  programmer with a simulated CH32V003 on the other end**.  The simulation
  answers the same packets a LinkE does, interprets the programs the tool loads
  into the target's debug program buffer, and implements the flash controller
  well enough to notice a wrong page address, a program without an erase, a
  halfword in the wrong order or an unaligned write that clobbers its
  neighbours.

The third one is the useful one, and it is still not hardware.  What it cannot
check is the thing only a part can: whether the vendor protocol constants and
the debug-module behaviour are what this tool believes them to be.

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
