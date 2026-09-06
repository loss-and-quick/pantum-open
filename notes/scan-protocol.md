# Pantum M6500 series — USB scan protocol ("ASP")

The framed protocol the scanner half of an M6500-series device speaks on its
vendor-specific USB interface. Reconstructed from the vendor's SANE backend
(`libsane-pantum6500.so.1.0.24`), then checked against a live M6507; the open
backend in `src/pantum.c` implements what is written here.

Addresses are offsets into that vendor library, kept so every claim can be
re-checked — `notes/disasm-method.md` says how to reproduce the disassembly.
Where the library and the hardware disagree, the hardware wins and the text
says so.

## 1. Transport layer

```c
typedef struct {                                    /* .data: available_transports @ 0x222a60 */
    const char *name;                               /* +0x00  "usb" / "tcp"                   */
    SANE_Status (*request)(Dev*, void *cmd, size_t cmdlen,
                           void *resp, size_t *resplen);        /* +0x08 */
    SANE_Status (*open)(Dev*);                      /* +0x10 */
    void        (*close)(Dev*);                     /* +0x18 */
    SANE_Bool   (*conn_state)(Dev*);                /* +0x20 */
    SANE_Status (*configure_device)(const char*, attach_cb);     /* +0x28 */
} Transport;                                        /* sizeof = 0x30 */
```
Populated via relocations (`readelf -rW`, 0x222a60..0x222ab8):
usb → usb_dev_request/open/close/conn_state/configure_device; tcp → tcp_* (port 9200, same protocol over TCP).

`dev->transport` sits at offset **0x4868** of the device structure (`calloc(1, 0x4870)` in list_one_device @0xdb00).

### usb_dev_request @ 0x13500
```c
SANE_Status usb_dev_request(Dev *dev, void *cmd, size_t cmdlen,
                            void *resp, size_t *resplen)
{
    com_pantum_sanei_usb_set_timeout(180000);        /* 0x2bf20 ms = 3 min */
    if (cmdlen && cmd) {
        size_t n = cmdlen;
        if (sanei_usb_write_bulk(dev->dn, cmd, &n))  return SANE_STATUS_IO_ERROR;
        if (n != cmdlen)                             return SANE_STATUS_IO_ERROR;
    }
    if (resp && resplen) {
        if (sanei_usb_read_bulk(dev->dn, resp, resplen)) return SANE_STATUS_IO_ERROR;
    }
    return SANE_STATUS_GOOD;
}
```
No framing/CRC of its own — just "write the buffer to bulk-out, read the buffer from bulk-in".
`dev->dn` (int, the sanei_usb descriptor) is at offset **0x28**.

### usb_dev_open @ 0x13670 / usb_dev_close @ 0x13740
open: up to 3 attempts at `sanei_usb_open(dev->sane.name, &dev->dn)`, with `sleep(1)` between attempts.
close: `set_altinterface(dn, 0)` then `usb_close`. **No vendor control requests at all.**
usb_configure_device @0x13790: timeout 1000 ms → attach_matching_devices → timeout 30000 ms.

---

## 2. Message format (32 bytes, big-endian)

`initMessage()` @ **0xb9e0**:
```c
void initMessage(uint32_t *buf, uint32_t msg) {
    if (!buf) return;
    memset(buf, 0, 32);
    buf[0] = htonl(0x41535001);   /* on the wire: 41 53 50 01 = "ASP\x01" */
    buf[1] = htonl(msg);
}
```

```c
struct pantum_msg {          /* exactly 32 bytes, ALL fields big-endian */
    uint32_t magic;    /* [0x00] 0x41535001 — 'A''S''P' 0x01                */
    uint32_t msg;      /* [0x04] message code (see table)                  */
    uint32_t arg0;     /* [0x08] argument/flag (ADF paper, page parity)    */
    uint32_t arg1;     /* [0x0c] —                                          */
    uint32_t status;   /* [0x10] result code (0 = OK) in responses          */
    uint32_t length;   /* [0x14] length of the data block immediately following */
    uint32_t rsvd6;    /* [0x18] 0                                          */
    uint32_t rsvd7;    /* [0x1c] 0                                          */
};
```
There is no checksum. Magic is the only signature.

