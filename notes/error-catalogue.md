# Pantum M6500 series — error and state catalogue

Every failure mode the device can report, where each one surfaces, and what a
driver can do about it. Three channels carry state, and they do not agree with
one another: the scanner's ASP frames (§1), the printer's PJL interpreter (§7),
and the panel, whose catalogue lives in the firmware and is richer than either
(§6).

Sources are marked: **[blob]** — recovered from proprietary code, whether the
vendor driver or a firmware image, **[hardware]** — confirmed on a live device,
**[?]** — inference that has not been checked.

What each source contributed:

| Source | What it provided |
|---|---|
| `libsane-pantum6500.so` (the M6500 SANE backend) | `status` codes in the ASP frame, their mapping to `SANE_Status`, `sane_strstatus` |
| `libsane-pantum_bm4200.so` (= `bm5200`, ≈ `cm230`) | the full two-level error table for related models: `DevStatusToLLDErr` + `MapErrorCode` |
| `libsane-pantum_mfp.so` | a different, older ASCII-command protocol, unrelated to the M6500 |
| the vendor CUPS filter | **negative result**: no status parsing at all |
| the vendor PPD catalogue (335 files) | **negative result**: `cupsIPPReason` is not used for errors |
| the device's own firmware, read over ACL | the internal catalogue of states, events and panel texts — the only complete one |

The firmware side is mapped in `notes/firmware-map.md`; the tool that reads it
is `proto/acl_read_mem.py`. Dumps are never published, so §6 records structure
and meaning rather than extracts.

---

## 1. M6500 SANE backend

### 1.1 `sane_strstatus` @ 0x107b10 — all 12 values **[blob]**

An ordinary SANE table, no vendor extensions. Below, alongside the meaning of
each entry, is when this backend actually returns it.

| Code | Backend string | When the backend sets it |
|---|---|---|
| 0 | `Success` | normal |
| 1 | `Operation not supported` | not set |
| 2 | `Operation was cancelled` | `sane_cancel`; frame `0x04` with an unknown `status` |
| 3 | `Device busy` | `status`=2 after 5 failed capture attempts (`sleep(2)` between them) |
| 4 | `Invalid argument` | transport failure, message echo mismatch, unknown `status` during capture, `set/get settings` refused |
| 5 | `End of file reached` | normal end of page |
| 6 | `Document feeder jammed` | `status` 6 or 7 |
| 7 | `Document feeder out of documents` | `status` 5; or the ADF check returned "no paper" |
| 8 | `Scanner cover is open` | `status` 8 |
| 9 | `Error during device I/O` | `sanei_usb` errors, connection dropped |
| 10 | `Out of memory` | `calloc`/`realloc`/`bHave_enough_memory` |
| 11 | `Access to resource has been denied` | not set |
| other | `Unknown SANE status code %d` | — |

The default string is formatted into the static buffer `DAT_00322b20`
(0x50 bytes); `sane_strstatus` is **not reentrant**.

### 1.2 The `status` field of the ASP frame (offset 0x10, big-endian) **[blob]**

The backend parses this field in two places, and **the mappings differ in
their default value** — this matters, because the same device code produces a
different `SANE_Status` depending on where it arrived.

`sendMessageAndReturn()` @0xdc60 returns this field as-is; on a transport
error or a message-echo mismatch it returns a synthetic `1`.

| `status` | Meaning | During scanner capture (msg 0x00, `reader_process` @0x117c0) | In frame `0x04` `e_AbortScanJob` |
|---|---|---|---|
| 0 | OK | continue | `SANE_STATUS_CANCELLED` (2) |
| 1 | (synthetic transport-error code) | `SANE_STATUS_INVAL` (4) | `SANE_STATUS_CANCELLED` (2) |
| 2 | scanner busy | retry 5×`sleep(2)`, then `DEVICE_BUSY` (3) | `SANE_STATUS_CANCELLED` (2) |
| 4 | refused / invalid parameters | `SANE_STATUS_INVAL` (4) | `SANE_STATUS_INVAL` (4) |
| 5 | no paper in the ADF | `SANE_STATUS_NO_DOCS` (7) | `SANE_STATUS_NO_DOCS` (7) |
| 6 | jam | `SANE_STATUS_JAMMED` (6) | `SANE_STATUS_JAMMED` (6) |
| 7 | jam | `SANE_STATUS_JAMMED` (6) | `SANE_STATUS_JAMMED` (6) |
| 8 | cover open | `SANE_STATUS_COVER_OPEN` (8) | `SANE_STATUS_COVER_OPEN` (8) |
| other | — | `SANE_STATUS_INVAL` (4) | `SANE_STATUS_CANCELLED` (2) |

