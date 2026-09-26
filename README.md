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

On Linux the USB device nodes are normally root-owned, so `wchlink` running as
an ordinary user cannot open the programmer until a udev rule grants access.
The tool itself needs no privileges.

```sh
sudo make install-udev-rules      # installs udev/99-wchlink.rules
# then replug the programmer
```

The installed rule combines two mechanisms:

* `TAG+="uaccess"`, which systemd-logind turns into an ACL for the active local
  session user;
* `MODE="0660", GROUP="plugdev"`, the Debian/Ubuntu group convention.

If the group path is used, make sure your user is in `plugdev` and start a new
login session after `usermod -aG plugdev $USER`; the current shell's
group list is fixed when it logs in.

Without any rule, libusb sees the device but cannot open it, and `wchlink`
says so rather than claiming no programmer is attached.

## Usage

```
wchlink info                 report the target chip; --chip optional
wchlink chips                list the parts this tool knows about
wchlink flash <file.bin>     write a binary image to flash
wchlink read  <file.bin>     read target memory into a file
wchlink reset                reset the target and let it run
wchlink unbrick              hold the target in reset and power-cycle it
wchlink programmer <sub>     inspect or control the WCH-LinkE itself
wchlink target <sub>         debug the target core
wchlink terminal             single-wire debug terminal         (milestone 4)
```

`programmer` subcommands:

```
wchlink programmer info      USB identity, mode, serial, product, firmware
wchlink programmer list      list every WCH programmer found on the bus
wchlink programmer rv        switch an ARM/SWD LinkE to RISC-V debug mode
wchlink programmer iap       eject an IAP-bootloader LinkE
```

`target` debug subcommands:

```
wchlink target halt          halt the core and leave it halted
wchlink target resume        release the core so it runs
wchlink target reset         reset the core and let it run
wchlink target pc            read the halted program counter
wchlink target regs          read pc and x0-x31
wchlink target read32 <addr>       read one 32-bit target word
wchlink target write32 <addr> <v>  write one 32-bit target word
```

`target halt`, `pc`, `regs`, `read32` and `write32` leave the target halted
when the command exits, so a sequence of reads sees one stable state.  Use
`target resume` when done.

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

# no --chip: identify the attached part automatically
wchlink info
wchlink flash firmware.bin --address 0x08000000

# no --address/--size: dump the whole code flash
wchlink read whole.bin

# inspect the programmer itself
wchlink programmer info
wchlink programmer list

# put the part back into reset and let it run
wchlink reset
```

## Chip detection

`wchlink info` is chip-focused: with `--chip` it answers from the built-in
table without touching USB, and without `--chip` it asks the WCH-LinkE for the
attached part's LinkE family/model pair and resolves that against its own chip
table.  `flash` and `read` also auto-detect when `--chip` is omitted.

`wchlink programmer info` is the programmer-focused command: it reports USB
identity, current mode, serial, product and firmware version without switching
the programmer first.  `programmer list` enumerates every WCH programmer
visible on the bus, including devices that are present but not openable, so a
missing udev rule is distinguishable from a missing cable.

An unknown model, a target that is not powered, or a target that is not
connected is an error; the tool never guesses at a part.  `read` is the only
command whose default changes when `--chip` is omitted: without
`--address`/`--size` it dumps the detected part's entire code flash.

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