### sendMessageAndReturn() @ 0xdc60
```c
uint32_t sendMessageAndReturn(Dev *dev, uint32_t msg) {
    uint32_t cmd[8], resp[8]; size_t n = 0x20;
    initMessage(cmd, msg);
    if (dev->transport->request(dev, cmd, 0x20, resp, &n)) return 1;
    if (ntohl(resp[1]) != msg) return 1;      /* "return message [%d] not equal to org message" */
    return ntohl(resp[4]);                    /* status */
}
```
`dev_unlock_scan()` @0xdd70 = `sendMessageAndReturn(dev, 1)`.

---

## 3. Message code table

Host → scanner (from `reader_process` @0x117c0 and `sendMessageAndReturn`):

| code | name in debug strings | meaning | payload |
|-----|--------------------|-------|---------|
| 0  | `dev_lock_scan`             | acquire the scanner | none |
| 1  | `dev_unlock_scan`           | release the scanner | none |
| 2  | `dev_start_scan`            | start job | none |
| 3  | (`sendMessage: message 3`)  | abort/cancel the current job | none, response not read |
| 6  | `dev_get_scan_job_settings` | read the current settings | response 32 B + **100 B** block |
| 7  | (`sendMessage: message 7`)  | write job settings | command **0x84 = 132 B** (32 B header + 100 B block) |
| 8  | `dev_set_default_setting`   | reset settings to default | none |
| 0x0f | `dev_check_adfstatus`     | ADF status | response 32 B, `arg0`=1 → paper present, `status`=0 → ADF present |

Scanner → host (parsed in `reader_process`, `switch (ntohl(hdr[1]))`):

| code | name | meaning |
|-----|-----|----------|
| 4    | `e_AbortScanJob` | job aborted, `status` (word[4]) = reason code |
| 5    | scan data        | `arg0` (word[2]) = page parity (0 odd/1 even, for duplex), `length` (word[5]) = how many bytes of data follow |
| 9    | scan job settings| followed by a 100 B settings block |
| 0x0a | (no debug string)| start of scan; the vendor's `switch` has no case for it and drops it |
| 0x0b | `Start Page`     | start of page |
| 0x0c | `e_EndJob`       | end of job |
| 0x0d | (no debug string)| end of scan; likewise unhandled by the vendor |
| 0x0e | `e_EndPage`      | end of page |

A full flatbed job on the M6507 emits them in the order `9, 0x0a, 0x0b,
5 x N, 0x0e, 0x0d, 0x0c`; `0x0a` and `0x0d` bracket the sensor pass inside the
page. The open backend names them `EVT_SCAN_BEGIN` / `EVT_SCAN_END` and, like
the vendor, ignores both.

### Device `status` codes → SANE_Status (mapping from reader_process)
| status | meaning | SANE_Status |
|--------|-------|-------------|
| 0 | OK | GOOD |
| 2 | scanner busy | DEVICE_BUSY(3) — driver retries lock 5 times with `sleep(2)` |
| 5 | no paper in ADF | NO_DOCS(7) |
| 6, 7 | jam | JAMMED(6) |
| 8 | cover open | COVER_OPEN(8) |
| other | error | INVAL(4) |

---

## 4. Job settings block (100 bytes, big-endian)

Read via msg 6 (`request(dev, NULL, 0, buf, &len=100)`) and sent back in msg 7
as the payload after a 32-byte header in which `length = 100`.
Offsets verified against `objdump` (addresses 0x11a5a…0x11c7f) and Ghidra local variable names.