This is **the entire** set of values the M6500 backend distinguishes: the
`switch` has exactly the branches 2 / 5 / 6,7 / 8 and `default`. It does not
distinguish codes 3 and 9+ from garbage. The debug string on the way out is
`return because of response type: %d.` — so `SANE_DEBUG_PANTUM6500=4` shows
the original value before it's lost in `default`.

### 1.3 ADF check — message `0x0f` **[blob]**

The response is an ordinary 32-byte frame:

| Field | Meaning |
|---|---|
| `msg` (0x04) | must be 0x0f, otherwise `Return message [%d] not equals to origin message [%d]` and the result is ignored |
| `arg0` (0x08) | `1` → there is paper in the ADF tray; otherwise `return because no paper in feeder.` → `SANE_STATUS_NO_DOCS` |
| `status` (0x10) | `0` → **the device has an ADF**, global flag `has_adf` = 1; **any nonzero value** → `No feeder or other errors.`, `has_adf` = 0 |

That is, in this frame `status` is not an error code but a flag for whether
an ADF is present at all. The protocol does not provide separate codes for
ADF faults: on the M6507 (a flatbed with no ADF) this always comes back
nonzero. The check runs only if the source is the ADF (`dev->source` ∈
{2,3}) and the job hasn't started yet.

### 1.4 Statuses the backend generates itself, without the device **[blob]**

| Condition (from the debug strings in `sane_pantum6500_start`) | Result |
|---|---|
| `!bHave_enough_space(dev) \|\| !bHave_enough_memory(dev)` | `SANE_STATUS_NO_MEM` |
| `dev->scanning == SANE_TRUE` | `CANCELLED` or `GOOD` (depending on the cancel flag) |
| `dev->reading == SANE_TRUE` | `CANCELLED` or `GOOD` |
| no pages queued | `SANE_STATUS_NO_DOCS` |
| resolution < 15 dpi on either axis | `SANE_STATUS_INVAL` (the check is `0xe < dpi` in `reader_process`) |

### 1.5 libusb errors (built-in `sanei_usb`) @ 0x107c30 **[blob]**

These go into the log as text and are surfaced externally as
`SANE_STATUS_IO_ERROR`.

| Code | String |
|---|---|
| 0 | `Success (no error)` |
| −1 | `Input/output error` |
| −2 | `Invalid parameter` |
| −3 | `Access denied (insufficient permissions)` |
| −4 | `No such device (it may have been disconnected)` |
| −5 | `Entity not found` |
| −6 | `Resource busy` |
| −7 | `Operation timed out` |
| −8 | `Overflow` |
| −9 | `Pipe error` |
| −10 | `System call interrupted (perhaps due to signal)` |
| −11 | `Insufficient memory` |
| −12 | `Operation not supported or unimplemented on this platform` |
| −99 | `Other error` |
| other | `Unknown libusb-1.0 error code` |

---

## 2. The extended error model from the BM4200/BM5200/CM230 backends **[blob]**

These backends (5.3 MB, C++, **not stripped** — they export full names)
implement the same ASP protocol, but with a full two-level error mapping.
For the M6500 it **does not apply** (there the mapping is hardcoded as
constants, see §1.2), but this is the most complete protocol documentation
available in the package — and it shows what other `status` values the
device is capable of returning in principle.

Reproduction:

```bash
B=$VENDOR_DRIVER/lib/sane/libsane-pantum_bm4200.so.1.0.24
nm -C -D -S $B | grep -E 'DevStatusToLLDErr|MapErrorCode'
objdump -d -C --start-address=0x7ed10 --stop-address=0x7ee60 $B   # jump table @ .rodata 0x3c4ae0
objdump -s --start-address=0x3bb620 --stop-address=0x3bb650 $B    # byte table for MapErrorCode
```

### 2.1 `CTScanner_A3::DevStatusToLLDErr(unsigned short)` @0x7ed10

