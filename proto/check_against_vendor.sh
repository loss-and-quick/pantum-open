#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Compare a rastertopantum implementation with the proprietary ptm6500Filter,
# byte for byte, over the full set of 77 regression cases.
#
# The filter under test is an argument, so the same case list covers both the
# C filter and the Python prototype: they take identical CUPS filter arguments
# and proto/rastertopantum.py is executable, so it is passed the same way.
#
#   check_against_vendor.sh build/rastertopantum          [workdir]
#   check_against_vendor.sh proto/rastertopantum.py       [workdir]
#
# Two comparison modes, picked automatically:
#   diff  the vendor filter is present -- run both and cmp the streams;
#   hash  the vendor filter is gone -- compare SHA-256 against the sums
#         recorded in tests/vendor-sha256.txt while it was still around.
#
# Settings:
#   PANTUM_VENDOR_DIR         prefix the vendor driver is installed under
#   PANTUM_VENDOR_PPD         path to "Pantum M6500 Series.ppd"
#   PANTUM_VENDOR_FILTER      path to ptm6500Filter
#   PANTUM_KEEP_VENDOR_OUTPUT directory to save the vendor streams into
#
# Needs: python3, ghostscript and cups-filters (cupsfilter) in PATH, plus the
# filter under test. The Python prototype additionally needs jbigkit
# (pbmtojbg); the C filter links libjbig itself.
set -u

FILTER="${1:-}"
if [ -z "$FILTER" ] || [ ! -x "$FILTER" ]; then
    echo "usage: $0 /path/to/filter [workdir]" >&2
    echo "       the filter must be executable (chmod +x for the .py prototype)" >&2
    exit 2
fi

# Prefix under which the vendor driver is installed. Distributions disagree
# about where that is, so it is a setting, not a guess: PANTUM_VENDOR_DIR
# names it, and the common system prefixes are tried as a convenience.
find_vendor_dir() {
    if [ -n "${PANTUM_VENDOR_DIR:-}" ]; then
        echo "$PANTUM_VENDOR_DIR"
        return
    fi
    for d in /usr /usr/local /opt/pantum; do
        if [ -x "$d/lib/cups/filter/ptm6500Filter" ]; then
            echo "$d"
            return
        fi
    done
    echo ""
}
DRV="$(find_vendor_dir)"
PPD_FILE="${PANTUM_VENDOR_PPD:-$DRV/share/cups/model/Pantum/Pantum M6500 Series.ppd}"
VENDOR="${PANTUM_VENDOR_FILTER:-$DRV/lib/cups/filter/ptm6500Filter}"
WORK="${2:-$(mktemp -d)}"
mkdir -p "$WORK"

HERE="$(cd "$(dirname "$0")" && pwd)"
SUMS="$(cd "$HERE/.." && pwd)/tests/vendor-sha256.txt"

# Without the vendor filter — once the unfree driver is uninstalled — fall back
# to the hashes recorded from it while it was still around.
if [ -x "$VENDOR" ]; then
    MODE=diff
elif [ -r "$SUMS" ]; then
    MODE=hash
    echo "vendor filter absent; comparing against ${SUMS##*/}"
else
    echo "no vendor filter ($VENDOR) and no recorded hashes ($SUMS)" >&2
    exit 2
fi

if [ ! -r "$PPD_FILE" ]; then
    echo "vendor PPD not found: $PPD_FILE (set PANTUM_VENDOR_PPD)" >&2
    exit 2
fi