```c
struct pantum_scan_settings {   /* 100 bytes, big-endian */
    uint32_t unk_00;      /* 0x00 */
    uint32_t unk_04;      /* 0x04 */
    uint32_t unk_08;      /* 0x08 */
    uint32_t resolution;  /* 0x0c  <-- WRITTEN: dpi (75/150/300/600/1200)      */
    uint32_t unk_10[9];   /* 0x10..0x30 — left untouched, taken as-is          */
    uint32_t zero_34;     /* 0x34  <-- WRITTEN: 0                              */
    uint32_t doc_source;  /* 0x38  <-- WRITTEN: 0x100 FB / 0x200 ADF / 0x400 ADF-duplex */
    uint32_t data_type;   /* 0x3c  read-only (data type, see §6)               */
    uint32_t win_top;     /* 0x40  <-- WRITTEN, units of 1/100 inch           */
    uint32_t win_left;    /* 0x44  <-- WRITTEN                                 */
    uint32_t win_bottom;  /* 0x48  <-- WRITTEN                                 */
    uint32_t win_right;   /* 0x4c  <-- WRITTEN                                 */
    uint32_t max_top;     /* 0x50  read-only ("Max scan window")               */
    uint32_t max_left;    /* 0x54 */
    uint32_t max_bottom;  /* 0x58 */
    uint32_t max_right;   /* 0x5c */
    uint32_t color_type;  /* 0x60  <-- WRITTEN: 1 = Color, 0 = Gray            */
};
```
Note: the remaining 9 dwords (0x10..0x33) are **not rewritten** — the driver takes
whatever the scanner returned for msg 6. An open driver must do the same (read-modify-write).