Debug string: `CTScanner_A3:: DevErrToAPPErr: iDevErr = %d, iErr = %d`.
Input is the frame's `status` field, output is the driver's internal error
code. The jump table has 24 entries of 4 bytes each in `.rodata` @0x3c4ae0.

| `status` | → error code | result after `MapErrorCode` |
|---|---|---|
| 0 | 2 | IO_ERROR |
| 1 | 11 | INVAL |
| 2 | 5 | DEVICE_BUSY |
| 3 | 31 | UNSUPPORTED |
| 4 | 4 | INVAL |
| 5 | 6 | NO_DOCS |
| 6 | 22 | NO_DOCS (`ERROR_MisPick`) |
| 7 | 7 | JAMMED |
| 8 | 10 | INVAL |
| 9 | 24 | INVAL |
| 10 | 8 | COVER_OPEN |
| 11 | 2 | IO_ERROR |
| 12 | 11 | INVAL |
| 13 | 15 | CANCELLED (`ERROR_Abortscanjob`) |
| 14 | 25 | INVAL |
| 15 | 26 | INVAL |
| 16 | 27 | INVAL |
| 17 | 28 | INVAL |
| 18 | 29 | INVAL |
| 19 | 30 | DEVICE_BUSY |
| 20 | 33 | UNSUPPORTED |
| 21 | 34 | COVER_OPEN |
| 22 | 2 (default) | IO_ERROR |
| 23 | 37 | ACCESS_DENIED |
| >23 | 2 (default) | IO_ERROR |

**The numbering here differs from the M6500** (on the M6500 "no paper" = 5,
"cover" = 8; here "no paper" = 5→NO_DOCS, "cover" = 10). So this is either a
different firmware generation or a different level of abstraction. This
table **cannot** be used directly for the M6500 — it is included as a map of
how many states exist in the family overall.

### 2.2 `MapErrorCode` @0x59230 — internal code → `SANE_Status`

A byte table of 38 entries in `.rodata` @0x3bb620, `default` = 4 (`INVAL`).

| Code | `SANE_Status` | Name (from debug strings, where recoverable) |
|---|---|---|
| 0 | GOOD (0) | `ERROR_None` |
| 1, 2, 3 | IO_ERROR (9) | `ScanDrv_STATUS_UNCONNECT` / `ERROR_NoConnet` |
| 4 | INVAL (4) | `ERROR_ParameterInvalid` / `ERROR_NULL_Point` |
| 5 | DEVICE_BUSY (3) | `ScanDrv_STATUS_DEVICE_BUSY` |
| 6 | NO_DOCS (7) | `ScanDrv_STATUS_NO_DOCS` |
| 7 | JAMMED (6) | jam |
| 8 | COVER_OPEN (8) | cover |
| 9, 10, 11 | INVAL (4) | — |
| 12, 13, 14 | NO_MEM (10) | `ERROR_NoEnoughMemory` / `ERROR_NoEnoughSpace` |
| 15, 16 | CANCELLED (2) | `ERROR_Abortscanjob` |
| 17–19 | INVAL (4) | — |
| 20 | NO_MEM (10) | — |
| 21 | INVAL (4) | — |
| 22 | NO_DOCS (7) | `ERROR_MisPick` |
| 23–29 | INVAL (4) | — |
| 30 | DEVICE_BUSY (3) | — |
| 31 | UNSUPPORTED (1) | — |
| 32 | INVAL (4) | — |
| 33 | UNSUPPORTED (1) | — |
| 34 | COVER_OPEN (8) | — |
| 35 | UNSUPPORTED (1) | — |
| 36 | INVAL (4) | — |
| 37 | ACCESS_DENIED (11) | — |

Other names from the same binaries that appear in debug strings:
`ERROR_IMAGE_INFO`, `ERROR_FOR_EVENTS_IN_WAIT_LIST`,
`ERROR_REUSE_ASYNC_KERNEL`.

---

## 3. The `ptm6500Filter` print filter — negative result **[blob]**

All 17 "model" filters are hard links to a single inode; it has been fully
disassembled (Ghidra, 90 functions, base 0x400000, symbols present).

* **There are no error-code tables in the filter.** The only `ERROR:`
  strings are three fatal CUPS messages:
  `ERROR: job-id user title copies options [file]`,
  `ERROR: Unable to open raster file - `, `ERROR: No pages found!`.
