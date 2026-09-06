# Pantum M6507 (`M6500 series`) firmware — image structure

Where things live in an M6500-series printer's flash and RAM, so that the device
itself can be used as a source when the vendor driver has nothing to say. Written
from dumps taken **read-only** over the ACL channel (`@PJL ENTER LANGUAGE=ACL`,
opcode `0x00AC`, sub-opcode `0x0009` = read memory) with `proto/acl_read_mem.py`;
§0 is the recipe.

The dumps themselves are not published — see the caution in §0 — so this is a map
rather than an extract: addresses, structure shapes and what each region turns out
to hold. Deliberately kept general, because the same firmware stack runs across
Pantum's lineup: addresses shift between models, but the table structures and the
anchors named in §4 and §5 should survive the move.

Device: Pantum M6507, USB `232b:0e20`, `@PJL INFO ID` → `M6500 series`.
Firmware version in the NVRAM area: `3.2.6.7`; bootloader build date:
`TIME=Tue Dec 05 10:05:42 2023`.

---

## 0. How to take a dump

```bash
cd <repository root>
# SPI flash (16 MiB) — about 40 seconds
sg lp -c 'python3 proto/acl_read_mem.py \
          0xF6000000 0x1000000 flash.bin'
# DRAM, the unpacked running image (first 64 MiB) — about 140 seconds
sg lp -c 'python3 proto/acl_read_mem.py \
          0x00000000 0x4000000 ram.bin'
```

Speed is bounded by the number of round trips, not by USB: a 4 KiB block gives ~1.5 KiB/s,
a 256 KiB block gives ~450 KiB/s. `proto/acl_read_mem.py` always reads in 256 KiB chunks.

Response header (16 bytes, big-endian): `00AC` `0009` `0001` `<BE32 length>` `000000000000`.

> **Caution.** Reading is safe, but on this family opcode `0x00AC` also has
> sub-opcodes for writing/erasing flash (`foo2zjs` flashes firmware into the
> HP LJ 1000/1018/1020 over the same `@PJL ENTER LANGUAGE=ACL` channel).
> Do not brute-force sub-opcodes blindly.
>
> **Do not publish a dump.** The tail of the flash holds the device's private TLS
> key, its certificate, the SNMP community string and the unit's serial number. A
> firmware image is also the vendor's code. Keep dumps local; what they establish
> belongs in notes like this one, the images themselves nowhere.

---

## 1. Address-space map

| Range | What it is |
|---|---|
| `0x00000000` … roughly `0x01FB0000` | DRAM: the unpacked running firmware image (code + rodata) |
| roughly `0x01FB0000` … `0x04000000`+ | DRAM: `.data`/`.bss`, heaps, page buffers; reads in full |
| `0xF6000000` … `0xF7000000` | SPI NOR flash, 16 MiB, memory-mapped |

`@PJL INFO MEMORY` reports `268435456` (256 MiB) — this is the memory
available for printing, not the size of the RAM module.

## 2. SPI flash `0xF6000000` (16 MiB)

| Range | Size | Contents |
|---|---|---|
| `0xF6000000`–`0xF6010000` | 64 KiB | bootloader: ARM vector table (`ldr pc,[pc,#0x18]` ×8), handlers for `Unexpected FIQ` / `Undefined exception.` / `SWI exception.` / `Prefetch exception.` / `Abort exception.` / `Reserved exception.`, the string `TIME=<build date>;CHGLST=`, Thumb code |
| `0xF6010000`–`0xF6020000` | 64 KiB | erased (0xFF) |
| `0xF6020000`–`0xF6B50000` | ~11.2 MiB | the **packed** main image. Uniformly high entropy, no readable strings — nothing to unpack here, work needs to happen on the copy in DRAM instead |
| `0xF6B50000`–`0xF6FA0000` | ~4.3 MiB | erased (0xFF) — headroom for updates |
| `0xF6FA0000`–`0xF6FE0000` | 256 KiB | **NVRAM/configuration**: paper-format names (`Letter`, `Legal`, `JIS B5`, `Monarc`, `DL Env`, `C5 Env`, `No.10`, `Big 16K`, `Big 32K`, `ISO B5`, `Oficio`, `Statement`, `C6 Env`, …), SNMP community `public`, credentials `admin`/`000000`, TLS certificate `Pantum Technology Cert` (2019-09-11 … 2029-09-08) and an encrypted private key, WSD name `WSD-Enabled Scanner`, the unit's serial number, the version string `3.2.6.7` (repeated many times) |
| `0xF6FE0000`–`0xF7000000` | 128 KiB | erased (0xFF) |