### Window clamps (addresses 0x11ba9…0x11c2d, units of 1/100")
* ADF (`doc_source ∈ {0x200,0x400}`): left ≤ 0x343 (835 = 8.35"), right ≤ 0x352 (850 = 8.5"),
  top ≤ 0x569 (1385), bottom ≤ 0x578 (1400 = 14" Legal)
* Flatbed (0x100): left/right same, top ≤ 0x482 (1154), bottom ≤ 0x491 (1169 = 11.69" = 297 mm)

`win_left = off_x`, `win_right = off_x + width`, `win_top = off_y`, `win_bottom = off_y + height`
(`dev->win_off_x`=+0x414, `win_off_y`=+0x418, `win_width`=+0x40c, `win_len`=+0x410).

### Resolution
`fix_window` @0xc080: the option value is rounded down to {75, 150, 300, 600, 1200}.
For ADF the maximum is **600** dpi (1200 → 600), for flatbed — **1200**.

---

## 5. Image data block format

`handle_scan_data(dev, len)` @0x11360 → `FUN_001108d0` @0x108d0.

After the msg=5 message, a **24-byte sub-header** is first read as a separate
bulk-in. The layout below is the one confirmed on an M6507; the vendor
disassembly alone suggests a different field order, and following it produces a
diagonally skewed image:

```c
struct pantum_data_hdr {   /* 24 bytes = 6 big-endian words */
    uint32_t data_type;    /* [0] 6 = grey, 0x0e = colour (see the table below) */
    uint32_t first_row;    /* [1] absolute number of the block's first row, from 0 */
    uint32_t rows;         /* [2] rows in this block                            */
    uint32_t channels;     /* [3] 1 for grey, 3 for colour                      */
    uint32_t pixels;       /* [4] *useful* pixels per row                       */
    uint32_t unused;       /* [5]                                               */
};
```

**Rows in the block are padded, and the sub-header does not say by how much.**
The real row stride is `(msg.length - 24) / rows`, and it can exceed
`pixels * channels`. A 708 px window at 300 dpi grey reports 720 px in `[4]`,
and a 16-row block occupies 11776 bytes — 736 bytes per row, 16 of them
trailing padding. Colour behaves the same way: 708 px is 2124 useful bytes at a
stride of 2208. Slice the block by the stride computed from the payload size
and use `[4]` only as the count of useful pixels; slicing by `pixels * channels`
skews the image diagonally.

The remainder (`msg.length - 24` bytes) is raw data, read in chunks:
* `data_type != 0x0f` → chunks of **≤ 0x2800 (10240)** bytes;
* `data_type == 0x0f` (JPEG) → chunks of **≤ 0x4000 (16384)** bytes, accumulated
  into the file `/tmp/com.pantum.m6500.<N>.jpeg`, then decompressed with libjpeg8
  (`jpeg_read_header/jpeg_start_decompress/jpeg_read_scanlines`) in batches of 128 rows.

### `data_type` codes (dispatcher `image_rescaling` @0xfc70)
| code | debug name | what the driver does |
|-----|-----------|--------------------|
| 0    | `e_RGBPacked`             | row = pixels*3 bytes, no conversion |
| 1    | `e_xRGBPacked`            | `convertXRGBData` @0xe6a0: 4 B/pixel `B G R x` → 3 B `R G B` |
| 0x0a | `e_BRGInterlaced`         | `convertInterlaceData` @0xe360 |
| 0x0c | `e_BRGInterlacedMirrored` | `convertInterlaceMirroredData` @0xe460 (same, plus row mirroring) |
| 0x0e | `e_RGBPackedData`         | byte-wise swap of the `B G R` triplet → `R G B` |
| 0x0f | (JPEG)                    | libjpeg decompression, RGB on output |

**"Interlace" = COLOR PLANES WITHIN A SINGLE ROW, not row interleaving.**
A row of length `n*3` is stored as three consecutive planes of `n` bytes each.
`convertInterlaceData`:
```c
n = row_bytes / 3;
for each row:
    for (i = 0; i < n; i++) {
        out[3*i+0] = row[n   + i];   /* plane 1 */
        out[3*i+1] = row[2*n + i];   /* plane 2 */
        out[3*i+2] = row[0   + i];   /* plane 0 */
    }
```
i.e. the planes are laid out in the order **B, R, G**, and the output is RGB.
The mirrored variant writes the result from the end of the buffer backwards (horizontal mirror).

**This plane order does not apply to the M6507.** That device always sets
`data_type` to `0x0e`, whose handler is a plain triplet reversal
(`out[0]=in[2], out[1]=in[1], out[2]=in[0]`) over **B, G, R** pixels — the
plane layout above belongs to type `0x0a` alone. An early revision of the open
backend applied the plane order to `0x0e` data and so exchanged red and green;
the mistake showed up only when the same sheet was scanned with both backends
and the per-channel means compared (ours R 252.822 / G 252.747 / B 253.399
against the vendor's R 252.747 / G 252.826 / B 253.404 — matching only after R
and G are swapped). Despite the name, `convertInterlaceData` is not the colour
path this hardware uses.

`convertGraytoBW` @0xe570 is **not used** on this path (left over from another backend):
it would pack 1 byte/pixel into 1 bit/pixel, LSB-first.

### Grey / lineart / halftone
Over USB the scanner only ever delivers **8-bit grayscale or color**. All 1-bit modes
are done on the host in `image_rescaling` @0xfc70:
* mode 1 (Lineart): gamma correction (γ=1.8, γ=1.0 for the CM1100/BM23xx models), then
  thresholding at `dev->threshold` (+0x428, from option 5), MSB-first bit packing;
* mode 4 (Halftone): dithering via `halftoneTable` (.data @0x21e680, 128×128) —
  through the `LUT` (.data @0x222680) when dpi < 300, directly when dpi ≥ 300;
  no gamma at all on this path. Set the bit (black) when the pixel is **below**
  `halftoneTable[(y % 128) * 128 + (x % 128)]`;
* mode 2 (Gray): 8 bits as-is + gamma;
* mode 3 (Color): 24 bits + gamma.

`halftoneTable` is a 16 KiB **blue-noise threshold mask**, not a clustered-dot
screen: 16384 cells holding every value 1..255 about 64 times each, mean
128.006, no period that divides 128 in either axis, and radially rising energy
in its Fourier transform (mean |F| 970 in the innermost eighth against 7006 in
the outermost). `LUT` is a plain 256-entry contrast curve applied ahead of it
at low resolutions — zero below input 39, saturated from 230, and within ±3 of
`255 * ((i - 37) / 192) ^ 1.73`.

Neither is reproduced in the open backend, and nothing in the vendor driver
reaches them from SANE: for a SANE frontend the vendor's own mode list is
`Lineart|Gray|Color`, and the halftone entry only appears in the list Pantum's
application asks for (`FUN_0010bad0(1)`, .data @0x222780, against @0x2227c0 for
everyone else).

The open backend offers `Halftone` all the same. It carries neither of these
two tables: it generates a blue-noise mask of its own with void-and-cluster
(the statistics above are what that result is checked against) and screens the
output of the ordinary tone curve through it, rather than replacing the curve
with `LUT` below 300 dpi the way mode 4 does. Generation takes about 0.22 s at
the first halftone scan of a process, from a fixed seed and deterministic
scans, so every run produces the same 16384 bytes and nothing has to be shipped.

Keeping the ordinary curve is a deliberate divergence: halftone here is `Gray`
screened down to one bit and must agree with `Gray` about tone. On a printed
eleven-step wedge it does — the mean of every step is within 0.008 of the grey
scan at 150 and 300 dpi and all eleven stay distinct, where lineart clips the
darkest steps to solid black and the brightest to paper white.

### The tone curve (`gamma_correction` @0xbbe0, `build_gamma_lut` @0xbb40)

Every scanned byte goes through one 256-entry lookup table before it leaves the
backend — grey, colour and the 8-bit input of the lineart threshold alike; only
the halftone path skips it. The table is built once per process, in two steps.

**Step 1 — the base curve**, chosen by the bit depth of the data (8 or 24) and
evaluated in `double`, then truncated towards zero and clamped to 0..255:

```
depth 8 (grey, lineart):
    i <  80 : i*1.8279807 + 0.56917985 - i²*0.015226293 + i³*0.00013340606
    i >= 80 : i*1.4804465 + 2.4442346 - i²*0.0024925889 + i³*5.6432708e-06

depth 24 (colour):
    i <  50 : i*326.56441 / (i + 223.04027)
    i < 160 : i*1.3679907 + 1.3433091 - i²*0.0028273626 + i³*9.1069919e-06
    i >=160 : 382.17813 - i*0.2533356 - 3965432.2/i²
```

**Step 2 — the gamma**, applied in place over the base table:

```
table[i] = round(255 * (base[i] / 255) ^ (1 / γ)),  clamped to 255
```

γ is **1.8**, except on the CM1100 and BM23xx models, where the backend logs
"gamma changed to 1.0" and leaves the base curve alone. Both steps saturate
well before 255: for depth 8 the base curve already reaches 255 at input 250,
which is why a blank sheet comes out at 255 rather than at the ~251 the sensor
actually reports.

Two details matter if the output is to be reproduced byte for byte: the base
curve is truncated (`(int)`) and the gamma step is rounded (`floor(x+0.5)`);
and the base table is built for whichever depth is used *first* in the process,
so a vendor process that scans grey and then colour keeps the grey curve.

### `fill_white_margin` @0xf5c0

Fills margins with 0xFF in the padded 8-bit buffer, before the tone curve and
before any thresholding. The row count is a global set at `Start Page`:

| source | top | left | right | bottom | condition |
|---|---|---|---|---|---|
| flatbed | `(int)(dpi*2/25.4)` | `(int)(dpi*1.5/25.4)` | — | — | top only if `win_off_y == 0`, left only if `win_off_x == 0` |
| ADF | `(int)(dpi*2.5/25.4)` | `(int)(dpi*1.5/25.4)` | same as left | same as left | only when a feeder answered `0x0f` |

Left/right are in **pixels**, so the byte count is multiplied by 3 in colour.
At 75 dpi flatbed that is 5 rows and 4 pixels, and both were confirmed on
hardware: columns 0..3 and rows 0..4 of a vendor scan are exactly 255.

Finished rows are written to the FIFO file `/tmp/com.pantum.m6500.<N>` (`fifo_write` @0xfbc0),
from which `sane_read` (`fifo_read` @0xfa60) reads them. One FIFO per page,
the page queue is `g_file_queue` (`creat_queue`/`enqueue`/`popqueue`/`dequeue`).

### How closely the reproduction matches

Curve, margins and colour byte order together decide whether an open backend
produces the same picture as the vendor's. Scanning one sheet with each backend,
same options, one pass each:

| mode | dpi | open | vendor | delta | histogram L1 |
|---|---|---|---|---|---|
| grey | 75 | 253.7499 | 253.7478 | +0.0021 | 0.14 % |
| grey | 150 | 253.3164 | 253.3171 | -0.0007 | 0.07 % |
| grey | 300 | 252.9138 | 252.9132 | +0.0006 | 0.03 % |
| grey | 600 | 252.9210 | 252.9200 | +0.0010 | 0.02 % |
| colour | 75 | 252.9940 | 252.9922 | +0.0018 | 0.10 % |
| colour | 150 | 252.5229 | 252.5223 | +0.0005 | 0.07 % |
| colour | 300 | 252.0764 | 252.0755 | +0.0010 | 0.03 % |
| colour | 600 | 252.4312 | 252.4312 | +0.0000 | 0.01 % |

97-99 % of pixels come out bit-identical and the rest sit on the edges of the
text — this is two physical passes over the same sheet, so a residual of that
shape is the floor, not a defect.

---

## 6. Modes and sources (`scan_mode_to_code`)

Constant tables in .rodata:
* `0x1a050`: `{1, 2, 3, 4}` — mode codes (index = position of the string in the list)
* `0x1a060`: `{0x100, 0x200, 0x400, 0x80}` — source codes

The mode and source a frontend selects are matched **by string**, against one of
several lists the vendor backend picks between depending on which application is
asking (`FUN_0010bad0` / `FUN_0010ba40`). Every list carries the same entries in
the same order — a plain English name, a longer English variant, and a localized
one — so only the position matters:

| position | mode name a SANE frontend sees | `dev->mode` code (+0x420) |
|---|---|---|
| 0 | `Lineart` | **1** |
| 1 | `Gray` | **2** |
| 2 | `Color` | **3** |
| 3 | `Halftone` | **4** |
| (no match) | — | 1 |

| position | source name | `dev->doc_source` code (+0x424) |
|---|---|---|
| 0 | `Flatbed` | **0x100** |
| 1 | `ADF Simplex` | **0x200** |
| 2 | `ADF Duplex` | **0x400** |
| (no match) | — | 0x100 |

`Halftone` is present in the code but is only offered to Pantum's own
application; a SANE frontend is handed `Lineart|Gray|Color`.

What goes into the settings block: `doc_source` as-is (0x38), and from the mode — only
`color_type = (mode == 3) ? 1 : 0` (0x60). Resolution (0x0c) — as the dpi number.

`set_parameters` @0xcb20 (`FUN_0010cb20`) computes the SANE parameters:
```c
pixels_per_line = win_width * dpi / 100;          /* dev+0x33c */
lines           = win_len   * dpi / 100;          /* dev+0x340 */
mode 1|4: format=GRAY, depth=1, bytes_per_line=(ppl+7)>>3;
mode 2  : format=GRAY, depth=8, bytes_per_line=ppl;
mode 3  : format=RGB,  depth=8, bytes_per_line=ppl*3;
```

---

## 7. USB: interface and endpoints

All of this is in the `sanei_usb` fork (`com_pantum_sanei_usb_*`).

* **VID/PID** are taken from the vendor's own `etc/sane.d/pantum6500.conf`
  (`$VENDOR_DRIVER/etc/sane.d/pantum6500.conf` in an unpacked driver package;
  the open backend's equivalent is `etc/sane.d/pantum.conf`). For M6507 there is no
  0x0e20 as a separate line — there is `usb 0x232b 0x0e20 M6500` (what the device
  reports itself as, "M6500 series"), and also `usb 0x232b 0x086d M6507N`.
* **Interface selection** (`com_pantum_libusb_scan_devices` @0x107fd0):
  – if `bDeviceClass == 0xff` — the whole device is taken;
  – otherwise the interfaces are enumerated and the **first** one with
    `bInterfaceClass ∈ {0x00, 0x06, 0x0e, 0x10, 0xff}` is taken.
  For M6507, interface 0 = class 0x07 (printer) → rejected,
  interface 1 = 0xff → **interface 1 is selected**.
* **Opening** (`com_pantum_sanei_usb_open` @0x9460):
  `libusb_open` → `libusb_get_configuration` → `libusb_set_configuration(cfg[0].bConfigurationValue)`
  (a `LIBUSB_ERROR_BUSY` error is ignored — "Maybe the kernel scanner driver or usblp claims the interface?")
  → `libusb_claim_interface(interface_nr)`.
* **Alt setting**: **not set** at open time (stays 0); `usb_dev_close` explicitly
  does `libusb_set_interface_alt_setting(dn, 0)`.
* **Endpoints are NOT hardcoded.** All endpoint descriptors of the selected
  interface are enumerated; the first bulk one with `bEndpointAddress & 0x80 == 0` → bulk-out,
  the first with 0x80 → bulk-in (full addresses, including the direction bit).
* Transfer: a single `libusb_bulk_transfer(handle, ep, buf, len, &transferred, timeout)`,
  up to 6 retries on error (`usleep(10000)` between them), and on the final error —
  `libusb_clear_halt` on the corresponding ep. Timeout is set via `set_timeout`:
  1000 ms at configure time, 30000 ms by default, **180000 ms during a request**.

---

## 8. Pseudocode for sane_start / reader_process

`sane_pantum6500_start` @0xf010 does not itself send anything over USB:
```c
SANE_Status sane_start(Dev *dev) {
    if (dev->transport->open(dev)) return SANE_STATUS_IO_ERROR;   /* [vt+0x10] */
    if ((dev->doc_source & 0xff00) == 0x100 && dev->scanning)     /* flatbed busy */
        { up to 3 times sleep(2); if still scanning → SANE_STATUS_DEVICE_BUSY; }
    if (ADF && dev->scanning) { ... multi-page mode ... }
    if (!fix_window(&dev->win_width, &dev->win_len)) return SANE_STATUS_INVAL;
    if (!bHave_enough_memory(dev))                   return SANE_STATUS_NO_MEM;
    dev->reading_pages = 1; dev->read_pages = 0; ...
    pthread_create(&dev->thread, NULL, reader_process, dev);
    wait until reader_process starts delivering data;
}
```

`reader_process` @0x117c0 — all of the protocol logic:
```c
void reader_process(Dev *dev)
{
    if (win_width <= 14 || win_len <= 14) { dev->state = SANE_STATUS_INVAL; return; }

    /* 1. lock, with 5 attempts when status==2 (busy) */
    for (try = 5; try; try--) {
        st = sendMessageAndReturn(dev, 0);            /* dev_lock_scan */
        if (st != 2) break;
        sleep(2);
    }
    if (st) { map_status(st); return; }

    /* 2. ADF status (only if doc_source ∈ {0x200,0x400}) */
    if (!dev->scanning) {
        uint32_t c[8], r[8]; size_t n = 32;
        initMessage(c, 0x0f);
        if (!request(dev, c, 32, r, &n) && ntohl(r[1]) == 0x0f) {
            if (ntohl(r[4]) == 0) {                   /* ADF present */
                g_has_adf = 1;
                if (ntohl(r[2]) != 1) { dev->state = SANE_STATUS_NO_DOCS; unlock(); return; }
            } else g_has_adf = 0;                     /* "No feeder or other errors." */
        }
    }

    /* 3. default + read settings */
    if (sendMessageAndReturn(dev, 8)) goto fail;      /* dev_set_default_setting */
    if (sendMessageAndReturn(dev, 6)) goto fail;      /* dev_get_scan_job_settings */
    size_t n = 100;
    request(dev, NULL, 0, settings, &n);              /* 100 B settings block */

    /* 4. edit the block and write it back */
    settings.doc_source = htonl(dev->doc_source);
    settings.resolution = htonl(dev->resolution);
    settings.color_type = htonl(dev->mode == 3);
    settings.zero_34    = htonl(0);
    settings.win_left   = htonl(clamp(off_x,            0, ADF?0x343:0x343));
    settings.win_right  = htonl(clamp(off_x + width,    0, ADF?0x352:0x352));
    settings.win_top    = htonl(clamp(off_y,            0, ADF?0x569:0x482));
    settings.win_bottom = htonl(clamp(off_y + height,   0, ADF?0x578:0x491));
    g_duplex = ((dev->doc_source & 0xff00) == 0x400);

    uint8_t cmd[132];
    initMessage(cmd, 7);
    ((uint32_t*)cmd)[5] = htonl(100);                 /* length */
    memcpy(cmd + 32, &settings, 100);
    size_t rn = 32; uint32_t resp[8];
    if (request(dev, cmd, 132, resp, &rn) || ntohl(resp[4]) != 0) goto fail;

    set_parameters(dev);
    if (sendMessageAndReturn(dev, 2)) goto fail;      /* dev_start_scan */

    /* 5. event loop */
    do {
        if (dev->cancel && !dev->cancel_sent) {
            uint32_t c[8]; initMessage(c, 3);
            request(dev, c, 32, NULL, NULL);          /* abort, response not awaited */
            dev->cancel_sent = 1;
        }
        size_t n = 32; uint32_t hdr[8];
        if (request(dev, NULL, 0, hdr, &n)) { dev->state = SANE_STATUS_INVAL; break; }
        switch (ntohl(hdr[1])) {
        case 4:   /* e_AbortScanJob */  map_status(ntohl(hdr[4])); unlock(); goto done;
        case 5:   /* data */           g_page_parity = ntohl(hdr[2]);
                                       handle_scan_data(dev, ntohl(hdr[5])); break;
        case 9:   /* settings echo */  n = 100; request(dev, NULL, 0, settings, &n); break;
        case 0x0b:/* Start Page */     new FIFO, enqueue(g_file_queue, fifo); break;
        case 0x0c:/* e_EndJob */       unlock(); goto done;
        case 0x0e:/* e_EndPage */      pad the page with white up to lines*bytes_per_line;
                                       for duplex — unpack the deferred even page;
                                       fifo->eof = 1; break;
        }
    } while (dev->state == SANE_STATUS_GOOD);
done:
    dev->scanning = 0;
}
```

## 9. Offsets in the device structure (sizeof = 0x4870)
```
+0x000 next                       +0x008 sane.name        +0x018 sane.model
+0x028 dn (sanei_usb handle)
+0x030 opt[12]  (SANE_Option_Descriptor, 0x38 bytes each)
+0x2d0 val[12]  (Option_Value, 8 bytes each)
        opt3=resolution(0x2e8) opt4=mode string(0x2f0) opt5=threshold(0x2f8)
        opt6=source string(0x300) opt7=paper-size(0x308)
        opt8..11 = tl-x/tl-y/br-x/br-y, SANE_Fixed (1/65536 mm) @0x310/0x318/0x320/0x328
+0x330 params.format   +0x334 params.last_frame  +0x338 params.bytes_per_line
+0x33c params.pixels_per_line  +0x340 params.lines  +0x344 params.depth
+0x348 reading  +0x34c scanning  +0x350 cancel_started  +0x354 cancel_sent
+0x358 cancel_ended  +0x35c state (SANE_Status)  +0x360 next_page_flag
+0x370 max_len_mm     +0x374 resolution word-list
+0x40c win_width      +0x410 win_len   (1/100 inch)
+0x414 win_off_x      +0x418 win_off_y (1/100 inch)
+0x41c resolution dpi +0x420 mode code (1..4)  +0x424 doc_source (0x100/0x200/0x400)
+0x428 threshold      +0x42c paper-size index
+0x430 total bytes need  +0x434 total read  +0x438 total from scanner
+0x448 pthread_t
+0x450 fifo[128] (0x88 bytes each)
+0x4850 current read fifo  +0x4858 current write fifo
+0x4860 read_pages  +0x4864 scanned_pages  +0x4868 transport*
```

---

## 10. Points confirmed on hardware

Everything above is what the vendor library does. These are the points an
implementation gets wrong easily, each checked against a live M6507. The two
biggest ones — the data-block sub-header layout and the padded row stride — are
folded into §5 rather than listed here.

1. **Window offsets work.** The device honestly applies all four settings-block
   words `[16]`=top, `[17]`=left, `[18]`=bottom, `[19]`=right (hundredths of an
   inch), not just the bottom/right pair. Frame size is
   `pixels = (right-left)*dpi/100`, `lines = (bottom-top)*dpi/100`, rounded down;
   that matches what the vendor backend produces.

2. **1200 dpi is accepted on the flatbed** (checked on a 30 x 15 mm window).

3. **Lineart: bit 1 = black, MSB first** — this matches PBM, so no inversion is
   needed on the way out.

4. **The settings block's `data_type` (0x3c) is read-only and constant.** The
   M6507 answers `0x0e` in every mode at every resolution. The JPEG path is
   selected purely by the type byte in the per-block sub-header, and this device
   never sets it to `0x0f` — see `docs/TODO.md` for why that path is considered
   unreachable here.

5. **Closing the session is mandatory.** A host that abandons a job without
   sending `0x01` leaves the panel showing a communication error (status 25).
   After `0x03` (abort) the device stops sending frames almost immediately —
   `0x0c` may never arrive — but `0x01` is still accepted and answered normally.
   After an abrupt disconnect a leftover frame (usually one 32-byte header) sits
   on bulk-IN and must be drained when the device is next opened, or the new
   session starts out of sync.
