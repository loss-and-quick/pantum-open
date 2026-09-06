# Printer status and control channels over USB (ACL, "PL", PJL)

What the printer half of an M6500-series device will and will not tell a host
over USB, and the wire formats involved. Device under test: Pantum M6507
(`MDL:M6500 series`), USB `232b:0e20`, interface 0 (printer class 07/01/02) →
`/dev/usb/lp0`, owned by the kernel's `usblp`.

The static analysis here is of `$VENDOR_DRIVER/lib/cups/filter/rastertoPantum`,
where `$VENDOR_DRIVER` is an unpacked vendor driver package (1.1.167 at the time
of writing). It is not stripped, and `ptm6500Filter` is **the same inode** — all
17 "model" filters are hardlinks to one executable whose behaviour is selected by
the `*NickName` in the PPD. Addresses below are offsets into that binary, kept so
the tables can be re-derived; `notes/disasm-method.md` has the recipe.

## Summary — which channel gives what

| Want | Channel | Works? |
|---|---|---|
| Model, command set, class | IEEE-1284 Device ID (`LPIOC_GET_DEVICE_ID` on `/dev/usb/lp0`) | yes, and it is the safest thing to ask |
| Device state code | `@PJL INFO STATUS` → `CODE=` | yes |
| State changes, **and toner level** | `@PJL USTATUS DEVICE=ON` → asynchronous `CODE=`/`TONER=` reports | yes — see `notes/error-catalogue.md` §7 |
| Toner level | `@PJL INFO SUPPLIES` and ~40 other names | no, the printer answers `?` |
| Page counter | `@PJL INFO PAGECOUNT` / `VARIABLES` | declared but never populated |
| Anything at all | ACL `queryStatusCmd` (§3, §4.1) | no — write-only, the printer never answers |
| Raw memory / flash | debug ACL `@PJL ENTER LANGUAGE=ACL` (§5) | yes, read-only; see `notes/firmware-map.md` |
| Supplies over SNMP or IPP | — | not applicable over USB, see §6 |

The short version: **the toner level is readable over USB, but only through the
asynchronous PJL report, not through any query.** Everything the vendor's own
filter does in this area is a dead end — it sends a status command and never
reads an answer.

---

## 1. ACL packet structure

The language the filter prints with is switched on by the string `@PJL ENTER LANGUAGE=PL`
(symbol `pjlCmd` @ `0x617400`). Inside it, binary packets of the following form appear:

```
+------+--------+--------+-------------------+----------+
| 0x0B | LEN    | CMD    | payload (LEN-4 B) | CHECKSUM |
+------+--------+--------+-------------------+----------+
   [0]    [1]      [2]        [3 .. LEN-2]      [LEN-1]
```

* `[0] = 0x0B` — packet signature (a constant in every single command, no exceptions).
* `[1] = LEN` — **the total packet length in bytes, including the 0x0B itself and the checksum**.
  Verified on all 8 static blobs and on all 17 constructors.
* `[2] = CMD` — command code.
* `[3..]` — parameters. Multi-byte numbers are **little-endian** (see `GetJobSpecCmd_Z`,
  `GetSysTimeCmd`, `GetImageDataChenksumCmd`).
* `[LEN-1] = CHECKSUM` — not a "magic byte" but an actual checksum.

### Checksum — `CalcChecksum` @ `0x406ad0`

```c
uint8_t CalcChecksum(const uint8_t *p, int n) {
    uint8_t acc = 0;
    for (int i = 0; i < n; i++) acc += p[i];
    return (uint8_t)(-acc);          //  neg %al
}
```
That is, **the sum of every byte in the packet, including the checksum byte itself, ≡ 0 (mod 256)**.
It is computed over the first `LEN-1` bytes.

Verification on `queryStatusCmd`: `0x0B + 0x05 + 0x31 + 0x80 = 0xC1`, `-0xC1 & 0xFF = 0x3F`.
**The trailing byte `0x3f` is the checksum, not a tag and not a terminator.**

