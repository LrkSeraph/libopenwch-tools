# libopenwch-tools

Host-side tools for WCH RISC-V microcontrollers. The first is **wchlink**, a
command-line flasher and debug tool for the **WCH-LinkE**.

This repository is separate from
[libopenwch](https://github.com/LrkSeraph/libopenwch): the library is RISC-V
target code, this is a host program built with libusb. The template repository
mounts it as a submodule and delegates `make flash` to it.

> **Status: M1–M4 implemented for CH32V00x; hardware run pending.**
> `info`, halt/resume, debug-register access, memory read-back, CH32V00x
> erase/write/verify, `reset`, `unbrick`, and the SDI/DMDATA terminal are
> implemented. The flash sequence passes against a simulated programmer and
> simulated CH32V003; it has not been exercised on real parts.

## Building

```sh
make
```

Binary: `build/wchlink`. `make install` installs it and the udev rule.
libusb is found through `pkg-config`; if only the runtime library is present:

```sh
make LIBUSB_CFLAGS=-I/path/include LIBUSB_LIBS=-l:libusb-1.0.so.0
```

## Permissions

```sh
sudo make install-udev-rules
# replug the programmer
```

The rule uses `TAG+="uaccess"` and `MODE="0660", GROUP="plugdev"`. If using the
group path, add yourself to `plugdev` and start a new session. Without a rule,
`wchlink` reports a present-but-unusable programmer, not "not found".

## Usage

```
wchlink info                 report the target chip (--chip optional)
wchlink chips                list known parts
wchlink flash <file.bin>     erase, write, verify
wchlink read  <file.bin>     read memory; default dumps whole flash
wchlink reset                reset and run
wchlink unbrick              hold reset and power-cycle
wchlink programmer <sub>     info | list | rv | iap
wchlink target <sub>         halt | resume | reset | pc | regs | read32 | write32
wchlink terminal             SDI/DMDATA debug output; Ctrl-C to stop
```

`flash` and `read` auto-detect the part when `--chip` is omitted.  The live
core commands `target pc`, `regs`, `read32`, and `write32` require `--chip`:
auto-detection releases the target and restarts the firmware, which destroys
the state being inspected.  `target halt`, `resume`, and `reset` do not need
it.  These debug commands leave the core halted; use `target resume`.
Options: `--serial`, `--chip`, `--address`, `--size`, `--verify`,
`--no-verify`, `--verbose`, `--quiet`, `--version`, `--help`.
Exit codes: `0` success, `1` USB/target error, `2` usage, `3` no programmer.

```sh
wchlink flash firmware.bin --chip ch32v003
wchlink flash config.bin --chip ch32v003 --address 0x08003f00
wchlink read dump.bin --chip ch32v003 --address 0x08000000 --size 1024
wchlink read whole.bin
wchlink programmer info
wchlink terminal
```

## Programmer and chip detection

A WCH-LinkE can appear as RISC-V debug (`1a86:8010`), ARM/SWD (`1a86:8012`),
or USB ISP/IAP bootloader (`4348:55e0`, `1a86:55e0`). `wchlink` switches an
accessible ARM/SWD or IAP device into RISC-V debug mode automatically.

`info` is chip-focused; `programmer info` reports the programmer itself.
An unknown or unpowered target is an error; the tool never guesses.

## Flashing

`flash` erases, programs, and verifies page by page. Partly covered pages are
read back and merged first. The target is halted during the operation and reset
afterwards.

Flashing is implemented for **CH32V00x only** (CH32V003/002/004/005/006/007).
CH5xx is refused because its ROM erase/program interface is not publicly
documented.

## Tests

```sh
make test
```

Three hardware-free suites:

* `target_test` — chip table and matching;
* `cli_test` — command-line handling and exit codes;
* `flash_test` — full flash sequence through a simulated WCH-LinkE and
  simulated CH32V003.

The simulation cannot replace a real part: it cannot validate vendor protocol
constants or debug-module behavior.

## Roadmap

| | |
|---|---|
| **M1** | USB discovery, `info`, chip table, CLI — done |
| **M2** | halt/resume, debug registers, memory read-back — done |
| **M3** | CH32V00x erase/write/verify, `reset`, `unbrick` — done; hardware pending |
| **M3b** | CH5xx flashing |
| **M4** | SDI/DMDATA terminal — done; hardware pending |

GDB stub is not in scope.

## Licence

LGPL-3.0-or-later; see `LICENSE` and `NOTICE`. Implementation is clean-room:
minichlink, wlink, and riscv-openocd-wch were consulted for protocol facts only.
The protocol is undocumented and community-derived; not endorsed by WCH.