# Rewrite the PageSize field of a rendered raster.
#
# One entry of the vendor's paper lookup, 421x595 pt, is not in any PPD of
# either driver, and cupsfilter snaps a custom size that close to A5 onto A5
# itself, so no rasterizer here can produce it.  Writing the size into the
# header afterwards produces exactly the raster a PPD declaring that size
# would have, and the filter reads nothing else about the sheet.
#
# PageSize[2] sits 352 bytes into cups_page_header2_t, after the four-byte
# sync word; the expected old value is passed in so that a change in the
# struct is caught here rather than silently comparing the wrong bytes.
set_page_size() {   # set_page_size <raster> <old_w> <old_h> <new_w> <new_h>
    python3 -c 'import struct, sys
path, old, new = sys.argv[1], tuple(map(int, sys.argv[2:4])), tuple(map(int, sys.argv[4:6]))
d = bytearray(open(path, "rb").read())
if bytes(d[:4]) not in (b"RaS2", b"2SaR", b"RaS3", b"3SaR"):
    sys.exit("not a CUPS raster stream")
if struct.unpack_from("<II", d, 356) != old:
    sys.exit("PageSize is not %dx%d, refusing to patch" % old)
struct.pack_into("<II", d, 356, *new)
open(path, "wb").write(d)' "$@"
}

pass=0
fail=0
run_case() {   # run_case <name> <pdf> <options> [cupsfilter-option] [WxH]
    local name="$1" pdf="$2" opts="$3" cfopt="${4:-}" size="${5:-}"
    local pscmd=()
    [ -n "$cfopt" ] && pscmd=(-o "$cfopt")
    cupsfilter -m application/vnd.cups-raster -p "$PPD_FILE" "${pscmd[@]}" \
        "$pdf" > "$WORK/in.raster" 2>/dev/null || {
            echo "  FAIL  $name (cupsfilter failed)"; fail=$((fail + 1)); return; }
    if [ -n "$size" ]; then
        set_page_size "$WORK/in.raster" 420 595 "${size%x*}" "${size#*x}" || {
            echo "  FAIL  $name (cannot set the page size)"
            fail=$((fail + 1)); return; }
    fi
    if [ "$MODE" = diff ]; then
        PPD="$PPD_FILE" "$VENDOR" 1 user title 1 "$opts" \
            < "$WORK/in.raster" > "$WORK/v.bin" 2>/dev/null
        # Keep the vendor stream when asked: once the unfree driver is gone,
        # these are the only reference the regression has left.
        [ -n "${PANTUM_KEEP_VENDOR_OUTPUT:-}" ] && \
            cp "$WORK/v.bin" "$PANTUM_KEEP_VENDOR_OUTPUT/$name.zjs"
    fi
    PPD="$PPD_FILE" "$FILTER" 1 user title 1 "$opts" \
        < "$WORK/in.raster" > "$WORK/m.bin" 2>/dev/null
    if [ "$MODE" = diff ]; then
        if cmp -s "$WORK/v.bin" "$WORK/m.bin"; then
            echo "  ok    $name"; pass=$((pass + 1))
        else
            echo "  FAIL  $name ($(stat -c%s "$WORK/v.bin") vs $(stat -c%s "$WORK/m.bin") bytes)"
            fail=$((fail + 1))
        fi
    else
        got=$(sha256sum < "$WORK/m.bin" | cut -d" " -f1)
        want=$(awk -v n="$name.zjs" "\$2 == n { print \$1 }" "$SUMS")
        if [ -z "$want" ]; then
            echo "  SKIP  $name (no recorded hash)"
        elif [ "$got" = "$want" ]; then
            echo "  ok    $name"; pass=$((pass + 1))
        else
            echo "  FAIL  $name (hash mismatch)"; fail=$((fail + 1))
        fi
    fi
}

# Test documents.
# The regression compares bytes, so the test documents must be identical
# from run to run; a script builds them rather than a PDF library.
python3 "$HERE/make_test_pdfs.py" "$WORK"

echo "content:"
for d in blank square full three four five noise; do run_case "$d" "$WORK/$d.pdf" ""; done

echo "options:"
for o in Density=0 Density=2 Density=4 TonerMode=True NegativePrint=True \
         ImageRotation=True DPI1200=True FeedLongEdge=True Collate=False; do
    run_case "$o" "$WORK/square.pdf" "$o"