### The `00 00` separator between packets

The function `Insert00()` @ `0x406890` is called before/after every ACL command, and
for all models **except** the BP2400/BP2408/BP2460/BM2400/BM2408/BM2460 family (and their
`NW`/`A`/`ANW` variants) writes two zero bytes `00 00` into the stream.
M6500/M6507 is not in the exclusion list → **the separator is written**.
The actual on-the-wire stream looks like this:

```
1b 25 2d 31 32 33 34 35 58            UEL
40 50 4a 4c 0d 0a                      "@PJL\r\n"                (PJLheader)
40 50 4a 4c 20 4c 41 4e 47 ...         "@PJL LANGUAGEVERSION=1.0\r\n"
40 50 4a 4c 20 4a 4f 42 54 49 ...      "@PJL JOBTIME=...\r\n"
40 50 4a 4c 20 4a 4f 42 4f 57 ...      "@PJL JOBOWNER=...\r\n"
40 50 4a 4c 20 45 4e 54 45 52 ...      "@PJL ENTER LANGUAGE=PL\r\n"   (pjlCmd)
00 00  0b 05 1f xx cc                  Insert00 + FineMode
00 00  0b 09 11 ...                    Insert00 + JobSpec
...
00 00  0b 05 31 80 3f                  Insert00 + queryStatusCmd   <-- Prnt_EndDoc
00 00  0b 04 17 da                     Insert00 + resetCmd
1b 25 2d 31 32 33 34 35 58 40 50 4a 4c 0d 0a
40 50 4a 4c 20 45 4f 4a 0d 0a
1b 25 2d 31 32 33 34 35 58 0d 0a       endOfJobCmd (UEL + @PJL EOJ + UEL)
```

All output goes through `SendDataToFile()` @ `0x4066f0` — an ordinary `fwrite(stdout)`
(plus a copy into temp files for collate/manual duplex). **The filter never reads from
the device at all**: the only `read()` calls in the binary are on
`gCollateFile` / `gManualDuplexOddFile` / `gManualDuplexEvenFile`, i.e. its own
temp files. `cupsSideChannelDoRequest` is resolved via `dlsym`, but **is never called**.

---

## 2. Full table of ACL commands

### 2.1 Static blobs in `.data`

| Symbol | vaddr | Bytes | LEN | CMD | Meaning |
|---|---|---|---|---|---|
| `abortCmd`          | 0x6173a0 | `0b 04 16 db`       | 4 | 0x16 | job cancel |
| `resetCmd`          | 0x6173e5 | `0b 04 17 da`       | 4 | 0x17 | reset job state (sent at the end of every job) |
| `formFeedCmd`       | 0x6173f0 | `0b 04 18 d9`       | 4 | 0x18 | eject page (`Prnt_EndPage`) |
| `manualDuplexCmd_Z` | 0x6173a5 | `0b 04 1b d6`       | 4 | 0x1b | manual duplex (ZJS branch) |
| `manualDuplexCmd_C` | 0x6173aa | `0b 04 30 c1`       | 4 | 0x30 | manual duplex (C branch) |
| `machineDuplexCmd`  | 0x6173af | `0b 04 40 b1`       | 4 | 0x40 | hardware duplex |
| `hostUseCmd`        | 0x6173f5 | `0b 04 32 bf`       | 4 | 0x32 | "host use" — sent in `Prnt_StartPage` |
| **`queryStatusCmd`**| 0x6173ea | **`0b 05 31 80 3f`**| 5 | **0x31** | status query, parameter `0x80`; sent in `Prnt_EndDoc` |

