# Pantum M6500 series — print stream format (ZjStream + JBIG)

The complete wire format an M6500-series printer accepts for a print job, and
how each CUPS option maps onto it. Reconstructed from the vendor's CUPS filter
`$VENDOR_DRIVER/lib/cups/filter/rastertoPantum` (not stripped; `ptm6500Filter` is
the same inode — all 17 "model" filters are hardlinks of one executable) and
verified against a live M6507. Addresses are offsets into that binary;
`notes/disasm-method.md` says how to reproduce the disassembly.

## Summary

The stream is **ZjStream (Zenographics) in its big-endian variant**, signature
`JZJZ`, carrying **plain JBIG1 (ITU-T T.82)** that is fully compatible with
Markus Kuhn's `jbig-kit`. **Neither PJL nor ACL appear in this branch** — the ACL
commands in `notes/status-protocol.md` belong to other platforms served by the
same binary (PLATFORM_C / PLATFORM_Z), while M6500 is PLATFORM_M.

The format is reproduced byte for byte. `proto/check_against_vendor.sh` compares
a candidate filter against the vendor's output over 77 cases — 5 kinds of page
content (blank, square, solid black, three pages, noise larger than 64 KB), 19
option combinations (density, toner saving, negative, rotation, 1200 dpi, media
types, trays), all 28 paper sizes in the PPD and the sideways A5 sheet of §4.1.
`src/rastertopantum.c` passes all of them. The `proto/rastertopantum.py`
prototype passes every case it implements; manual duplex is the one thing it
never grew, so the `md-*` cases and `A5Rot-duplex` are its known gap. Streams
from both have also been printed on hardware and accepted without error.

---

## 1. Choosing the branch: why M6500 is "platform 3"

`main()` reads `gPPDFile = ppd->nickname` (`ppd_file_t + 0x68`), but the branch is
selected **not by NickName**, but by the `*cupsIPPReason PlatForm` string from the PPD:

```c
ppdLocalizeIPPReason(ppd, "PlatForm", 0, buf, 1024);
if      (!strcasecmp(buf, "PLATFORM_M")) { platform = 3; paper = gPaperSizeArray_PT2500_PTM6500; }
else if (!strcasecmp(buf, "PLATFORM_C")) { platform = 1; paper = gPaperSizeArray_SN2016; }
else if (!strcasecmp(buf, "PLATFORM_Z")) { platform = 2; paper = gPaperSizeArray_PT3100_PTM5300; }
```

`Pantum M6500 Series.ppd` contains `*cupsIPPReason PlatForm/PlatForm: "text:PLATFORM_M"`
→ **platform == 3**. In the filter log this shows up as `DEBUG: ##platform = 3, ##autoduplex = 0`.

All the logic in `Prnt_StartDoc` / `Prnt_StartPage` / `Prnt_SendPage` / `Prnt_EndPage` /
`Prnt_EndDoc` branches on `platform`:

| platform | PPD PlatForm | what is emitted |
|---|---|---|
| 1 | `PLATFORM_C` | PJL preamble + ACL packets `0x0b …` + JBIG (options `0x08`, MX=8) |
| 2 | `PLATFORM_Z` | PJL preamble + ACL packets, JBIG **in strips** with ACL header `0x1a` (48 bytes) |
| **3** | **`PLATFORM_M`** | **plain ZjStream, no PJL and no ACL** |

**NickName (`gPPDFile`) is used only for exceptions**, none of which apply to M6500:

* the `Pantum BP2400/BP2408/BP2460/BM2400/BM2408/BM2460` family (+`NW`/`A`/`ANW`) — for these
  `Insert00()` does not write the `00 00` separator, and `JBIGCompress` doubles the width at 1200 dpi;
* `Pantum M6700DW Plus Series` — a different halftoning table (`NEW_HT_TABLE_600_Z_M`);
* `Pantum M5000-M6000 Series` — `GetTonerParamCmdForM5300`;
* `Pantum S2000 Series` and others — their own paper tables.

None of these comparisons match `"Pantum M6500 Series"`, which means
**the M6500 branch is the generic platform 3 branch with no special cases at all**.

---

## 2. Complete byte map of a job

Below is the actual filter output for a blank A4 page (500 bytes), annotated.