done
# Combinations that exercise more than one knob at a time.
run_case "DPI1200+Negative" "$WORK/square.pdf" "DPI1200=True NegativePrint=True"
# Rotation only shows up on real halftones, so it needs the noise page.
run_case "Rotate-noise" "$WORK/noise.pdf" "ImageRotation=True"
run_case "DPI1200+Rotate" "$WORK/noise.pdf" "DPI1200=True ImageRotation=True"
run_case "Rotate+Negative" "$WORK/noise.pdf" "ImageRotation=True NegativePrint=True"
run_case "TonerMode+Density4" "$WORK/square.pdf" "TonerMode=True Density=4"
run_case "Rotate-custom" "$WORK/noise.pdf" "ImageRotation=True" "PageSize=Yougata4"
run_case "Rotate-A5" "$WORK/noise.pdf" "ImageRotation=True" "PageSize=A5"
run_case "1200-custom" "$WORK/noise.pdf" "DPI1200=True" "PageSize=Younaga3"

# cupsMediaType travels in the raster header, so cupsfilter has to see it too.
for o in MediaType=Thick MediaType=Envelope MediaType=Label MediaType=Transparency \
         MediaType=Cardstock; do
    run_case "$o" "$WORK/square.pdf" "$o" "$o"
done
# "Thin" is our keyword for the media the vendor PPD calls "Kleenex"; both
# carry cupsMediaType 263, which is what the device actually reads. The raster
# is rendered against whichever PPD is in use, so ask that one by its own name
# and tell the filter ours.
if grep -q "^\\*MediaType Kleenex/" "$PPD_FILE"; then render_thin=Kleenex; else render_thin=Thin; fi
run_case "MediaType=Thin" "$WORK/square.pdf" "MediaType=Thin" "MediaType=$render_thin"
for o in InputSlot=Auto InputSlot=ManualFeed InputSlot=AutoFeed; do
    run_case "$o" "$WORK/square.pdf" "$o"
done
run_case "Thick+ManualFeed" "$WORK/square.pdf" "InputSlot=ManualFeed" "MediaType=Thick"

echo "manual duplex:"
for d in blank square three four five noise; do
    run_case "md-$d" "$WORK/$d.pdf" "ManualDuplex=True"
done
run_case "md-A5" "$WORK/square.pdf" "ManualDuplex=True" "PageSize=A5"
run_case "md-negative" "$WORK/three.pdf" "ManualDuplex=True NegativePrint=True"
run_case "md-1200" "$WORK/three.pdf" "ManualDuplex=True DPI1200=True"

echo "paper sizes:"
for ps in Letter A4 A5 A6 B5 ISOB5 B6 Executive Statement Folio Oficio Postcard \
          EnvMonarch EnvDL EnvC5 Com10Envelope EnvC6 ZL Legal Big32K Big16K 32K 16K \
          Yougata4 JapanesePostcard Younaga3 Nagagata3 Yougata2; do
    run_case "$ps" "$WORK/square.pdf" "PageSize=$ps" "PageSize=$ps"
done

# A5 turned on its side: DMPAPER 61, video size swapped and the gray page
# turned a quarter clockwise before halftoning.  Rendered as A5 and relabelled
# 421x595 pt, which is the only size the vendor's lookup treats specially.
echo "A5 sideways:"
run_case "A5Rot" "$WORK/square.pdf" "" "PageSize=A5" 421x595
run_case "A5Rot-noise" "$WORK/noise.pdf" "" "PageSize=A5" 421x595
run_case "A5Rot-rotate" "$WORK/noise.pdf" "ImageRotation=True" "PageSize=A5" 421x595
run_case "A5Rot-1200" "$WORK/noise.pdf" "DPI1200=True" "PageSize=A5" 421x595
run_case "A5Rot-negative" "$WORK/noise.pdf" "NegativePrint=True" "PageSize=A5" 421x595
run_case "A5Rot-duplex" "$WORK/square.pdf" "ManualDuplex=True" "PageSize=A5" 421x595

echo
echo "$pass identical, $fail differing"
[ $fail -eq 0 ] && echo "ALL IDENTICAL" || echo "SOME CASES DIFFER"
exit $((fail != 0))