Non-ACL strings nearby (for completeness): `endOfJobCmd` @0x6173c0 = `UEL "@PJL\r\n@PJL EOJ\r\n" UEL "\r\n"`,
`pjlCmd` @0x617400 = `"@PJL ENTER LANGUAGE=PL\r\n"`, `UEL_CMD` @0x617498 = `\x1b%-12345X`,
`UEL_CMD_C` @0x617488 = `\x1b\xff-12345X` (note — the second byte is `0xff`, not `%`),
`PJLheader` @0x617460, `pjlVersion` @0x617440 = `"@PJL LANGUAGEVERSION=1.0\r\n"`,
`pjlPreSendTime`/`pjlPreComputerName` = `"@PJL JOBTIME="` / `"@PJL JOBOWNER="`,
`fwVer` @0x617470 = `"versions=3.1.2.1"`, `pjlJZJZ_P2400` @0x6174a2 = `"JZJZ"`,
`CMD_PJL_SKIP_BLANK_PAGES_ENABLE/DISABLE` = `"@PJL SET BLANKSUPPRESS=ENABLE|DISABLE\n"`.

### 2.2 Commands assembled at runtime

| Function | vaddr | LEN | CMD | Payload |
|---|---|---|---|---|
| `GetRasterImageCmd`        | 0x407660 | 0x12 | 0x0c | raster block geometry (15 bytes) |
| `GetJobSpecCmd_Z`          | 0x406f50 | 0x09 | 0x11 | `[3]` job bit flags (0x80/0x08/0x04/0x02/0x01), `[4..7]` copy count LE32 |
| `GetJobSpecCmd_C`          | 0x406ff0 | 0x06 | 0x11 | `[3]` = byte from settings, `[4]` same flags |
| `GetPageSpecCmd`           | 0x407510 | 0x12 | 0x12 | page parameters (15 bytes) |
| `GetCustomPaperCmd`        | 0x4075b0 | 0x0c | 0x13 | custom paper size (8 bytes), zeros otherwise |
| `GetDensityCmd`            | 0x4070d0 | 0x05 | 0x15 | `settings->TonerDensityLevel` (offset 0x40) |
| `GetTonerSaveCmd`          | 0x407070 | 0x05 | 0x19 | `settings->TonerSaveMode` (offset 0x3c) |
| `GetCoverageCmd_Z`         | 0x407830 | 0x07 | 0x19 | toner coverage: `gImageCoverage/8/1000`, three fields of 6/6/8 bits |
| `GetImageDataChenksumCmd`  | 0x4076f0 | 0x06 | 0x1a | `gImageChecksum` LE16 |
| `OutputJbigkitCompression` | 0x40b297 | 0x30 | 0x1a | JBIG block header (48 bytes) |
| `GetSleepCmd`              | 0x406f20 | 0x05 | 0x1d | constant `0x01` |
| `GetCoverageCmd_C`         | 0x407730 | 0x06 | 0x1e | coverage (float computation from dpi/paper type), LE16 |
| `GetTonerParamCmdForP3000` | 0x407100 | 0x24 | 0x1e | 33 bytes of constants (`1e 24 26 2f 50 06 20 20 …`) |
| `GetTonerParamCmdForM5300` | 0x4071b0 | 0x28 | 0x1e | 37 bytes of constants; **see the note below** |
| `GetFineModeCmd`           | 0x4070a0 | 0x05 | 0x1f | `settings[0x29]` — Fine mode |
| `GetSysTimeCmd`            | 0x407480 | 0x08 | 0x1f | year LE16 (`tm_year+1900`), month (`tm_mon+1`), day |
| `queryStatusCmd` (inline)  | —        | 0x05 | 0x31 | parameter `0x80` |

Notes:

* **CMD 0x1f is overloaded**: `GetFineModeCmd` (LEN 5) and `GetSysTimeCmd` (LEN 8) use
  the same code. I.e. the interpretation depends on the packet length.