* **Not a single `STATE:` string** — not in `ptm6500Filter`, not in
  `rastertoPantumPCL`, not in `ptps`, not in `prepdftopdf`. That is, the
  filter never reports any printer state to CUPS; `printer-state-reasons`
  never changes because of it.
* `queryStatusCmd` (`0b 05 31 80 3f`) is sent in `Prnt_EndDoc` and **the
  response is never read**; `cupsSideChannelDoRequest` is resolved but never
  called. Details and the hardware check are in `notes/status-protocol.md` §3-4.
* All the static commands next to `abortCmd`/`queryStatusCmd` are
  job-control commands, not error codes:
  `abortCmd 0b 04 16 db`, `resetCmd 0b 04 17 da`, `formFeedCmd 0b 04 18 d9`,
  `hostUseCmd 0b 04 32 bf`, `machineDuplexCmd 0b 04 40 b1`,
  `manualDuplexCmd_Z 0b 04 1b d6`, `manualDuplexCmd_C 0b 04 30 c1`.
* The only "status" the filter is able to show is its own `prntLastError`
  string of the form `Prnt_EndDoc() :: send queryStatusCmd Fail 0 ==  ` —
  this reports a failed **write** to the stream, not a response from the
  printer.

## 4. PPD files — negative result **[blob]**

Across all 335 PPDs in the package, the union of declared `cupsIPPReason`
values is **four** keys, and none of them is an error state:

| Key | Value | What it actually is |
|---|---|---|
| `PlatForm` | `text:PLATFORM_M` or `text:PLATFORM_Z` | selects the code-generation branch in the filter |
| `SupportManualDuplex` | `text:TRUE` | a capability flag |
| `UseRenderer` | — | a capability flag |
| `MDuplex` | localized text in 20 languages | the "flip the stack" instruction, shown during manual duplex |

`*cupsIPPSupplies: False`, `*cupsSNMPSupplies: False`,
`*cupsCommands: "ReportLevels"`. No model in the series has a print error
catalogue in its PPD.

## 5. Localized message catalogues in the package — none **[blob]**

The package contains not a single `.mo`, `.qm`, `.po`, `.xml`, or `.json`
file. `share/doc/pantum/strings.txt` is 8 lines of GUI installer strings.
All localized text lives **in the firmware** (see §6), not in the driver.

---

## 6. Firmware: the internal catalogue of states and events **[blob]**

Source: a RAM dump of the running device (a DRAM dump taken by `proto/dump_device.sh`),
captured by `proto/acl_read_mem.py` over the read-only ACL channel. The
image structure is in `notes/firmware-map.md`.

The state manager is the panel module `mfp_uiapp_status.c` (`notes/firmware-map.md`
§5). Its name table is a `char[196][30]` array at RAM address **0x0108fbc9**,
referenced from code by a literal in the pool @0x00073564 — read that address out
of a DRAM dump to see the pool for yourself.

The pool is shared across several enums; the blocks appear in declaration
order. **The numeric enum values cannot be derived from the pool itself** —
only the order within a block is guaranteed, so specific numbers are marked
`[?]`.

### 6.1 Message classes (severity), pool indices 93-102

`UNUSED`, `MSG_READY`, `MSG_JOB`, `MSG_CONFIRM_PROMPT`, `MSG_TRANSITORY`,
`MSG_ALERT`, `MSG_WARN`, `MSG_NOTIFY`, `MSG_ANNOUNCE`, `MSG_FATAL`.

The panel code distinguishes at least `MSG_ALERT` and `MSG_FATAL` separately
(strings `UI_pwrsave: ==MSG_ALERT===errflag:%d`, `==MSG_FATAL===errflag:%d`).

### 6.2 States (`state`), pool indices 41-88

