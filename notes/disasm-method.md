# Reproducing the disassembly

Every address quoted in `notes/` can be re-derived with the recipes here. The
decompiler output itself is never committed — it is a direct derivative of the
vendor's binaries — so this file is what stands in for it.

Commands use `$VENDOR_DRIVER` for the directory an unpacked vendor driver
package sits in. Extract it from the vendor's own archive rather than a
redistributed build: repackagings tend to drop the `opt/pantum` subtree, which
holds the vendor's qpdf build and a copy of `ipp-usb`.

## Ghidra headless (tested with 12.1.2; `ghidra-analyzeHeadless` must be in PATH)
```bash
B=$VENDOR_DRIVER/lib/sane/libsane-pantum6500.so.1.0.24
export DECOMP_OUT=/tmp/decomp.c
export HOME=/tmp/ghome; mkdir -p $HOME
ghidra-analyzeHeadless /tmp/gproj pantum -import $B \
    -scriptPath notes/tools -postScript DecompAll.java -deleteProject
```
The script `notes/tools/DecompAll.java` decompiles every function into the
single file `$DECOMP_OUT` — 129 of them for this library. Keep that output
local; it is not committed here for the reason given above.

**IMPORTANT:** Ghidra loads the image at base `0x100000`, while `nm -D`/`objdump`
give addresses without the base. I.e. `FUN_001117c0` ↔ `0x117c0` in objdump.
Ghidra found the static functions as `FUN_*`; the correspondences:

| Ghidra | address (objdump) | real name (from debug strings) |
|--------|------------------|-------------------------------|
| FUN_0010b9e0 | 0xb9e0  | `initMessage(buf, msg)` |
| FUN_0010dc60 | 0xdc60  | `sendMessageAndReturn(dev, msg)` |
| FUN_0010dd70 | 0xdd70  | `dev_unlock_scan(dev)` |
| FUN_0010dda0 | 0xdda0  | `bHave_enough_space(dev)` |
| FUN_0010c000 | 0xc000  | `list_conf_devices(name)` (attach from pantum6500.conf) |
| FUN_0010db00 | 0xdb00  | `list_one_device(name)` |
| FUN_0010c080 | 0xc080  | `fix_window(dev)` — `scan_mode_to_code` is here |
| FUN_0010cb20 | 0xcb20  | `set_parameters(dev)` |
| FUN_0010cc50 | 0xcc50  | `init_options(dev)` |
| FUN_0010fc70 | 0xfc70  | `image_rescaling(dev, dtype, nbytes, rows, ppr_padded, buf)` |
| FUN_001108d0 | 0x108d0 | body of `handle_scan_data` |
| FUN_001117c0 | 0x117c0 | `reader_process(dev)` — scan thread |
| FUN_0010bb40 / FUN_0010bbe0 | 0xbb40 / 0xbbe0 | building the gamma LUT / `gamma_correction` |
| FUN_0010ba40 / FUN_0010bad0 | 0xba40 / 0xbad0 | source / mode string lists |

## Helpers
```bash
# string by vaddr (.rodata: vaddr == file offset)
dd if=$B bs=1 skip=$((0x1a380)) count=300 status=none | awk 'BEGIN{RS="\0"} NR==1{print}'

# constant tables
objdump -s -j .rodata --start-address=0x1a050 --stop-address=0x1a070 $B
objdump -s -j .data   --start-address=0x222780 --stop-address=0x222a60 $B

# pointers in .data are filled in by relocations:
readelf -rW $B | awk '{a=strtonum("0x"$1); if (a>=0x222a60 && a<0x222ac0) print}'
```

## Key "anchors" — debug strings and their addresses
| vaddr | string |
|-------|--------|
| 0x1a380 | `usb_dev_request` |
| 0x1a2a8 | `%s, com_pantum_sanei_usb_read_bulk` |
| 0x1a208 | `%s, com_pantum_sanei_usb_write_bulk` |
| 0x18c08 | `sendMessageAndReturn: message %d` |
| 0x18c60 | `return message [%d] not equal to org message [%d]` |
| 0x19e48 | `dev_lock_scan` |
| 0x19d50 | `dev_unlock_scan` |
| 0x19e10 | `dev_set_default_setting` |
| 0x19df0 | `dev_get_scan_job_settings` |
| 0x19620 | `dev_check_adfstatus returns status good` |
| 0x196f8 | `Max scan window: top %d, left %d, bottom %d, right %d` |
| 0x19730 | `default scan window: ...` |
| 0x19770 | `scan window set to scanner: ...` |
| 0x18470 | `data type %d, color type %d` |
| 0x1848d | `sendMessage: message %d` |
| 0x18337 | `%s%d.jpeg` (`/tmp/com.pantum.m6500.<N>.jpeg`) |

## Other objects in the package

The same recipe (`DecompAll.java`) also works for the other blobs. Two
adjustments that cost time if you don't know them:

* the Ghidra project directory **must already exist** — otherwise
  `Abort due to Headless analyzer error: Directory not found`;
* it's best to set a separate `HOME` for each run, otherwise parallel runs
  fight over `~/.config/ghidra`.

```bash
P=$VENDOR_DRIVER
W=/tmp/gwork; mkdir -p $W/proj $W/home
HOME=$W/home DECOMP_OUT=$W/ptm6500Filter.c \
  ghidra-analyzeHeadless $W/proj pf \
  -import $P/lib/cups/filter/ptm6500Filter \
  -scriptPath notes/tools -postScript DecompAll.java -deleteProject
```

Results: `ptm6500Filter` — 90 functions, base 0x400000, symbols present;
`libsane-pantum_mfp.so` — 96 functions, a different (old ASCII command)
protocol.

### Large backends (bm4200/bm5200/cm230, 5.3 MB)

There's no need to run them through Ghidra in full: they are **not stripped
and export demangled C++ names**, so the function you need can be pulled out
directly.

```bash
B=$P/lib/sane/libsane-pantum_bm4200.so.1.0.24
nm -C -D -S $B | grep -E 'DevStatusToLLDErr|MapErrorCode|GetADFStatus'
objdump -d -C --start-address=0x7ed10 --stop-address=0x7ee60 $B
```

`switch` jump tables live in `.rodata` as 32-bit **signed offsets relative to
the table's own address** (`lea tbl(%rip),%rax; mov (%rdx,%rax),%eax;
add %rdx,%rax; jmp *%rax`). Target address = `table_base + int32`.

## Device firmware

A separate source, unrelated to the driver package: the image is read
directly from the printer over the ACL channel. The recipe, address map, and
caveats are in `notes/firmware-map.md`; the tool is `proto/acl_read_mem.py`.
