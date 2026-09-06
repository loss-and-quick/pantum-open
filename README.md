# pantum-open

A free driver for Pantum M6500-series laser multifunction devices: a SANE
backend for the scanner and a CUPS filter for the printer, both written from a
clean-room reconstruction of the vendor's USB protocols.

Pantum's own driver is proprietary. Its scanner half is also fragile — it links
against `libjpeg8` and an old SANE ABI, so it breaks every time the system moves
on. This project replaces both halves with code that has no such dependencies.

**Hardware it was developed against:** Pantum M6507, USB `232b:0e20`, which
identifies itself as `M6500 series`. Other models in the family use the same
protocols and are listed in the SANE config, but none of them has been tested —
see [docs/TODO.md](docs/TODO.md).

## What works

| | |
|---|---|
| Scanning | flatbed, at 75/150/300/600/1200 dpi, in colour, grey, lineart and halftone |
| Printing | all 28 PPD paper sizes plus custom, density, toner saving, 1200 dpi, negative, rotation, manual duplex |
| Frontends | anything that speaks SANE or prints through CUPS |

The print filter is byte-for-byte identical to the vendor's across 77
regression cases (`proto/check_against_vendor.sh`), and the scan pipeline
reproduces the vendor's tone curve and margins closely enough that 97-99 % of
pixels in a side-by-side scan come out identical.

## What does not work

- **No automatic document feeder.** The code is written but has never run on a
  unit that has one, and is marked unverified throughout.
- **No network transport.** USB only, even on models that also speak the same
  protocol over TCP.
- **No hardware duplex** — the hardware does not have it. `ManualDuplex` prints
  the fronts, pauses, and prints the backs in reverse order.
- **No driverless printing or eSCL scanning**, and this cannot be fixed in
  software: the device's printer interface is USB class `07/01/02`, and
  IPP-over-USB requires `07/01/04`.
- **No toner level in the SANE or CUPS layer**, though the printer will report
  one over PJL — see `proto/pantum_status.py`.

The full list, including questions that are already settled, is in
[docs/TODO.md](docs/TODO.md).

## Installing

```
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
sudo cmake --install build
```

Build requirements: libusb-1.0 and the SANE headers for the backend; libcups
and jbig-kit (`libjbig` plus `jbig.h`) for the print filter.
`-DPANTUM_BUILD_BACKEND=OFF` and `-DPANTUM_BUILD_FILTER=OFF` build one half
without the other.

The install puts `lib/sane/libsane-pantum.so.1`, `etc/sane.d/pantum.conf` and
`etc/sane.d/dll.d/pantum` where SANE looks for them — the `dll.d` entry enables
the backend without editing the system-wide `dll.conf` — and
`lib/cups/filter/rastertopantum` with
`share/cups/model/Pantum-M6500-open.ppd` where CUPS looks for those.

To try it without installing system-wide, point SANE at the staging tree:

```
SANE_CONFIG_DIR=<prefix>/etc/sane.d LD_LIBRARY_PATH=<prefix>/lib/sane \
  SANE_DEBUG_PANTUM=3 scanimage -L
```

### On NixOS

```nix
inputs.pantum-open.url = "github:loss-and-quick/pantum-open";
```

The flake's default package carries both halves, so the same derivation goes
into both options:

```nix
hardware.sane.extraBackends = [ pantum-open.packages.${system}.default ];
services.printing.drivers   = [ pantum-open.packages.${system}.default ];
```

`packages.sane-pantum` and `packages.rastertopantum` build one half each, for
when the other's closure is unwelcome. `nix build` with no arguments builds the
combined package.

## Using it

udev hands the device to group `lp`, so a process that talks to it needs that
group — hence the `sg lp` in the examples below.

### Scanning

The backend is called `pantum` and behaves like any other SANE device:

```
sg lp -c 'scanimage -L'
sg lp -c 'scanimage -d pantum --mode Color --resolution 300 -o page.pnm'
```

Options: `--mode Lineart|Halftone|Gray|Color`,
`--resolution 75|150|300|600|1200`, `--source` (`Flatbed`, plus feeder entries
on units that report one), `--threshold` (lineart only), `--preview` (forces
75 dpi), and the usual `-l/-t/-x/-y` geometry in millimetres up to
215.9 x 296.9 mm.

`Halftone` is one bit per pixel like `Lineart`, but instead of one threshold for
the whole page it screens the grey through a 128x128 blue-noise dither mask, so
a grey stays grey as a density of dots rather than collapsing to black or white.
Photographs and grey wedges survive it; a page of text is better off in
`Lineart`. It uses the same tone curve as `Gray`, so the two agree about what a
tone is, and `--threshold` reads as inactive there. The vendor driver has this
mode too and never offers it to a SANE frontend.