| # | Name in firmware | Meaning | Panel text (English, §6.4) |
|---|---|---|---|
| 41 | `idle` | idle | — |
| 42 | `ready` | ready | — |
| 43 | `burnflash_programming` | firmware flashing in progress | `Firmware update...` |
| 44 | `burnflash_complete` | firmware flashing complete | `Update done` |
| 45 | `printing` | printing | `Printing...` |
| 46 | `canceling_print` | canceling print | `Canceling...` |
| 47 | `initializing` | initializing | `Initializing...` |
| 48 | `engine_cleaning` | cleaning the engine | — |
| 49 | `engine_cleaning_roller` | cleaning the roller | — |
| 50 | `engine_measuring_toner` | measuring toner | — |
| 51 | `paper_out` | no paper | `Paper empty` |
| 52 | `paper_out_all` | paper out everywhere | `Paper empty` |
| 53 | `manual_duplex` | waiting for the stack to be flipped | `Back side: Start` |
| 54 | `door_open` | cover open | `Close Cover` |
| 55 | `rear_door_open` | rear cover open | `Close Cover` |
| 56 | `paper_jam` | jam | `Paper jam` |
| 57 | `paper_jam_input` | jam at the input | `Feed jam` |
| 58 | `paper_jam_output` | jam at the output | `Middle jam` / `Paper jam` |
| 59 | `toner_mismatch` | incompatible cartridge | `Cartridge Err` |
| 60 | `no_toner` | no cartridge | `No Cartridge` |
| 61 | `scanning` | scanning | `Scanning...` |
| 62 | `calibrating` | calibrating | `Warming up...` |
| 63 | `canceling_scan` | canceling scan | `Canceling...` |
| 64 | `scanner_error` | scanner error | `Scanner Err NN` |
| 65 | `miss_pick` | sheet not picked up | `Feed jam` |
| 66 | `scan_running` | scan in progress | `Scanning...` |
| 67 | `copying` | copying | `Copying...` |
| 68 | `canceling_copy` | canceling copy | `Canceling...` |
| 69 | `copy_user_input` | waiting for panel input | — |
| 70 | `device_busy_copy_later` | busy, copy later | `System busy` |
| 71 | `copy_pending` | copy queued | — |
| 72 | `copy_page_count` | copy counter | `Copies:    /` |
| 73–80 | `connecting`, `searching`, `failed`, `success`, `no_routers`, `canceled`, `oem-status1`, `oem-status2` | Wi-Fi/network states | — |
| 81–88 | `testing`, `link_down`, `link_connecting`, `link_connected`, `link_error`, `sta_enabled`, `sta_disabled`, `Sig_Strength` | connection-link states | — |

The correspondence to panel texts is **[?]** (based on the meaning of the
names, not on code).

### 6.3 Events (`event`), pool indices 103-159 and 181-194

Block 103-159 (declared contiguously, in enum order):

| # | Name | Meaning |
|---|---|---|
| 103 | `warm_up` | warm-up |
| 104 | `scan_next_page` | "place the next sheet" |
| 105 | `ADF_continue_scan` | continue scanning from the ADF |
| 106 | `ADF_paper_out` | ADF out of sheets |
| 107 | `memory_low` | low memory |
| 108 | `toner_low` | toner running low |
| 109 | `toner_life_end` | cartridge life exhausted |
| 110 | `middle_jam_not_clean` | middle jam not cleared |
| 111 | `output_jam_not_clean` | output jam not cleared |
| 112–116 | `middle_jam_1` … `middle_jam_5` | middle jam, sensors 1-5 |
| 117 | `ADF_cover_open` | ADF cover open |
| 118 | `scan_fail` | scan failed |
| 119–125 | `communication_err_21` … `communication_err_27` | communication errors 21-27 |
| 126 | `memory_full` | memory full |
| 127 | `file_aborted` | file transfer aborted |
| 128 | `user_id_password_err` | wrong login/password (scan-to-FTP/SMB/e-mail) |
| 129 | `file_oversize` | file too large |
| 130 | `send_file_failed` | file send failed |
| 131 | `sending_file` | sending in progress |
| 132 | `send_finished` | send complete |
| 133 | `job_finished` | job complete |
| 134–139 | `scanner_error_11` … `scanner_error_16` | scanner errors 11-16 |
| 140–153 | `printer_internal_err_00` … `printer_internal_err_13` | internal printer errors 00-13 |
| 154 | `sleep` | entering sleep |
| 155 | `paper_present` | paper detected |
| 156 | `paper_removed` | paper removed |
| 157 | `cal_graph_end` | end of calibration chart |
| 158 | `paper_present_failure` | paper-present sensor faulty |
| 159 | `scanner_busy` | scanner busy |

Block 181-194 was added later (after the block of internal names
`pt1`/`pv1`…), i.e. these are events with larger enum values:

