#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Save the vendor filter's output for every regression case, so the regression
# survives the vendor driver being uninstalled.
#
# Full streams land in dumps/print/reference/ (git-ignored: they are output of
# proprietary software); their SHA-256 sums are what gets committed.
#
# Needs the same tools as the regression itself: python3, ghostscript and
# cups-filters (cupsfilter) in PATH, plus the vendor driver still installed.
#
#   save_vendor_reference.sh /path/to/rastertopantum [workdir]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/dumps/print/reference"
sums="$root/tests/vendor-sha256.txt"

mkdir -p "$out"
PANTUM_KEEP_VENDOR_OUTPUT="$out" "$root/proto/check_against_vendor.sh" "$@"

( cd "$out" && sha256sum ./* | sed 's| \./| |' ) > "$sums"
echo "saved $(wc -l < "$sums") reference streams; sums in ${sums#"$root/"}"