```
offset  bytes                                   meaning
------  --------------------------------------  -------------------------------------------
000000  1b 25 2d 31 32 33 34 35 58              UEL = ESC "%-12345X"
000009  4a 5a 4a 5a                             stream magic "JZJZ"

00000d  00 00 00 58  00 00 00 11  00 00 00 00   ZJ_HEADER: size=88 type=0x11 items=0
        00 00  5a 5a                              itembytes=0 signature=0x5a5a
00001d  <72 bytes of constants>                 GetTonerParamCmdForPlatformM blob (see §5)

000065  00 00 00 34  00 00 00 00  00 00 00 03   ZJT_START_DOC, 3 items, 36 bytes of items
        00 24  5a 5a
000075  00 00 00 0c  00 00 01 00  00 00 00 00   ZJI_PAGECOUNT       = 0
000081  00 00 00 0c  00 01 01 00  00 00 00 00   ZJI_DMCOLLATE       = 0
00008d  00 00 00 0c  00 02 01 00  00 00 00 01   ZJI_DMDUPLEX        = 1  (DMDUP_SIMPLEX)

000099  00 00 00 b8  00 00 00 02  00 00 00 0e   ZJT_START_PAGE, 14 items, 168 bytes
        00 a8  5a 5a
0000a9  … 00 03 …  00 00 00 09                  ZJI_DMPAPER         = 9    (A4)
0000b5  … 00 04 …  00 00 00 01                  ZJI_DMCOPIES        = 1
0000c1  … 00 05 …  00 00 00 00                  ZJI_DMDEFAULTSOURCE = 0
0000cd  … 00 06 …  00 00 00 01                  ZJI_DMMEDIATYPE     = 1    (Plain)
0000d9  … 00 07 …  00 00 00 01                  ZJI_NBIE            = 1
0000e5  … 00 08 …  00 00 02 58                  ZJI_RESOLUTION_X    = 600
0000f1  … 00 09 …  00 00 02 58                  ZJI_RESOLUTION_Y    = 600
0000fd  … 00 16 …  00 00 00 00                  ZJI_RET             = 0
000109  … 00 0c …  00 00 12 80                  ZJI_RASTER_X        = 4736
000115  … 00 0d …  00 00 1a 80                  ZJI_RASTER_Y        = 6784
000121  … 00 10 …  00 00 00 01                  ZJI_VIDEO_BPP       = 1
00012d  … 00 11 …  00 00 12 80                  ZJI_VIDEO_X         = 4736
000139  … 00 12 …  00 00 1a 80                  ZJI_VIDEO_Y         = 6784
000145  … ff 00 …  00 00 00 02                  Pantum: DENSITY     = 2

000151  00 00 00 24  00 00 00 04  00 00 00 00   ZJT_JBIG_BIH, 20 bytes of "raw" payload
        00 00  5a 5a
000161  00 00 01 00  00 00 12 80  00 00 1a 80   BIH: DL=0 D=0 P=1 -, XD=4736, YD=6784,
        00 00 01 00  00 00 03 5c                     L0=256, MX=0 MY=0, order=3, options=0x5c

000175  00 00 00 46  00 00 00 05  00 00 00 00   ZJT_JBIG_BID, 54 bytes of data
        00 00  5a 5a
000185  ff 02 ×27                               27 strips of 256 lines, each "blank"

0001bb  00 00 00 10  00 00 00 06  00 00 00 00 00 00 5a 5a   ZJT_END_JBIG
0001cb  00 00 00 10  00 00 00 03  00 00 00 00 00 00 5a 5a   ZJT_END_PAGE
0001db  00 00 00 10  00 00 00 01  00 00 00 00 00 00 5a 5a   ZJT_END_DOC
0001eb  1b 25 2d 31 32 33 34 35 58                          UEL
```

Multi-page job: the `START_PAGE … END_PAGE` blocks repeat between `START_DOC`
and `END_DOC`. No "form feed" and no ACL commands anywhere.

### Job pseudocode

```
UEL
"JZJZ"
chunk(0x11, raw = PANTUM_PARAM_BLOB[72])
chunk(START_DOC, items = {PAGECOUNT=0, DMCOLLATE=0, DMDUPLEX=1})
for page in pages:
    chunk(START_PAGE, items = {...14 or 17 items...})
    chunk(JBIG_BIH,  raw = bih[20])
    for off in range(0, len(bid), 65536):          # payload max 0x10000
        chunk(JBIG_BID, raw = bid[off:off+65536])
    chunk(END_JBIG)
    chunk(END_PAGE)
    if manual_duplex and page is last odd page:
        chunk(PAUSE)                               # type 0x0b, then even pages
chunk(END_DOC)
UEL
```

---

## 3. Structures

### 3.1 Chunk header — 16 bytes, **big-endian**

```c
struct ZJ_HEADER {          /* all big-endian */
    uint32_t size;          /* total chunk size, including these 16 bytes */
    uint32_t type;          /* ZJT_*                                    */
    uint32_t nitems;        /* number of ZJ_ITEM records in the payload             */
    uint16_t item_bytes;    /* nitems * 12                              */
    uint16_t signature;     /* always 0x5a5a                            */
};
```

Payload = `nitems` records of `ZJ_ITEM` (12 bytes), followed by "raw" bytes
(`size - 16 - item_bytes`). The `JBIG_BIH`/`JBIG_BID`/`0x11` chunks have no records,
`item_bytes == 0`, but do carry a raw payload — that is, `item_bytes` counts **only** items.

**This is exactly the foo2zjs format.** Verified: `zjsdecode` from `foo2zjs` parses
the vendor's stream without errors if you strip the UEL and the Pantum chunk `0x11`
and leave `JZJZ` at the start (see §7). The difference between Pantum and HP/Minolta
is only the byte order and the extra chunk `0x11`.