* **CMD 0x19 is overloaded** the same way: `GetTonerSaveCmd` (LEN 5) and `GetCoverageCmd_Z` (LEN 7).
* **CMD 0x1e is overloaded**: `GetCoverageCmd_C` (LEN 6) and both `GetTonerParamCmd*` (LEN 0x24/0x28).
* **Bug in `GetTonerParamCmdForM5300`**: byte `[1]` is written with length `0x28` (40), but
  `CalcChecksum` is computed over `0x23` (35) bytes, and the result is placed at `[0x27]` (39).
  Bytes `[0x22..0x26]` are left uninitialized in the process (only `[0..15]` and `[0x20..0x27]` are zeroed).
  The command is only sent for `NickName == "Pantum M5000-M6000 Series"`, and has nothing to do with M6500.
* **`GetTonerParamCmdForPlatformM`** @0x407260 — **not an ACL packet**: there is no `0x0b`,
  it is an 88-byte (`0x58`) parameter structure with **big-endian** 32/16-bit fields,
  sent whole in the `pjlJZJZ_P2400` branch.
* `SetCmdType` / `SetJBIGCmdAndParam` / `SetCmdParam` (0x407980/0x407a10/0x407b40) —
  also **not ACL**: these are big-endian ZjStream/JBIG header structures, without `0x0b`.

### 2.3 Have all `*Cmd` been found

`nm rastertoPantum | grep -i cmd` gives exactly this set of symbols; there are no more
strings ending in `Cmd` in `.rodata` (`GetTonerParamCmdForPlatformM`, `GetTonerParamCmdForP3000`,
`GetTonerParamCmdForM5300`, `GetTonerSaveCmd`, `queryStatusCmd` are debug strings for logs).
No command codes outside the table above occur anywhere in the binary.

---

## 3. How the response to `queryStatusCmd` is parsed — **it is not**

This is the main negative result of the static analysis.

* `Prnt_EndDoc` @0x40a6e0 does exactly:
  `Insert00()` → `SendDataToFile(queryStatusCmd, strlen(queryStatusCmd)=5, 1)` →
  `Insert00()` → `resetCmd` → `endOfJobCmd`. The return value of `SendDataToFile` is
  only a flag for "did the write succeed" — no response is requested.
* There are three `read()` calls in the whole binary — all three on temp-file
  descriptors (`gCollateFile`, `gManualDuplexOddFile`, `gManualDuplexEvenFile`) in `main()`.
* `cupsSideChannelDoRequest` (the standard way for a CUPS filter to talk to the backend
  in both directions) is resolved in `InitCUPSDLL`, but **is never called**.
* There is no code and no strings anywhere in the filter that parse a toner level,
  a page counter, or error flags. The strings `TonerSaveMode`, `TonerDensityLevel`,
  `TonerMode`, `TONER_PAH` are **print settings** (toner saving, density), not telemetry.

Conclusion: `queryStatusCmd` in this driver is "fire and forget". There is nothing to
reconstruct the response format from, because no client-side parser exists.

---

## 4. Verification on a live device

All commands were sent to `/dev/usb/lp0` (`sg lp -c 'python3 …'`),
reception used `O_NONBLOCK` + `select()`.

### 4.1 ACL `queryStatusCmd` — no response