Non-blocking reads work: `sane_set_io_mode(TRUE)` makes `sane_read` return a
zero-length buffer instead of waiting for the next frame. `sane_get_select_fd`
is unsupported and will stay that way — the backend is synchronous and owns no
descriptor that becomes readable when pixels arrive.

### Printing

The filter is `rastertopantum` and its PPD is
`share/cups/model/Pantum-M6500-open.ppd`. The PPD's `*NickName` is deliberately
different from the vendor's, so a vendor queue and an open queue can coexist:

```
lpinfo -v                       # find the device URI, it starts with usb://Pantum/
lpadmin -p Pantum-open -v <uri> \
        -P <prefix>/share/cups/model/Pantum-M6500-open.ppd -E
lp -d Pantum-open file.pdf
```

Supported options, read out of the job exactly the way the vendor filter reads
them: `PageSize` (28 sizes plus custom), `MediaType`, `InputSlot`,
`Density=0|2|4`, `TonerMode`, `DPI1200`, `NegativePrint`, `ImageRotation`,
`ManualDuplex`.

### Asking the printer how it is doing

```
sg lp -c 'python3 proto/pantum_status.py status'
sg lp -c 'python3 proto/pantum_status.py watch'
```

`status` prints the model, command languages, state code and memory size.
`watch` subscribes to the printer's asynchronous PJL reports, which is the only
channel that carries a toner percentage. Neither is wired into CUPS.

## How it works

Two USB interfaces, and the split matters. **Interface 0** is the printer:
class `07/01/02`, bidirectional, claimed by the kernel's `usblp`, and CUPS
prints through it. **Interface 1** is vendor-specific `ff/ff/ff` with bulk
endpoints `0x02` out and `0x82` in; the scanner lives there and speaks a framed
protocol of its own.

The scanner protocol is 32-byte big-endian frames with the magic `ASP\x01`, the
same layout in both directions. The host locks the scanner, reads the settings
block, writes it back modified, and starts the job; the device then pushes
frames until the page and the job close. Image blocks are always 8-bit grey or
colour — neither bilevel mode exists in the hardware, both are packed down on
the host. Full specification: [notes/scan-protocol.md](notes/scan-protocol.md).

The print stream is ZjStream (Zenographics), big-endian, magic `JZJZ`, carrying
each page as a plain JBIG1 (ITU-T T.82) entity — so a filter can simply link
`libjbig`. Full byte map, item tables, paper codes and the halftone matrix:
[notes/print-protocol.md](notes/print-protocol.md).

## Repository layout

- `src/` — the shipped code: `pantum.c`/`pantum.h` are the SANE backend, which
  talks to libusb-1.0 directly and exports the `sane_pantum_*` entry points the
  SANE `dll` loader looks for; `rastertopantum.c` is the CUPS filter, reading
  raster through `libcupsraster` and writing ZjStream with JBIG from `libjbig`.
- `etc/sane.d/`, `ppd/` — the configuration each subsystem needs to find the
  above.
- `notes/` — the protocol specifications this was written from. Start with
  [scan-protocol.md](notes/scan-protocol.md) and
  [print-protocol.md](notes/print-protocol.md); the rest cover the printer's
  [status channels](notes/status-protocol.md), the
  [error and state catalogue](notes/error-catalogue.md), a
  [map of the device firmware](notes/firmware-map.md), and
  [how to reproduce the disassembly](notes/disasm-method.md).
- `proto/` — the tools the reconstruction was done with and the prototypes that
  proved it. `pantum_scan.py` is a working scanner in Python; `usbshim.c` is an
  `LD_PRELOAD` shim over libusb that captured the protocol without needing root
  for `usbmon`, and `parse_dump.py` turns its captures into one line per frame;
  `zjs_dump.py` decodes a ZjStream chunk by chunk; `rastertopantum.py` is the
  prototype print filter; `pantum_status.py` and `acl_read_mem.py` talk to the
  printer half; `nonblocking_test.c` drives the backend with
  `sane_set_io_mode(TRUE)`, a path no frontend exercises.
- `proto/check_against_vendor.sh` — the regression that keeps the print filter
  honest: a byte-for-byte diff against the vendor filter over 77 cases,
  including manual duplex and the rotation cases the prototype used to get
  wrong. It takes the filter under test as an argument, so it drives either the
  C filter or the Python one. Without the vendor filter installed it falls back
  to the sums in `tests/vendor-sha256.txt`. Its fixtures come from
  `proto/make_test_pdfs.py`, which builds fixed documents rather than letting a
  PDF library vary them between runs — the comparison is byte for byte.
  `proto/make_scan_target.py` generates the printable target used for scanner
  measurements, so that no real document has to go on the glass.
- `AGENTS.md` — conventions for working on this repository.

## Licence

GPL-2.0-or-later. The print filter links JBIG-KIT, which is GPLv2+, and the
same licence keeps the tree compatible with sane-backends.