### 3.2 Parameter record — 12 bytes, big-endian

```c
struct ZJ_ITEM {
    uint32_t size;          /* always 12          */
    uint16_t type;          /* ZJI_*              */
    uint8_t  param;         /* always 1           */
    uint8_t  reserved;      /* 0                  */
    uint32_t value;
};
```

### 3.3 Chunk types (numbering matches foo2zjs)

| code | name | emitted by | note |
|---|---|---|---|
| 0x00 | `ZJT_START_DOC` | `Prnt_StartDoc` | 3 items |
| 0x01 | `ZJT_END_DOC` | `Prnt_EndDoc` | empty |
| 0x02 | `ZJT_START_PAGE` | `Prnt_StartPage` | 14 items (17 for custom size) |
| 0x03 | `ZJT_END_PAGE` | `Prnt_EndPage` | empty |
| 0x04 | `ZJT_JBIG_BIH` | `Prnt_SendPage` | 20-byte BIH |
| 0x05 | `ZJT_JBIG_BID` | `Prnt_SendPage` | ≤ 65536 bytes of data |
| 0x06 | `ZJT_END_JBIG` | `Prnt_SendPage` | empty |
| 0x0b | `ZJT_2600N_PAUSE` | `main` (manual duplex) | empty, "flip the stack" |
| **0x11** | **Pantum-specific** | `Prnt_StartDoc` | 72 bytes of constants, foo2zjs has nothing like it |

Not seen: `ZJT_SIGNATURE` (7), `ZJT_RAW_IMAGE` (8), `ZJT_START_PLANE` (9),
`ZJT_END_PLANE` (10). None of the disassembled output paths
(`Prnt_StartDoc`/`StartPage`/`SendPage`/`EndPage`/`EndDoc` + `OutputJbigkitCompression`)
produce these types, and none of the streams captured from the filter contain them.
**The vendor filter does not use the uncompressed `RAW_IMAGE` mode**, so it cannot be
relied on as a "simpler first stream" — whether the firmware supports it is unknown,
and there is no point testing that blind on the device now that JBIG has already
been reproduced byte for byte.

### 3.4 Item types

Names on the left are canonical (confirmed by `zjsdecode` from foo2zjs, see §7);
on the right is what the vendor filter itself calls them in its debug strings.

| code | name (foo2zjs) | vendor name | source of the value |
|---|---|---|---|
| 0x00 | `ZJI_PAGECOUNT` | — | constant 0 |
| 0x01 | `ZJI_DMCOLLATE` | — | constant 0 |
| 0x02 | `ZJI_DMDUPLEX` | — | constant 1 (`DMDUP_SIMPLEX`) |
| 0x03 | `ZJI_DMPAPER` | `PAPER_SIZE_PARAM` | paper code from the table (§4.1) |
| 0x04 | `ZJI_DMCOPIES` | `COPIES_PARAM` | `NumCopies` from the raster header |
| 0x05 | `ZJI_DMDEFAULTSOURCE` | `INPUT_TRAY_PARAM` | tray, see §4.3 |
| 0x06 | `ZJI_DMMEDIATYPE` | `MEDIA_TYPE_PARAM` | `cupsMediaType` from the raster header |
| 0x07 | `ZJI_NBIE` | `CHANNEL_NUM_PARAM` | constant 1 (one BIE per page) |
| 0x08 | `ZJI_RESOLUTION_X` | `H_RESOLUTION_PARAM` | `HWResolution[0]` (always 600) |
| 0x09 | `ZJI_RESOLUTION_Y` | `V_RESOLUTION_PARAM` | `HWResolution[1]` (always 600) |
| 0x0c | `ZJI_RASTER_X` | `IMG_WIDTH_PARAM` | `VIDEO_X * VIDEO_BPP` |
| 0x0d | `ZJI_RASTER_Y` | `IMG_HEIGHT_PARAM` | `VIDEO_Y` |
| 0x10 | `ZJI_VIDEO_BPP` | `VIDEO_DEPTH_PARAM` | 1, or 2 when `DPI1200=True` |
| 0x11 | `ZJI_VIDEO_X` | `VIDEO_WIDTH_PARAM` | raster width (§4.2) |
| 0x12 | `ZJI_VIDEO_Y` | `VIDEO_HEIGHT_PARAM` | raster height |
| 0x16 | `ZJI_RET` | `RET_PARAM` | constant 0 |
| 0x18 | — | `CUSTOM_WIDTH_PARAM` | only if `DMPAPER == 0` |
| 0x19 | — | `CUSTOM_HEIGHT_PARAM` | only if `DMPAPER == 0` |
| 0x1a | — | `CUSTOM_PAPER_UNIT_PARAM` | constant 0 |
| 0xff00 | — | `DENSITY_PARAM` | density/toner saving (§4.4) |