| # | Name | Meaning |
|---|---|---|
| 181–184 | `net_netdrvr_ip_up`, `net_uapdrvr_ip_up`, `net_netdrvr_ip_down`, `net_uapdrvr_ip_down` | network events |
| 185–188 | `plat_default_country_set/clr`, `plat_password_set/clr` | platform events |
| 189 | `middle_jam_6` | middle jam, sensor 6 |
| 190 | `print_step_too_short` | feed step too short |
| 191 | `email_size_over_server_limit` | email exceeds the server's size limit |
| 192 | `engine_param_err` | engine parameter error |
| 193 | `engine_communicate_err` | no communication with the print engine |
| 194 | `engine_param_sys_err` | engine parameter system error |

### 6.4 Panel texts — a `char[94][32]` array @0x010984c3 (English) **[blob]**

The array lives at `0x010984C3` in a DRAM dump. The other languages sit
in identical arrays (see `notes/firmware-map.md` §4). Slots 31-93:

| Index | Text | Meaning |
|---|---|---|
| 31–38 | `Initializing...`, `Printing...`, `Scanning...`, `Copying...`, `Warming up...`, `Canceling...`, `Firmware update...`, `Update done` | normal states |
| 39 | `Close Cover` | cover open |
| 40 | `No Cartridge` | cartridge not installed |
| 41 | `Paper empty` | no paper |
| 42 | `Feed jam` | jam during feed |
| 43 | `Middle jam` | jam inside |
| 44 | `Paper jam` | jam |
| 45 | `PC busy` | host busy |
| 46 | `System busy` | device busy |
| 47–49 | `Back side: Start`, `Scan next: Start`, `Back side:   OK` | manual-duplex / multi-page-scan prompts |
| 50 | `Scan failed` | scan failed |
| 51 | `Comm. failed` | connection dropped |
| 52–68 | `Printer Err 00` … `Printer Err 16` | internal printer errors |
| 69–74 | `Scanner Err 11` … `Scanner Err 16` | scanner errors |
| 75–81 | `Comm. Err 21` … `Comm. Err 27` | communication errors |
| 82 | `Cartridge Err` | cartridge error |
| 83 | `Toner life end` | toner life exhausted |
| 85–87 | `No Shutdown`, `Restarting...`, `Service call` | service messages |
| 88–93 | `End scan:     OK`, `Cancel:       OK`, `Restart:   Start`, `Copies:    /`, `Zoom:`, `Page:` | key prompts |

Notes:

* The panel numbering **does not match** the event numbering: the panel
  shows `Printer Err 00..16` (17 texts), while the firmware has 14
  `printer_internal_err_*` events (00..13). The panel table is wider than
  the event enum.
* The `Comm. Err 21..27` list **directly confirms** the "communication
  error, status 25" indication observed on the bench M6507 → this is
  `communication_err_25` / `Comm. Err 25`. **[blob + hardware]**
* The panel catalogue has exactly two toner states: `toner_low` (events)
  and `Toner life end` — the panel does not show a percentage level.

### 6.5 PJL code classes **[blob]**

At RAM address 0x0112aa74 there is a constant set of boundaries, used by the
PJL-status classifier function located right before it:

```
1388 = 5000   2710 = 10000   4e20 = 20000
7530 = 30000  9c40 = 40000   c350 = 50000
```

From this, the `CODE=` classes are:

| Range | Class |
|---|---|
| < 10000 | informational |
| 10000–19999 | device state ("ready" and derivatives) |
| 20000–29999 | background media feed |
| 30000–39999 | error with auto-continue |
| 40000–49999 | operator intervention required |

The PJL implementation is a licensed HP-compatible stack (files
`../pjl/pjlinfo.c`, `pjlout.c`, `pjlcmds.c`, `pjlpr.c`, `pjlstart.c`,
`stpjl.c` in the image). Output format: `CODE=%5d` @0x01f737c8 and
`CODE=%u` @0x01f73825. **The "internal state → CODE=" table could not be
found statically** — it does not sit in the image as a constant array, but
is computed. Hence every specific `CODE=` value, except the ones observed on
hardware, is marked `[?]`.

The image also holds the format string of the asynchronous broadcast,
`@PJL USTATUS DEVICE STATUS=%x,CODE=%d,TONER=%d` (@0x01a2763c) — the first
evidence that **the firmware has a toner level and reports it over USTATUS**
even though `INFO SUPPLIES` does not. That prediction was later confirmed on
hardware; §7 has the reports it produces.

---

## 7. PJL state on a live device **[hardware]**