| What was sent | Response |
|---|---|
| `0b 05 31 80 3f` (bare command), waited 3 s | empty |
| same, waited 10 s | empty |
| `UEL` + `0b 05 31 80 3f` + `UEL` | empty |
| `UEL "@PJL\r\n"` + `"@PJL ENTER LANGUAGE=PL\r\n"` + `00 00` + `0b 05 31 80 3f` (full emulation of the filter's session) | empty |

No side effects: no page was ejected, the printer stayed at `CODE=10002`,
subsequent PJL requests were answered normally.

### 4.2 PJL — works, but does not report toner

Request format: `\x1b%-12345X@PJL <request>\r\n\x1b%-12345X`.
**Verbatim device responses** (`\x0c` = form feed at the end of every response):

```
@PJL INFO ID
@PJL INFO ID 
M6500 series
```

```
@PJL INFO STATUS
@PJL INFO STATUS 
CODE=10002
```

```
@PJL INFO PAGECOUNT
@PJL INFO PAGECOUNT 

```
(body empty — counter not populated)

```
@PJL INFO CONFIG
@PJL INFO CONFIG 
PAPER [ 16 ENUMERATED ]
	Letter
	Legal
	Executive
	Custom 8.5x13
	A4
	A5
	B5
	Envelope 10
	Envelope Monarch
	Envelope C5
	Envelope DL
	Envelope B5
	A6
	Jap PC
	Dbl Jap PC
	Custom 16K 195
LANGUAGE [ 7 ENUMERATED ]
	icefile
	URF
	PJL
	ZJS
	scan
	CMD
	ACL
MEMORY  = 268435456
USTATUS [ 4 ENUMERATED ]
	DEVICE 
	JOB 
	PAGE 
	TIMED 
```

```
@PJL INFO VARIABLES
@PJL INFO VARIABLES 
PAGECOUNT = Bad Value [ 2 RANGE ]
	0
	0
TIMEOUT = 60 [ 2 RANGE ]
	30
	300
JAMRECOVERY = ON [ 3 ENUMERATED ]
	OFF
	ON
	AUTO
```

```
@PJL INFO USTATUS
@PJL INFO USTATUS 
DEVICE = OFF  [ 3 ENUMERATED ]
	ON 
	OFF 
	VERBOSE 
JOB = OFF  [ 2 ENUMERATED ]
	ON 
	OFF 
PAGE = OFF  [ 2 ENUMERATED ]
	ON 
	OFF 
TIMED = 0 [ 2 RANGE ]
	5
	300
```

```
@PJL INFO MEMORY
@PJL INFO MEMORY  = 268435456
```

```
@PJL INQUIRE PAGECOUNT
@PJL INQUIRE PAGECOUNT =

```
(empty)

```
@PJL DINQUIRE PAGECOUNT
@PJL DINQUIRE PAGECOUNT =
0
```
(this is the variable's *default* value, not a live counter reading)

```
@PJL INQUIRE TIMEOUT
@PJL INQUIRE TIMEOUT =
60
```

**The response `?\r\n`** (= "command not supported") was received for:
`INFO SUPPLIES`, `INFO SUPPLY`, `INFO FILESYS`, `INFO PRODINFO`, `INFO TONER`,
`INFO TONERLEVEL`, `INFO TONERREMAIN`, `INFO TONERSTATUS`, `INFO SUPPLIESSTATUS`,
`INFO SUPPLYINFO`, `INFO CONSUMABLES`, `INFO CONSUMABLE`, `INFO CARTRIDGE`,
`INFO CARTRIDGEINFO`, `INFO DRUM`, `INFO MARKER`, `INFO PANTUM`, `INFO PANTUMINFO`,
`INFO PTINFO`, `INFO EXTINFO`, `INFO DEVICEINFO`, `INFO MACHINEINFO`,
`INFO PRINTERINFO`, `INFO DEVICE`, `INFO FIRMWARE`, `INFO VERSION`,
`INFO SERIALNUMBER`, `INFO LOG`, `INFO COUNTER`, `INFO PAGES`, `INFO PRINTPAGES`,
`INFO TOTALPAGE`, `INFO TOTALPAGES`, `INFO LIFECOUNT`, `INFO MAINTENANCE`,
`INFO ENGINE`, `INFO NVRAM`, `INFO USAGE`, `INFO STATISTICS`, `INFO TRAYS`,
`INFO PAPER`, `INFO LANGUAGE`, `INFO PRINTINFO`, `INFO POWER`.

So the supported PJL INFO set is: **ID, STATUS, CONFIG, VARIABLES, USTATUS,
MEMORY, PAGECOUNT** (the last one is empty). There is no supplies object.

### 4.3 The query set is not the whole PJL surface

The sweep above only covers queries. `@PJL USTATUS DEVICE=ON` subscribes to
**asynchronous** reports instead, and that is the one channel on this firmware
that does carry a toner level. It is documented, with the codes it produces, in
`notes/error-catalogue.md` §7 — do not conclude from the `?` answers here that
the device has no supplies data.

### 4.4 Deliberately not probed

* Any ACL command code not present in the vendor filter. Brute-forcing `CMD`
  risks hitting a write or erase command.
* Debug-channel ACL sub-opcodes other than the verified read-memory one (§5),
  for the same reason.
* The vendor's firmware-upgrade filter was never run.

---

## 5. The debug channel `@PJL ENTER LANGUAGE=ACL` — it works

This is a **different** ACL, not the one from §1. It is described in `olivluca/pantum_dump`
(https://github.com/olivluca/pantum_dump, tested on the M6500W) and is a holdover
from HP/Zenographics ZjStream — the same string `@PJL ENTER LANGUAGE=ACL`
is used in `foo2zjs/arm2hpdl.c` to upload firmware to the HP LJ 1000/1018/1020.

Protocol (verified read-only on an M6507):

```
→  1b 25 2d 31 32 33 34 35 58 40 50 4a 4c 20 45 4e 54 45 52 20 4c 41 4e 47 55 41 47 45 3d 41 43 4c 0d 0a
       UEL + "@PJL ENTER LANGUAGE=ACL\r\n"
→  00 ac 00 09 AA AA AA AA LL LL LL LL 00 00 00 00
       opcode 0x00AC, sub-opcode 0x0009 = read memory,
       AAAA = address BE32, LLLL = length BE32
→  1b 25 2d 31 32 33 34 35 58        UEL = "execute"
←  16-byte header + LL bytes of data
```

Format of the 16-byte response header (derived experimentally, three lengths and two addresses):

| offset | size | value |
|---|---|---|
| 0..1 | BE16 | `0x00AC` — opcode echo |
| 2..3 | BE16 | `0x0009` — sub-opcode echo |
| 4..5 | BE16 | `0x0001` — status (success) |
| 6..9 | BE32 | length of the data that follows |
| 10..15 | — | zeros |

Examples (real responses):
```
len=64   →  00 ac 00 09 00 01 00 00 00 40 00 00 00 00 00 00
len=256  →  00 ac 00 09 00 01 00 00 01 00 00 00 00 00 00 00
len=512  →  00 ac 00 09 00 01 00 00 02 00 00 00 00 00 00 00
len=4096 →  00 ac 00 09 00 01 00 00 10 00 00 00 00 00 00 00
```

Regions checked:

* `0x00000000` — RAM, start: ARM vectors `ea 22 26 36` (`b` instructions, little-endian).
* `0xf6000000` — SPI flash, start of the firmware:
  ```
  18 f0 9f e5 x8   (ldr pc,[pc,#0x18] — ARM reset vector)
  ...
  "TIME=Tue Dec 05 10:05:42 2023;CHGLST="
  ... "Unexpected FIQ" / "Undefined exception." / "SW..."
  ```
  I.e. this is ARM firmware from 2023-12-05. Read speed ≈ 4 KiB in 2.7 s
  (a full 18 MiB SPI dump ≈ 3.5 hours, 128 MiB of RAM — a day; a byte-by-byte dump
  is impractical without optimizing the block size).

This channel is what makes `notes/firmware-map.md` possible: it is the only way
to see the device's own catalogue of states, events and panel texts, none of
which is in the vendor driver.

It is *not* needed for the toner level — the asynchronous PJL report gives that
directly (`notes/error-catalogue.md` §7). The stored supply counter presumably
lives in NVRAM/SPI (the vendor filter sends a coverage command with every page,
so the printer meters toner from coverage itself), but its address cannot be
recovered from the filter, which knows nothing about it; locating it would take
a dump-and-diff across printed pages, or work on the firmware itself.

**Do not brute-force other `0x00AC` sub-opcodes.** Some of them almost certainly
write or erase flash — the same command is what `foo2zjs` uses to *upload*
firmware to the HP LaserJet 1000/1018/1020.

---

## 6. Routes that do not lead anywhere

Checked so nobody has to check them again.

**The IEEE-1284 Device ID is static identification only.** Read with the
`LPIOC_GET_DEVICE_ID` ioctl on `/dev/usb/lp0` — the standard, read-only
mechanism the CUPS `usb` backend uses, no vendor commands involved
(`proto/pantum_status.py id`). The M6507 answers:

```
MFG:Pantum;MDL:M6500 series;CMD:ACL,CMD,scan,ZJS,PJL;CID:Pantum M6500 series;CLS:PRINTER;DES:Pantum M6500 series;
```

`CMD` is the useful field: it is what says the device speaks ACL, ZJS and PJL
and that a scan interface exists. There is no `SN:` field and no status fields
of the kind some HP and Epson devices carry (`STT:`, `S1:`), so there is nothing
about consumables here.

**The vendor PPDs declare no supplies.** Across the whole PPD catalogue,
`*APSupplies`, `*cupsSNMPSupplies` and the rest are absent; `*cupsIPPReason`
carries only a platform code and a manual-duplex hint. `TonerMode` is a
print-quality option (toner saving), not a level readout.

**The CUPS side channel has no supplies command.** `cupsSideChannelDoRequest` is
resolved in the vendor filter but never called (§3), and the API it belongs to
only offers state, device ID and drain — there is no "read supplies" request to
make.

**SNMP does not apply.** Networked Pantum models do answer the standard
Printer-MIB (`prtMarkerSuppliesLevel`, `1.3.6.1.2.1.43.11.1.1.9`) plus a vendor
enterprise subtree, but SNMP-over-USB is not a thing; the CUPS side channel's
SNMP requests are implemented by the network backends, not by `usb`.

**IPP-over-USB does not apply either.** `ipp-usb` binds an interface with
`bInterfaceClass 0x07 / SubClass 0x01 / Protocol 0x04`. This model's interface 0
is `07/01/02` — an ordinary bidirectional printer interface — so `ipp-usb` finds
no matching interface and does not start. Networked variants of the same printer
may expose a `0x04` interface; a USB-only unit does not, which also rules out
driverless printing and eSCL scanning on it.

Publicly, nothing describes a USB status protocol for this family: the material
that exists is about scanning on Linux, not about status or toner.

## 7. What ends up being read — the utility

`proto/pantum_status.py` implements everything above. `status` collects the
one-shot queries:

```
sg lp -c 'python3 proto/pantum_status.py status'
```

which on an idle M6507 reports the model, the command languages, `CODE=10002`,
the memory size, the language list, and the fact that the page counter is a
default rather than a reading. `--raw` adds the verbatim PJL responses.

The toner level is not part of that output, because no query returns it. It
arrives only in the asynchronous report, which the `watch` subcommand
subscribes to:

```
sg lp -c 'python3 proto/pantum_status.py watch --interval 1'
```

`watch` unsubscribes on exit — see `notes/error-catalogue.md` §7 for why that
matters and for the codes it produces.

---

## 8. How to reproduce the analysis

```bash
B=$VENDOR_DRIVER/lib/cups/filter/rastertoPantum
export DECOMP_OUT=/tmp/decomp-rastertoPantum.c
export HOME=/tmp/ghome; mkdir -p $HOME
ghidra-analyzeHeadless /tmp/gproj rtp -import $B \
    -scriptPath notes/tools -postScript DecompAll.java -deleteProject
```
(the same recipe and the same `notes/tools/DecompAll.java` as for the SANE backend —
see `notes/disasm-method.md`. The binary is **not stripped**, load base 0x400000,
Ghidra addresses match `nm`/`objdump`. `$VENDOR_DRIVER` is the unpacked vendor
`pantum-driver` package, as described at the top of this document.)

Dump of the command table:
```bash
objdump -s -j .data --start-address=0x617340 --stop-address=0x6174c0 $B
nm -n $B | grep -iE 'cmd$|Cmd'
```