Flash header (`0xF6000000`):

```
+0x00  8 × e59ff018        ldr pc,[pc,#0x18]      vector table
+0x20  f6000080 f60000c4 f60000f8 f6000128        handler addresses
+0x30  f6000168 f6000198 f60001d8 f6000090
+0x40  00000000 00000000 00001014 0f785028        (length / checksum?)
+0x50  "TIME=Tue Dec 05 10:05:42 2023;CHGLST="
```

## 3. DRAM: the running image

The vectors at `0x00000000` are no longer `ldr pc`, but `b`:
`ea222636` → `0x0088_98E0`, where the strings `***undefined isr***`,
`***prefetch isr***`, `***abort isr***` also live.
So the kernel/handlers live around `0x00889000`.

Rough layout (by density of printable characters, in 64 KiB steps):

| Range | What |
|---|---|
| `0x00000000`–`0x0108B000` | code (little-endian ARM32, with some Thumb blocks) + literal pools |
| `0x0108B000`–`0x0108F000` | ASCII symbol tables (font/conversion tables) |
| `0x0108FBC9` | **status-manager name pool**, `char[196][30]` — see §5 |
| `0x01092183`–`0x010DA163` | **panel strings, 13 language blocks**, each `char[794][32]`, stride `0x6340` — see §4 |
| `0x0112AA74` | PJL-status classifier constants: 5000/10000/20000/30000/40000/50000 |
| `0x0119C740` | RTOS banner: `Copyright (c) 1996-2001 Express Logic Inc. * ThreadX ARM9/ARM Version G4.0.4.0 *` |
| `0x017CB650`–`0x017D0000` | debug strings for the ACL web bridge (`[acl]…`) and an identifier table `STRING_*` (359 names) |
| `0x017CF000`–`0x017D3000` | a name table `OID_*` (494 names) — an internal object model accessible from EWS/SNMP |
| `0x017D0000`–`0x01AE0000` | the **embedded web server (EWS)**: HTML/JS/CSS and localization into ~15 languages, copyrights `Zhuhai Pantum Electronics` (2015) and `Zhuhai Seine Technology` (2013) |
| `0x0199_7F00`–`0x0199_8600` | the panel module and status manager: `LCD_FillLcdLineBufferFromEmwin`, `UI_pwrsave: …`, path `mfp_uiapp_status.c`, a job-type list `ejob_*` |
| `0x019B8000`–`0x019C2000` | the PJL keyword table (`ONLINE`, `USTATUS`, `USTATUSOFF`, …) |
| `0x01A26D7C`–`0x01A27700` | Pantum's PJL layer: `PJL PARSER:@PJL …`, `PJL STATUS:@PJL USTATUS DEVICE STATUS=%x,CODE=%d,TONER=%d` |
| `0x01AE296C` | asserts from Pantum's own toner-interface module (`print_mech/toner`) |
| `0x01DBA164` | `../hp/hpstat.c` |
| `0x01F73000`–`0x01FA9C60` | assert strings from the licensed PDL stack: `../pjl/*.c`, `../pdi/*.c`, `../pdi1/*.c`, `../xl/*.c`, `../gl2/*.c`, `../hp/*.c`, `../fontfusion/*.c`, `../st/*.c`, `../dl/*.c` |
| `0x01F96FE0`+ | Unicode ↔ CJK recoding tables |
| `0x01FB0000`+ | `.data` / `.bss` / heaps |

## 4. Panel strings

13 blocks, each `char[794][32]`, the first at `0x01092183`, stride `0x6340`
(25408 bytes). Slot order is the same across all languages, so the table works
as `text[lang][slot]`.

| Block index | Address | Language (based on slot 31) |
|---|---|---|
| 0 | `0x01092183` | Chinese (GBK) |
| 1 | `0x010984C3` | **English** (`Initializing...`) |
| 2 | `0x0109E803` | French (`Initialisation...`) |
| 3 | `0x010A4B43` | Italian (`Inizializz...`) |
| 4 | `0x010AAE83` | Chinese (variant) |
| 5 | `0x010B1243` | Norwegian (`Initialiserer...`) |
| 6 | `0x010B2D83` | Russian (CP encoding) |
| 7 | `0x010B90C3` | Spanish (`Iniciándose...`) |
| 8 | `0x010BF483` | Swedish (`Initierar...`) |
| 9 | `0x010C0FC3` | Chinese (variant) |
| 10 | `0x010CD643` | German (`Wird initiiert`) |
| 11 | `0x010D3983` | Portuguese (`Inicializando...`) |
| 12 | `0x010DA163` | (empty/reserved) |