The order of items in `START_PAGE` is fixed, exactly as in the byte map above
(note: `RET` (0x16) sits **between** `RESOLUTION_Y` and `RASTER_X`, not in
ascending code order). Sizes in the `START_PAGE` header:

```
size       = 0xb8            (0xdc, if DMPAPER == 0)
nitems     = 14              (17, if DMPAPER == 0)
item_bytes = 0xa8            (0xcc, if DMPAPER == 0)
```

The three custom items (`0x1a`, `0x18`, `0x19` — in exactly this order) are inserted
right after `ZJI_DMPAPER`.

---

## 4. Mapping PPD/command-line parameters onto the stream

The filter reads options from `argv[5]` via `cupsParseOptions` and `ppdMarkOptions`,
using the PPD keyword as the key. If an option is not found, `GetSelectOption`
substitutes the string `"no<Name>"` (strings `noDensity`, `noDPI1200`, `noTonerMode`,
`noNegativePrint`, `noImageRotation`, `noFeedLongEdge` in `.rodata`).

### 4.1 `PageSize` → `ZJI_DMPAPER`, `ZJI_VIDEO_X/Y`

Table `gPaperSizeArray_PT2500_PTM6500` @ `0x616020`, 40-byte records,
terminated by `width == -1`:

```c
struct paper_entry {
    int32_t width_pt, height_pt;   /* sheet size in PostScript points */
    int32_t video_x,  video_y;     /* raster size in 600 dpi dots    */
    int32_t dmpaper;               /* ZJI_DMPAPER value              */
    int32_t reserved;              /* 0                                 */
    char    name[16];
};
```

Lookup (`GetPaperArrayIndex`) is **by the (width_pt, height_pt) pair from the
CUPS raster header** (`PageSize[2]`), not by name.

| PageSize | pt | VIDEO_X × VIDEO_Y | DMPAPER |
|---|---|---|---|
| A5 | 420×595 | 3264×4736 | 11 |
| A4 | 595×842 | 4736×6784 | 9 |
| Letter | 612×792 | 4864×6368 | 1 |
| Legal | 612×1008 | 4864×8192 | 5 |
| B5 (JIS) | 516×729 | 4064×5856 | 13 |
| EnvMonarch | 279×540 | 2112×4288 | 37 |
| EnvDL | 312×624 | 2368×4992 | 27 |
| EnvC5 | 459×649 | 3616×5184 | 28 |
| Com10Envelope | 297×684 | 2240×5472 | 20 |
| Postcard | 283×420 | 2144×3264 | 43 |
| 16K | 524×737 | 4160×5920 | 263 |
| Big16K | 553×765 | 4384×6144 | 264 |
| 32K | 369×524 | 2848×4160 | 266 |
| Big32K | 383×553 | 2976×4384 | 260 |
| A6 | 298×420 | 2272×3264 | 70 |
| ISOB5 | 499×709 | 3936×5696 | 34 |
| Executive | 522×756 | 4128×6080 | 7 |
| Folio | 612×936 | 4864×7584 | 14 |
| Oficio | 612×972 | 4864×7872 | 269 |
| Statement | 396×612 | 3072×4864 | 6 |
| EnvC6 / Yougata2 | 323×459 | 2464×3616 | 31 |
| ZL | 340×652 | 2624×5216 | 270 |
| B6 | 354×499 | 2720×3936 | 271 |

**Special case in `GetPaperArrayIndex`** @ `0x405eb0` — a page of `421×595` pt
(0x1a5×0x253) is tested **before** the table is searched, answers with the A5
entry (index 0) and raises a flag through the function's third argument. On
platform 3 that flag means "A5 lying on its side":

* `ZJI_DMPAPER` = **61** instead of 11. 61 is `DMPAPER_A5_ROTATED` in the
  DEVMODE numbering the rest of these codes come from;
* both the sheet size and the video size the page carries are swapped
  (`SwapUSHORT` on each pair), so `ZJI_VIDEO_X` = 4736 and `ZJI_VIDEO_Y` =
  3264. `ZJI_RASTER_X` follows the usual rule and is `VIDEO_X * dpiBit`, i.e.
  9472 under `DPI1200`;
* the grey page is turned 90° **clockwise** by `Rotate90()` @ `0x406040` — a
  64×64 blocked byte transpose, `dst[y][x] = src[h-1-x][y]` — *after* the 180°
  turn `ImageRotation` asks for, and *before* halftoning, so the halftone
  screen stays anchored to the sheet as it will be fed. The direction was
  measured by calling the function on a 3×4 test buffer in a debugger;
* no item is added or removed: the chunk is the same 14-item `START_PAGE` any
  page from the table gets.

No `PageSize`, `PaperDimension` or `CustomPageSize` entry in any of the 335 PPDs
of the vendor package asks for 421×595, and `cupsfilter` snaps a
`Custom.421x595` request onto A5 (420×595) before the raster is written, so
nothing in either driver reaches this branch by itself. It is implemented
anyway, and verified byte for byte against the vendor filter on rasters whose
header was relabelled 421×595 — regression cases `A5Rot*`.