Captured with `proto/pantum_status.py` (`status` and `watch`); the same commands
reproduce all of it on any unit of the family.

| Query | Response | Comment |
|---|---|---|
| `@PJL INFO STATUS` | `CODE=10002` | normal, the device is idle |
| `@PJL INFO ID` | `M6500 series` | |
| `@PJL INFO CONFIG` | `PAPER [16]`, `LANGUAGE [7]` = icefile, URF, PJL, ZJS, scan, CMD, ACL; `MEMORY = 268435456`; `USTATUS [4]` = DEVICE, JOB, PAGE, TIMED | |
| `@PJL INFO VARIABLES` | `PAGECOUNT = Bad Value [0..0]`, `TIMEOUT = 60 [30..300]`, `JAMRECOVERY = ON [OFF/ON/AUTO]` | `JAMRECOVERY` is the only variable related to errors |
| `@PJL INFO USTATUS` | `DEVICE = OFF [ON/OFF/VERBOSE]`, `JOB = OFF`, `PAGE = OFF`, `TIMED = 0 [5..300]` | broadcast is disabled |
| `@PJL INFO PAGECOUNT` | empty body | |
| `@PJL INFO SUPPLIES` / `FILESYS` / `PRODINFO` | `?` | not supported |
| `@PJL DINQUIRE PAGECOUNT` | `0` | this is the variable's *default*, not the counter |
| `@PJL ECHO <text>` | echo | works as a "ping" for the PJL interpreter |

### Observed `CODE=` values

| Condition | `CODE=` | Source |
|---|---|---|
| idle | `10002` | [hardware] |
| during a flatbed scan (interface 1 busy with a scan job) | `10002` — **does not change** | [hardware] |
| PJL request during an active scan | sometimes there is no response at all (timeout) — this is not an error, just the interpreter being busy | [hardware] |

