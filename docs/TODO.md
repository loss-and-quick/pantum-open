# Limitations and open work

What this driver does not do yet, what it will probably never do, and which
questions have already been answered so nobody spends a second evening on them.

The goal is full support for the M6507 first, then the rest of the M6500
family.

## Scanning

- [ ] **ADF: written, never run.** Source codes `0x200`/`0x400` in the window
      descriptor, tray status via command `0x0f` and the multi-page loop are all
      implemented from the disassembly, and every one of those code paths is
      marked UNVERIFIED where it sits. The only part hardware has confirmed is
      the negative answer: on a flatbed-only unit command `0x0f` comes back with
      status 1, the backend reads that as "no feeder", and the `--source` list
      stays `Flatbed` alone — exactly as the vendor's does. Nobody should call
      the feeder path working until it has run on a unit that has a feeder.
- [ ] **Other M6500 models.** `etc/sane.d/pantum.conf` carries the USB ids from
      the vendor configuration, but only `232b:0e20` has ever been tested.
- [ ] **TCP transport.** Networked models speak the same protocol on port 9200
      (`notes/scan-protocol.md` §1). Only USB is implemented.
- [ ] **`sane_get_select_fd`** stays `UNSUPPORTED`, deliberately. A frontend
      selects such a descriptor for *reading*, and the only descriptors this
      backend owns are libusb's, which on Linux are usbfs handles registered for
      `POLLOUT` and never become readable. Supporting it means either driving
      transfers through libusb's asynchronous API and pumping its event loop
      from the frontend's `select()`, or the usual reader thread feeding a pipe
      — both replace the synchronous core of the backend. Non-blocking
      `sane_read` works without either and is implemented.

## Printing

- [ ] **Collate and copies** are not implemented, and will not be: the vendor
      filter ignores `argv[4]` too, so cupsd does the multiplying
      (`notes/print-protocol.md` §4.7).

## Status reporting

The catalogue in `notes/error-catalogue.md` is incomplete: the firmware computes
its `CODE=` values rather than storing a table, so each one has to be observed
on a live device. §8 of that file lists what is missing and how to capture it
safely.

## Settled questions

Answered, with evidence. Reopen only with new hardware.

- **The 421x595 pt paper case means "A5 fed on its side", and is implemented.**
  The flag `GetPaperArrayIndex` raises for that size makes the filter announce
  `DMPAPER` 61 (`DMPAPER_A5_ROTATED`) instead of 11, swap the video size to
  4736x3264, and turn the grey page a quarter turn clockwise before halftoning
  — after the 180 degree turn `ImageRotation` asks for, so the halftone screen
  stays anchored to the sheet. No item is added; the chunk stays the ordinary
  14-item `START_PAGE`. Still no PPD in either driver emits 421x595 and
  `cupsfilter` snaps a `Custom.421x595` request onto A5 before the raster is
  written, so the branch is unreachable from CUPS; the six `A5Rot*` regression
  cases relabel an A5 raster header to reach it and match the vendor filter
  byte for byte. `ppd/Pantum-M6500-open.ppd` does not offer the size either:
  the filter now handles it if a raster ever carries it, but whether the
  engine really takes A5 short edge first has never been tried on hardware,
  and the vendor's own PPDs do not ask for it. Two vendor quirks are written up in
  `notes/print-protocol.md` §4.1: the flag is never cleared, so in the vendor
  a 421x595 page turns every later A5 page of the same job as well (not
  copied, ours decides per page), and the blank sheet manual duplex adds to an
  odd page count announces the turned size but carries an image of the untuned
  one (copied as found).

- **JPEG payloads (`data_type 0x0f`) are unreachable on this device.** Nothing
  on the host selects them: the vendor's scan-data handler branches on the type
  byte the *device* writes into the block sub-header, and no request path for it
  exists anywhere in the backend. The M6507 answers with a constant `0x0e` in
  the settings block and sends `0x06`/`0x0e` blocks in every combination tried —
  grey, colour and lineart at 75, 300, 600 and 1200 dpi. The open backend
  recognises the type and fails cleanly with `UNSUPPORTED` rather than
  mis-parsing it, which is as far as this can be taken without a device that
  produces one.