Two quirks of the vendor's implementation, both reproduced experimentally:

* **The flag is per job, not per page.** `GetPaperArrayIndex` writes it only
  when the special case hits and `main()` never clears it, so once one
  421×595 page has gone by, every later A5 page of the same job comes out
  turned as well. Verified on a two-page raster: `421×595` then `420×595`
  gives `DMPAPER` 61 twice, the same two pages in the other order give 11 then
  61. Our filter decides per page and deliberately does **not** copy this.
* **The blank sheet manual duplex adds to an odd page count contradicts its
  own header.** Its `START_PAGE` announces the turned video size (4736×3264)
  while the JBIG image inside is 3264×4736, because the swap that goes with
  the turn happens in the raster loop and a blank page never enters it. Copied
  as found, so the streams stay identical.

**Formats not in the table** (in the PPD these are `Yougata4`, `JapanesePostcard`,
`Younaga3`, `Nagagata3`) → `ZJI_DMPAPER = 0` plus three custom items:

```
ZJI_CUSTOM_UNIT   (0x1a) = 0
ZJI_CUSTOM_WIDTH  (0x18) = page_width_pt  * 600 / 72     /* hundredths — actually 1/600-inch units */
ZJI_CUSTOM_HEIGHT (0x19) = page_height_pt * 600 / 72
VIDEO_X = round_up(cupsWidth,  32)
VIDEO_Y = round_up(cupsHeight,  8)
```
(verified on all four: Yougata4 298×666 pt → 2483×5550, raster 2272×5320.)

### 4.2 Resolution: `DPI1200`

`HWResolution` in the PPD is **always `[600 600]`** for both variants of `DPI1200`,
and `ZJI_RESOLUTION_X/Y` is always 600. "1200 dpi" is implemented differently —
`is1200dpiprint()` sets `g_dpiBit = 2`, and then:

| | `DPI1200=False` | `DPI1200=True` |
|---|---|---|
| `ZJI_VIDEO_BPP` | 1 | 2 |
| `ZJI_RASTER_X` | `VIDEO_X` | `VIDEO_X * 2` |
| `ZJI_VIDEO_X` | `VIDEO_X` | `VIDEO_X` (unchanged) |
| JBIG image width | `VIDEO_X` | `VIDEO_X * 2` |
| halftoning | 1 bit/pixel, threshold | 2 bit/pixel, 4 levels |

That is, each original 600 dpi pixel is encoded as a pair of horizontal
1200 dpi subpixels (codes 0..3 = `00`/`01`/`10`/`11`). Details in §6.2.

### 4.3 `MediaType` and tray

`ZJI_DMMEDIATYPE` is taken **not from the filter options, but from the `cupsMediaType`
field of the CUPS raster header** (set by `*MediaType` in the PPD via `setpagedevice`).
Values: Plain=1, Transparency=2, Envelope=259, Label=261, Thin paper=263,
Cardstock=286, Thick=289. If `MediaType=Thick` is passed only to the filter but not
to the rasterizer, the stream still shows 1 — verified.

`ZJI_DMDEFAULTSOURCE` (tray) is tied to the media type:

```c
tray = 0;                                   /* InputSlot=Auto (default)       */
if (!strcasecmp(InputSlot, "ManualFeed")) tray = 1;
if (!strcasecmp(InputSlot, "AutoFeed"))   tray = 2;
if (cupsMediaType != 1) tray = 1;           /* anything but Plain -> manual feed */
```

Verified: any non-Plain media type gives `tray = 1` regardless of `InputSlot`;
the `ManualFeed` and `MediaPosition` fields of the raster header are not read.
The M6500 PPD has no `InputSlot` option, but the filter understands it.

### 4.4 `Density` and `TonerMode` (toner saving)

Both options are written **into the same item `0xff00`** (`DENSITY_PARAM`),
as the low byte of `settings[0x12]`:

| options | `0xff00` |
|---|---|
| default (`Density=2`) | 2 |
| `Density=0` (Lighter) | 0 |
| `Density=4` (Darker) | 4 |
| `TonerMode=True` (saving) | **0** |

That is, "toner saving" on the M6500 is a forced minimum density,
with no dedicated bit of its own. `GetTonerSaveCmd`/`GetDensityCmd` (ACL 0x19/0x15)
**are never called** in the platform 3 branch.

### 4.5 `NegativePrint`, `ImageRotation`

Both only change the raster content, adding no items:

* `NegativePrint=True` — flips the halftoning decision
  (`pixel >= threshold` instead of `pixel < threshold`); margins that are white
  in normal mode become **black** (§6.1);
* `ImageRotation=True` — the image is rotated 180° **before halftoning**:
  the grey buffer `out_w × out_h` is flipped, while the threshold matrix stays
  anchored to the sheet (the threshold is looked up by the **output** pixel's
  coordinates). Rotating the already-packed bitmap instead would only match on
  a purely black-and-white image (which has no halftoning at all) and would
  diverge on any halftone — verified byte for byte on a "noise" page.