Transport limitation: `/dev/usb/lp0` can be opened by **exactly one**
process (the kernel's `usblp`); a second one gets `EBUSY`. So status cannot
be polled through the same node during printing — only before and after the
job.

### Values captured from the device via `@PJL USTATUS` **[hardware]**

Asynchronous notifications are enabled with `@PJL USTATUS DEVICE=ON`; while
idle the device stays silent, and a report only arrives on a state change.
Each report looks like this:

```
@PJL USTATUS DEVICE
CODE=40021
TONER=44
```

| Code | Meaning | How confirmed |
|---|---|---|
| `10001` | ready, online | arrives at the start of a job and after closing the cover |
| `10002` | idle | response to `@PJL INFO STATUS` at rest |
| `10003` | warm-up | after closing the cover and after installing the cartridge |
| `10005` | processing a job | alternates with `10001` during printing |
| `40021` | **front cover open** | the operator opened and closed the cover, the code appeared and went away |
| `40600` | **cartridge removed / not installed** | arrives together with `TONER=0` |
| `42000` | **jam in the feed area** | the panel showed "feed jam"; corresponds to the `paper_jam_input` state from §6.2 |

**The toner level is available here and nowhere else.** Every `USTATUS DEVICE`
report carries `TONER=` as a percentage. No `@PJL INFO` query returns it — see
the sweep in `notes/status-protocol.md` §4.2 — so a driver that only asks
questions will conclude, wrongly, that the device has no supplies data.

`TONER=` shows not the remaining amount "overall", but what's visible in
the **installed** cartridge: without a cartridge it honestly reports
`TONER=0`, after installing one — `TONER=44`. The value does not change
from page to page, including the report at the moment of a jam, meaning
it's read from NVRAM rather than recomputed on the fly.

Unsubscribing is `@PJL USTATUSOFF`. Important: a process killed by a signal
leaves the subscription active, and the printer keeps sending reports;
`proto/pantum_status.py` therefore turns SIGTERM into a normal exit.

### An empty tray has no code of its own **[hardware]**

Verified by direct experiment: all paper was removed from the tray, and a
job was sent. The printer tried to pick up a sheet, didn't find one, and
stopped — the panel showed **"feed jam"**, and PJL reported `CODE=42000`,
the exact same code as for a real jam.

In other words, there is no separate "out of paper" on the M6507: the
device only reports that the feed failed. Telling an empty tray apart from
a stuck sheet by the code is impossible — this is consistent with the
`miss_pick` state name in the firmware.

Practical consequence for the driver: on `42000` you cannot tell the user
"remove the jammed paper", because most likely there is none. The correct
wording is "the printer failed to pick up a sheet: check the paper in the
tray".

Two details that will crash a naive status poller:

* while the device is waiting for paper it **stops accepting writes** — every
  send fails with `EAGAIN`. Status cannot be polled in that state, only read:
  the `USTATUS` subscription keeps working and queues its reports by itself.
* for the same reason `USTATUSOFF` does not get through either. Unsubscribing
  has to be retried, or the printer stays subscribed after the program exits.

### What does not surface in the PJL branch **[hardware, negative result]**

Checked manually by an operator with the watcher running: **the tray
(removed and reinserted), no paper in the tray, the rear cover, and the ADF
cover do not change `CODE=` at all** — neither in `@PJL INFO STATUS`
polling nor in the asynchronous reports. Only the front cover and the
cartridge are reflected.

This follows from how the firmware is built: it keeps printer, scanner and
copier statuses separate and only combines them for the panel
(`notes/firmware-map.md` §5), while PJL surfaces the print branch alone.
Judging by the
state names in §6.2 (`miss_pick`), the absence of paper is detected only
when a feed is attempted, not at rest — meaning this code can only be
captured by starting a print job with an empty tray.

### `CODE=` values not yet captured

| Code | Meaning | Status |
|---|---|---|
| `10006` | low toner | [?] needs an almost-empty cartridge |
| `10023` | printing | [?] `10005` may be sent instead |
| `40000` | sleep | [?] needs waiting for the idle timeout |
| other `4xxxx`/`5xxxx` | other jams, fuser and scanner errors | [?] not staged deliberately |

## 8. Filling in the rest of the catalogue

Most `CODE=` values in §6 are still `[?]`: the firmware computes them rather
than holding a table, so they can only be observed. If you have one of these
machines, the two sections below are how to add to the list.

### 8.1 Capturing codes by hand

`proto/pantum_status.py watch` polls `@PJL INFO STATUS`, subscribes to
`@PJL USTATUS DEVICE` and prints only what changes. It sends nothing else to
the device, and unsubscribes when it exits:

```bash
sg lp -c 'python3 proto/pantum_status.py watch --interval 1 --log codes.txt'
```

With it running, provoke one state at a time, returning the device to rest
between steps and giving it five to ten seconds to react: open and close the
front cover; open and close the rear cover; remove and refit the cartridge;
remove and refit the paper tray; empty the tray, send a job, then refill it;
open and close the ADF cover if the unit has one; let the device sleep
(`TIMEOUT` defaults to 60 s) and wake it; press Cancel on the panel while idle.

For comparison, the states that produced nothing on the bench M6507 are listed
above under "What does not surface in the PJL branch" — the tray, the rear
cover and the ADF cover are silent, which is a property of the firmware and not
of the capture.

**Do not stage a jam.** It risks the machine and is unnecessary; the codes are
identical to the ones an empty tray produces. If a jam happens on its own, run
the watcher before clearing it — that is the safe way to record `paper_jam*`.

### 8.2 Not attempted, and why

* Anything that prints consumes paper and toner, so the `printing`,
  manual-duplex and "tray missing" codes were left uncaptured.
* ACL sub-opcodes other than the verified read-memory `0x0009`: this family
  has write and erase behind the same opcode (`notes/status-protocol.md` §5).
* The vendor's firmware-upgrade filter was never run.

### 8.3 Open questions for static analysis

* **The "internal state → PJL `CODE=`" bridge.** The range classifier (§6.5)
  and the state manager (§6.2) are both located; what joins them is not. It
  would mean loading a DRAM dump into Ghidra as raw ARM:LE:32:v7 at base 0 and
  disassembling the functions that reference `CODE=%5d` (@0x01f737c8, reachable
  from the literal pools @0x00d2ecc4 and @0x00d2ef78).
* **Numeric values of the `state`/`event` enums.** The name pool gives order
  only. Either cross-references from the code, or differential RAM snapshots
  taken in different states — `notes/firmware-map.md` §8 records how far the
  differential approach got and where it stalled.
* **Where the toner counter is stored.** The reported value is solved (§7);
  the stored one is not. The consumables `OID_*` names suggest an internal
  object model reachable from the embedded web server, which nothing here has
  examined — see `notes/firmware-map.md` §8.