The full English block sits at `0x010984C3` in a DRAM dump;
the status/error slots are laid out in `notes/error-catalogue.md` §6.4.
Slot 29 is `   MB`, slot 30 is `V0.0.0.0` (handy anchors for locating the blocks in
another model's image).

## 5. Status manager (`mfp_uiapp_status.c`)

The panel's status manager. Its assert strings identify it as
`mfp_uiapp_status.c`, under the control-panel subtree of the platform's OEM
layer (see §6 for what that layer is).

Name pool — `char[196][30]` @ `0x0108FBC9`; referenced from code by a literal @ `0x00073564`.
The pool is shared across several enums, with the blocks laid out in declaration order:

| Indices | Block |
|---|---|
| 0–9 | digit strings `"0"`…`"9"` |
| 10–40 | scopes: `overall`, `module`, `system`, `platform`, `print`, `consumables`, `scan`, `contscan`, `copy`, `idcopy`, `contcopy`, `wireless`, `wps`, `network`, `oem-module`, `link`, `config_v4`, `config_v6`, `up`, `down`, `configured`, `not_configured`, `configuring`, `deprecated`, `registered`, `unkown`, `int`, `str`, `NetDrvr`, `ethernet`, `WLAN` |
| 41–88 | **states** (`state`) |
| 89–92 | field labels: `scope`, `state`, `event`, `type` |
| 93–102 | message classes: `UNUSED`, `MSG_READY`, `MSG_JOB`, `MSG_CONFIRM_PROMPT`, `MSG_TRANSITORY`, `MSG_ALERT`, `MSG_WARN`, `MSG_NOTIFY`, `MSG_ANNOUNCE`, `MSG_FATAL` |
| 103–159 | **events**, first wave |
| 160–180 | internal parameter names: `pcnt`, `pt1`/`pv1` … `pt10`/`pv10` |
| 181–194 | **events**, added later |
| 195 | `end of array` — end marker |

The decoding of blocks 41–88 and 103–194 is in `notes/error-catalogue.md` §6.2 and §6.3.

Neighboring debug strings from the same module show the data model:
`sts_msg:jobT:%d`, ` P:msgT:%d,M:%d,s:%d`, ` S:…`, ` C:…`, ` Comb:…`,
`combsts_set:sts:%d`, `combsts_set:idle`, `combsts_set:comm. err` —
i.e. the printer (`P`), scanner (`S`), and copier (`C`) each have their own status,
and the panel shows their **combination**. This explains why
`@PJL INFO STATUS` doesn't change while scanning: PJL reports the print branch's
status, not the combined one.

Job types (`ejob_*`, `0x01998380`+): `ejob_NULL`, `ejob_SmJobScanApp`,
`ejob_ScanToMemCard`, `ejob_ScanToEmail`, `ejob_GsoapScanApp`, `ejob_WsdScanApp`,
`ejob_HttpScan`, `ejob_ThumbnailPhoto`, `ejob_DemoScanApp`, `ejob_ScanCal`,
`ejob_ScanToHost`, `ejob_ID_Copy`, `ejob_Poster_Copy`, `ejob_CopyToHost`,
`ejob_Copy`, `ejob_zjs_host_print`, `ejob_CMD`, `ejob_ACL`, `ejob_PCL`,
`ejob_PJL`, `ejob_InternalPageHttp`, `ejob_InternalPagePrint`, `ejob_PrintIO`.

## 6. Code provenance — what this stack actually is

The build left assert strings naming 269 distinct source files, and they group
into clear layers:

| Layer | Directories | What it is |
|---|---|---|
| RTOS | — | **ThreadX ARM9 G4.0.4.0**, Express Logic (© 1996–2001) |
| Print PDL stack (licensed) | `../pjl/`, `../hp/`, `../hppdl/`, `../gl2/`, `../xl/`, `../pdi/`, `../pdi1/`, `../pdixl/`, `../dl/`, `../st/`, `../mn/`, `../om/`, `../od/`, `../qman/`, `../pm/`, `../sn/`, `../io/`, `../cmpr/`, `../fs/`, `../fsi/`, `../bios/`, `../ccm/`, `../img/`, `../style.c` | a classic HP-compatible interpreter (PJL + PCL5 + PCL-XL + HP-GL/2 + a PostScript-like PDI). This is exactly where `CODE=` and all of PJL's semantics come from |
| Fonts | `../fontfusion/` | **Bitstream FontFusion** (T1/T2K/TrueType/PFR) |
| Scanning | `../common/scan/src/`, `../common/scan/apps/` | ~90 files: `scanman.c`, `scantask.c`, `scanpipe.c`, `piedma.c`, `picdma_*.c`, `cal_*.c`, `icefilter.c`, `icenet.c`; applications `copyapp.c`, `scanapp.c`, `scan_to_file_app.c`, `http_scan.c`, `icefileapp.c` |
| Pantum's OEM layer | `oem/pantum/mfp_basalt/…`, `oem/pantum/common/print_mech/toner/…` | the panel, the status manager, the toner interface |

Those build paths root the whole tree at a **Marvell MFP** platform (a
"basalt"-series board), with Pantum supplying the OEM overlay on top — the panel,
the status manager and the toner interface are Pantum's; everything below them is
licensed. The same stack presumably runs on the rest of
Pantum's MFP lineup — so addresses change, but **the table structures and names
stay the same**, and the anchors from §4/§5
(`V0.0.0.0`, `end of array`, `MSG_FATAL`, `combsts_set:`) should work on
other models too.

## 7. Interfaces the device declares

`@PJL INFO CONFIG` → `LANGUAGE [7 ENUMERATED]`:

| Language | What is known |
|---|---|
| `icefile` | the scanner's internal image format ("ICE" — the scanner block in the sources `icedma.c`, `icefilter.c`, `icefileapp.c`) |
| `URF` | Apple AirPrint raster |
| `PJL` | analyzed, see `notes/error-catalogue.md` §7 |
| `ZJS` | ZjStream — what the CUPS filter prints with (`notes/print-protocol.md`) |
| `scan` | the scanner's ASP protocol (`notes/scan-protocol.md`) |
| `CMD` | not analyzed |
| `ACL` | the debug channel; only read-memory has been analyzed (`notes/status-protocol.md` §5) |

## 8. What else can still be extracted from here

* **The "state → PJL `CODE=`" table.** Not a constant array — the functions
  referencing `CODE=%5d` (@`0x01F737C8`; literals pointing to it are
  @`0x00D2ECC4` and @`0x00D2EF78`) need to be disassembled. Recipe: load
  the DRAM dump into Ghidra as raw `ARM:LE:32:v7`, base `0x0`.
* **The stored toner counter.** The *reported* level is already solved — the
  format string at `0x01A2763C` promised `STATUS=%x,CODE=%d,TONER=%d` in the
  asynchronous PJL report, and hardware confirmed it (`notes/error-catalogue.md`
  §7). What is still unlocated is where the value is kept: the `OID_*` names
  around consumables (supply level, toner in machine, supply out, supply
  mismatch, toner zone) point at an internal object model reachable from the
  embedded web server or the `CMD` language, which nothing here has looked at.
* **Numeric values of the state and event enums.** The name pool only gives
  ordering. Differential method: dump the same `.data` region
  (`proto/acl_read_mem.py`) in two different states and diff them. Tried on a pair
  of "idle / scanning in progress" states for `0x01F00000`+4 MiB: 39 32-bit
  words change, but all of them are heap queue pointers around `0x02090B50`,
  not the state variable itself. Need to look lower, in the `.bss` of the panel
  module itself.
* **Firmware update.** `pt2013upgradeFilter` in the driver, and the write
  sub-opcodes of opcode `0x00AC` — **do not touch**.

## Which flash regions change while printing

Two full flash images taken about ten printed pages apart differ in exactly
six 64 KiB erase blocks, all at the tail of the device:

| Address | Differing bytes |
|---|---|
| `0xF6F60000` | 64563 |
| `0xF6F70000` | 11870 |
| `0xF6FA0000` | 64651 |
| `0xF6FB0000` | 65261 |
| `0xF6FC0000` | 65260 |
| `0xF6FD0000` | 61805 |

Everything below `0xF6F60000` — the bootloader and the packed firmware image
— is byte-identical between the two runs, which is the expected shape: those
regions are only touched by a firmware update.

The changed blocks are rewritten wholesale rather than patched in place, so a
diff does not isolate individual fields: a flash sector is erased and written
back as a unit. `0xF6FA0000` onwards contains records on a 184-byte stride
whose fields age towards `0xFFFF`, which reads like a wear-levelled log.

**No page counter was found this way.** Searching both dumps for a 16- or
32-bit value, either endianness, that grew by the number of pages printed
between them returns nothing once erase patterns are excluded. So the counter
is either encoded, derived from the log records rather than stored as a
number, or kept outside the region examined. `@PJL INFO PAGECOUNT` returns an
empty body on this model, so it is not reachable that way either.