The quarter turn a 421×595 pt sheet asks for (§4.1) is a third operation on the
same buffer, applied after this one and likewise before halftoning.

### 4.6 Duplex

| mode | what happens |
|---|---|
| hardware (`machineDuplexCmd` 0x40) | **unavailable**: `SendMachineDuplexCmd` is only called for platform 1/2. `ZJI_DMDUPLEX` in `START_DOC` is always = 1 (simplex) |
| manual (`ManualDuplex=True`) | `main()` copies the stream into two temp files (`gManualDuplexOddFile` / `gManualDuplexEvenFile`), outputs all odd pages first, then the **`ZJT_2600N_PAUSE` (0x0b)** chunk, then the even pages — **in reverse order**, padded to an even number of sheets (see below) |
| `manualDuplexCmd_Z` (0x1b) / `_C` (0x30) | ACL commands of other platforms, not used in the M6500 branch |

Exact layout of manual duplex (verified byte for byte on 1/3/4/5-page documents):

```
odd pages  1, 3, 5, …            in forward order
chunk(ZJT_2600N_PAUSE)
even pages    …, 6, 4, 2            in REVERSE order
```

The reverse order is needed because the operator flips the whole stack at once:
the sheet printed last ends up on top.

If the page count is odd, the last sheet has no back side, and the vendor adds
a **blank** page — it comes first in the back-side block (i.e. last in the list
before the flip). This page is output as a bitmap **of all zeros**, bypassing
halftoning entirely: with `NegativePrint=True` an ordinary blank page would come
out solid black, but this one stays toner-free.
Verified: `ManualDuplex=True` on a one-page document gives
`… END_PAGE, chunk(0x0b), START_PAGE(blank page) … END_DOC`, and on a
three-page document: `1, 3, PAUSE, blank, 2`.

### 4.7 What does NOT affect the stream

`FeedLongEdge`, `Collate`, `MediaPosition`, and `*cupsIPPReason MDuplex`
(this is just a hint text shown to the user).

`argv[4]` (copy count) is fully ignored by the filter: `ptm6500Filter 1 user title 3 ""`
gives **byte for byte the same stream** as with `1` — there is no page repetition
in the stream at all, multiplying copies stays on the cupsd side.

`ZJI_DMCOPIES` is taken from `NumCopies` in the **raster header**, not from
`argv[4]`: `ptm6500Filter 1 user title 3 ""` with `NumCopies = 1` gives
`DMCOPIES = 1`, while a raster produced by `cupsfilter -o copies=3` gives
`DMCOPIES = 3` — verified.

---

## 5. Chunk 0x11 — the constant blob

`GetTonerParamCmdForPlatformM()` @ `0x407260` builds an 88-byte structure:
16 bytes of `ZJ_HEADER` header (`size=0x58, type=0x11, nitems=0, item_bytes=0,
sig=0x5a5a`) and 72 bytes of payload. **The payload does not depend on any
parameter — it is 36 big-endian `uint16_t` values, hardcoded in the binary:**

```
0000 0051 0073 006e 0096 007a 0064 0096 00c8 0064 0051 0064
0064 0064 0064 0064 0064 0064 0064 0064 0064 0064 0064 0064
0078 0087 0096 00b4 00c8 0078 0096 00be 015e 0190 0064 0064
```
in decimal:
```
0, 81, 115, 110, 150, 122, 100, 150, 200, 100, 81,
100 ×13,
120, 135, 150, 180, 200, 120, 150, 190, 350, 400, 100, 100
```

Judging by the function name, this is likely a table of fuser/toner parameters
(percentages and voltages). For the open filter it is enough to **output it as is**.
Note: in `notes/status-protocol.md` this block is marked as "not an ACL packet" — correct,
this is a ZjStream chunk, not an ACL packet.

---

## 6. Page data

### 6.1 Halftoning (`HalftoneDither` @ `0x40abc0`, `platform == 3` branch)

Input — CUPS raster 8 bit/pixel, `cupsColorSpace = 0` (`CUPS_CSPACE_W`:
0 = black, 255 = white), `cupsBitsPerPixel = 8`, resolution 600×600.

```c
/* out_w x out_h — raster size from the paper table (§4.1) */
/* the grey buffer is allocated UP FRONT at out_w x out_h and filled with 0xff (paper),
   then the CUPS raster lines are copied into it — hence the margin behavior under negative */
for (y = 0; y < out_h; y++)
  for (x = 0; x < out_w; x++) {
      uint8_t thr = HT_TABLE_600[(y % 16) * 16 + (x % 16)];
      int bit = negative ? (gray[y][x] >= thr) : (gray[y][x] < thr);
      put_bit(bit);                 /* 1 = black (toner), MSB first */
  }
```

`HT_TABLE_600` @ `0x615f20` — 16×16 threshold matrix (clustered dot):