- **Colour channels do not need realigning.** Re-measured on saturated colour
  originals — printed stickers with hard yellow, magenta and blue edges, the
  kind of target that would show an offset if one existed — matching each
  channel against the other two over shifts of -4..+4 px. Over the same
  physical area at 300 and at 600 dpi the best whole-pixel shift is 0 on both
  axes for all three pairs, and the parabolic sub-pixel optimum stays well
  under half a pixel (worst 0.36 px at 300, 0.21 px at 600). A fixed physical
  offset would *double* in pixels between those resolutions; these do not grow
  at all, so what is left is measurement noise rather than a sensor offset. The
  vendor does not realign either. No correction implemented;
  `proto/measure_channel_align.py` reproduces the measurement.

- **Neither backend calibrates, and neither can.** The device hands the host
  an already-normalised image; whatever shading correction happens, happens
  before the data leaves it. Three independent checks.

  *The vendor backend contains no calibration code.* All 129 functions of its
  scan library were decompiled (`notes/disasm-method.md`); there is no dark or
  white reference, no accumulator, no per-column array anywhere in it. The
  whole host-side pixel path is `fill_white_margin` -> one 256-entry tone
  curve applied identically to every pixel -> un-padding -> optional threshold
  or dither. A shading correction needs one coefficient per sensor column; the
  backend allocates nothing of the sort.

  *The vendor sends no command this backend does not.* Both were captured over
  libusb (`proto/usbshim.c`) scanning the same sheet at 300 dpi Gray. The
  vendor's host-to-device sequence is `0x00 0x08 0x06 0x07 0x02 ... 0x01`;
  this backend's is the same plus one `0x0f` feeder probe at open. Nothing
  outside the documented set appears in either direction at any point, in
  particular no code above `0x0f`. Captures in
  `dumps/scan/calibration-*.usb.gz`.

  *The frames arrive flat.* `proto/pantum_scan.py` writes what the device
  sends, before any host processing. At 300 dpi Gray the white field reads
  exactly 255 over 94.8% of the page, and the per-column mean of the
  background varies by 2.37 counts across all 2560 columns — all of it in the
  first eight columns, the physical edge of the window, which both backends
  paint white anyway. In colour the three channels are flat to within 1.30
  (R), 3.84 (G) and 0.47 (B) counts across the width, again only at the
  extreme edges. The same holds at every resolution the device offers — the
  background spans 3.82 counts at 75 dpi, 2.37 at 300 and 0.11 at 600, always
  saturated over about 94% of the page. There is no illumination gradient left
  to correct, and no headroom to correct one with: white is clipped at 255 by
  the time the host sees it, so a per-column gain could only push already-white
  pixels further into the clip.

  *The outputs agree.* Vendor and open scans of the same sheet differ by 0.73
  counts on average — the two are separate physical passes — and the smoothed
  white-column profile spans 0.96 counts for the vendor against 0.95 for this
  backend. Nothing implemented; `dumps/scan/calibration-check.txt.gz` holds
  the numbers.

  What a clipped white field cannot rule out on its own is a per-column *gain*
  error, which would show in the mid-tones while white still saturated; the
  test for that is a uniformly mid-grey original, which was not to hand. It
  would not change what this backend does either way. The contract here is to
  match the vendor, and the vendor applies no correction and asks the device
  for none — so any per-column curve invented on this side would be a
  divergence from the reference, not a fix to it. If a grey target ever shows
  a real gradient, it is the *device* that is uncalibrated, and the fix would
  have to be a command the vendor never sends.

- **The vendor's halftone mode is not reachable through SANE.** Its mode 4
  dithers through a 128x128 blue-noise mask held in a proprietary blob, but the
  mode is only offered to Pantum's own application — the string list a SANE
  frontend receives is `Lineart|Gray|Color`. So there is nothing to compare an
  open halftone implementation against through that interface. This backend
  offers `Halftone` anyway, with a mask it generates itself; see
  `notes/scan-protocol.md` §5.