```
06 52 e9 fe fb e2 46 05 07 55 eb ff fc e4 49 05
37 79 a5 da d3 98 72 31 39 7a a7 dc d5 9c 73 2f
c7 86 69 29 1b 5f 7e c0 ca 88 6b 2d 1f 61 7b be
f3 b4 12 03 01 0d ac ee f4 b6 13 03 01 0c aa ed
f8 de 41 04 0a 4f e8 fd fa e0 43 04 09 4c e6 fc
d0 91 6d 35 3e 77 a2 d8 d2 95 70 33 3b 75 9f d7
16 58 83 c5 ce 8e 66 26 18 5b 80 c3 cc 8b 64 22
02 10 b1 f1 f7 bb 15 02 01 0f af f0 f5 b9 14 02
07 55 eb ff fc e4 49 05 06 52 e9 fe fb e2 46 05
39 7a a7 dc d5 9c 73 2f 37 79 a5 da d3 98 72 31
ca 88 6b 2d 1f 61 7b be c7 86 69 29 1b 5f 7e c0
f4 b6 13 03 01 0c aa ed f3 b4 12 03 01 0d ac ee
fa e0 43 04 09 4c e6 fc f8 de 41 04 0a 4f e8 fd
d2 95 70 33 3b 75 9f d7 d0 91 6d 35 3e 77 a2 d8
18 5b 80 c3 cc 8b 64 22 16 58 83 c5 ce 8e 66 26
01 0f af f0 f5 b9 14 02 02 10 b1 f1 f7 bb 15 02
```

**An important detail about margins.** `cupsWidth`/`cupsHeight` are smaller than
`VIDEO_X`/`VIDEO_Y` (for A4: 4724×6780 vs. 4736×6784). The vendor halftones the
**whole** `out_w × out_h` buffer, pre-filled with `0xff`. So:

* in normal mode the margins are white (`255 < thr` is false for any `thr ≤ 255`);
* with `NegativePrint=True` the margins are **black** (`thr <= 255` is true).

This is the only place where a naive implementation ("zero padding") diverges
from the vendor — verified byte for byte.

### 6.2 `DPI1200` mode — 2 bits per pixel

```c
uint8_t thr = HT_TABLE_600[(y % 16) * 16 + (x % 16)];
uint8_t b   = gray[y][x];
int lvl = (b == 0) ? 0xff : max(0, 0xff - b - thr);
int code = HT_TABLE_2BIG[lvl];          /* 0..3 */
if (negative) code = 3 - code;
put_2bits(code);                        /* 4 pixels per byte, high bits first */
```

`HT_TABLE_2BIG` @ `0x611200` — 256 bytes: `[0] = 0`, `[1..23] = 1`,
`[24..63] = 2`, `[64..255] = 3`.

The resulting bitstream is fed into JBIG as an image of width `VIDEO_X * 2`,
i.e. each two-bit code is two adjacent 1200 dpi pixels.

### 6.3 JBIG — it's exactly `jbig-kit`

`pantum_compress()` @ `0x40cf7e` still carries the assert strings of the source
file it was built from, and they name a private build of `jbig.c` — this is a
fork of Markus Kuhn's JBIG-KIT (the same code as `jbig.c` in foo2zjs). It
implements the **LRLTWO
template (two-line, 10-bit context) with the typical TPBON prediction**; the TP
context is `0x195` (`TPB2CX` from JBIG-KIT). Strips of `L0` lines; the context
states (`ST`/`MPS`) **are not reset between strips**, each strip is a separate
arithmetic word.

BIH header (20 bytes, big-endian, built by `SetJBIGCmdAndParam` @ `0x407a10`):

| field | offset | value on the M6500 |
|---|---|---|
| `DL` | 0 | 0 |
| `D` | 1 | 0 |
| `P` | 2 | 1 |
| `-` | 3 | 0 |
| `XD` | 4..7 | `VIDEO_X * VIDEO_BPP` |
| `YD` | 8..11 | `VIDEO_Y` |
| `L0` | 12..15 | **256** |
| `MX` | 16 | **0** |
| `MY` | 17 | 0 |
| `order` | 18 | **3** (`ILEAVE | SMID`) |
| `options` | 19 | **0x5c** (`LRLTWO | TPDON | TPBON | DPON`) |

(`TPDON`/`DPON` have effectively no effect when `D = 0`, `P = 1`, but the byte
in the stream is exactly this value regardless.)

**Practical conclusion — the equivalent command:**

```
pbmtojbg -q -m 0 -s 256 -p 92 -o 3  page.pbm  page.jbg
```

produces **byte for byte the same BIE** (BIH + BID) as the vendor filter.
Verified on a blank, fully black, "square" and noise page, and also in `DPI1200`
mode. That is, an open filter can simply link against `libjbig` (`jbg_enc_init`,
`jbg_enc_options(&s, 3, 0x5c, 256, 0, 0)`, `jbg_enc_out`).

Correspondence of jbig-kit flags:
`JBG_LRLTWO = 0x40`, `JBG_TPDON = 0x10`, `JBG_TPBON = 0x08`, `JBG_DPON = 0x04`;
`JBG_ILEAVE = 0x02`, `JBG_SMID = 0x01`.

### 6.4 Slicing the BID into chunks

`Prnt_SendPage` @ `0x40b8e0`: the whole page is compressed in a single call
(`OutputJbigkitCompression` → `JBIGCompress` → `pantum_compress`), the result
accumulates in `gpCompImgData`, then is sliced:

```
while (left >= 0x10000) { chunk(JBIG_BID, size = 0x10010, payload 0x10000); left -= 0x10000; }
if (left)                 chunk(JBIG_BID, size = left + 0x10, payload left);
chunk(END_JBIG);
```

Verified on a page with 623 KB of compressed data — 9 full chunks plus a tail.

---

## 7. Cross-checking via `zjsdecode` (foo2zjs)

`zjsdecode`, from the foo2zjs package. It expects a stream with no UEL and
does not know about the `0x11` chunk, so:

```bash
SIZE=$(stat -c%s square.out)
{ printf 'JZJZ'; tail -c +102 square.out | head -c $((SIZE - 101 - 9)); } | zjsdecode
```
(102 = 0x65 + 1 — the start of `START_DOC`; the tail is trimmed by 9 bytes for the final UEL.)

Result — a complete parse without a single error, with the same item names as
in §3.4. `zjsdecode` also confirms: "27 stripes, 0 layers, 1 planes" and
`Options = 92  LRLTWO TPDON TPBON DPON`.

The other way around: `jbgtopbm` from `jbigkit` decodes the vendor's `BIH ‖ BID`
into a correct PBM (verified — on the "square" page the black pixels fall
exactly in rows 1065..1899 and columns 716..1549).

---

## 8. Tools in `proto/`

| file | what it does |
|---|---|
| `zjs_dump.py` | parses Pantum's ZjStream: chunks, items, raw payloads |
| `rastertopantum.py` | prototype open filter: CUPS raster → ZjStream |
| `make_test_pdfs.py` | writes the fixed test documents the regression compares against |
| `check_against_vendor.sh` | byte-for-byte comparison against `ptm6500Filter` (77 cases); takes the filter under test as an argument, so it drives either the C filter or `rastertopantum.py` |

Dependencies: python3, ghostscript and cups-filters in PATH; the Python
prototype also needs jbigkit (`pbmtojbg`).

Capturing an "input → output" pair from the live filter:

```bash
PPD_FILE="…/Pantum M6500 Series.ppd"
cupsfilter -m application/vnd.cups-raster -p "$PPD_FILE" file.pdf > in.raster
PPD="$PPD_FILE" .../ptm6500Filter 1 user title 1 "Density=4" < in.raster > out.bin
python3 proto/zjs_dump.py out.bin
```

---

## 9. The real CUPS filter

Done: `src/rastertopantum.c` + `ppd/Pantum-M6500-open.ppd`.

* the raster is read through `libcupsraster` (`cupsRasterOpen`/`cupsRasterReadHeader2`/
  `cupsRasterReadPixels`), so a compressed CUPS raster is handled the same way as a raw one;
* JBIG — `libjbig` from jbig-kit:
  `jbg_enc_layers(&s, 0)` + `jbg_enc_options(&s, JBG_ILEAVE|JBG_SMID,
  JBG_LRLTWO|JBG_TPDON|JBG_TPBON|JBG_DPON, 256, 0, -1)`
  (this is exactly what `pbmtojbg -q -m 0 -s 256 -p 92 -o 3` does);
* manual duplex — two temp files + chunk `0x0b`, with the back sides in
  reverse order and a blank padding page (§4.6);
* the platform (PLATFORM_M) is hardcoded in the open filter, but
  `*cupsIPPReason PlatForm: "text:PLATFORM_M"` is kept in the PPD so the PPD
  stays self-contained;
* the page is held in memory line by line: no full grey buffer is allocated,
  a raster line is turned into bits right away. The one exception is the
  sideways A5 sheet of §4.1, where the quarter turn needs the whole grey page
  at once — 15 MB, and only for that one size.

Regression: `proto/check_against_vendor.sh <path-to-filter>` — 77 cases,
all match `ptm6500Filter` byte for byte.

Not implemented (and not needed): `Collate` and `argv[4]` copies — the vendor
ignores them too (§4.7).

## 10. Negative results

* **There is no uncompressed mode.** The filter neither emits nor supports
  `ZJT_RAW_IMAGE` (8); the firmware most likely doesn't either — there is no
  way to avoid JBIG.
* **The device does not understand PCL/PostScript** (confirmed by the Device ID
  and the PPD catalogue).
* **There is no hardware duplex on the M6500/M6507 in this branch** — manual only.
* **There is no back channel:** the filter reads nothing from the device,
  `cupsSideChannelDoRequest` resolves but is never called (see `notes/status-protocol.md` §3).
