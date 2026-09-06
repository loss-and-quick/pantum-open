#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Take a complete, read-only snapshot of the printer: identity, PJL state,
# SPI flash, NVRAM and the running DRAM image.
#
# Everything here only reads. The ACL memory-read sub-opcode (0x0009) is the
# single command sent to the debug channel; nothing is written, erased or
# reconfigured. Even so, run this on a printer you own and can power-cycle.
#
# WARNING about the output: the flash tail holds the device's TLS private key,
# its certificate, the SNMP community and the web password. Treat the dumps as
# secrets — do not commit them, do not attach them to bug reports. Only their
# hashes and derived text tables belong in git.
#
# Needs: python3, and the device node readable (it belongs to group "lp").
#
#   sg lp -c 'proto/dump_device.sh'
#   sg lp -c 'proto/dump_device.sh --quick'     # skip the slow DRAM region
#
# If python3 is not in PATH, point PYTHON= at one.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
dev="${PANTUM_DEV:-/dev/usb/lp0}"
python="${PYTHON:-python3}"
quick=0
[ "${1:-}" = "--quick" ] && quick=1

if ! command -v "$python" >/dev/null; then
    echo "python3 not found; set PYTHON= or run inside a shell that has it" >&2
    exit 2
fi
if [ ! -w "$dev" ]; then
    echo "$dev is not writable: run through 'sg lp -c ...' or set PANTUM_DEV" >&2
    exit 2
fi

# Directory per run: a snapshot is only meaningful as a set taken at one time.
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
out="$root/dumps/firmware/$stamp"
mkdir -p "$out"
echo "writing to dumps/firmware/$stamp"

step() { printf '\n== %s\n' "$1"; }

step "USB descriptors"
for d in /sys/bus/usb/devices/*/; do
    [ "$(cat "$d/idVendor" 2>/dev/null)" = "232b" ] || continue
    {
        echo "path: $d"
        for f in idVendor idProduct manufacturer product serial bNumInterfaces bcdDevice; do
            printf '%-16s %s\n' "$f" "$(cat "$d/$f" 2>/dev/null)"
        done
        for i in "$d"*:*/; do
            printf 'interface %-8s class/sub/proto %s/%s/%s driver %s\n' \
                "$(basename "$i")" \
                "$(cat "$i/bInterfaceClass" 2>/dev/null)" \
                "$(cat "$i/bInterfaceSubClass" 2>/dev/null)" \
                "$(cat "$i/bInterfaceProtocol" 2>/dev/null)" \
                "$(basename "$(readlink "$i/driver" 2>/dev/null)" 2>/dev/null)"
        done
    } > "$out/usb-descriptors.txt"
    break
done
sed -n '2,4p' "$out/usb-descriptors.txt" 2>/dev/null

step "IEEE 1284 device id"
"$python" "$root/proto/pantum_status.py" -d "$dev" id > "$out/device-id.txt" 2>&1 || true
cat "$out/device-id.txt"

step "PJL information"
"$python" "$root/proto/pantum_status.py" -d "$dev" info > "$out/pjl-info.txt" 2>&1 || true

step "SPI flash 0xF6000000, 16 MiB (about 40 s)"
"$python" "$root/proto/acl_read_mem.py" 0xF6000000 0x1000000 "$out/flash-f6000000.bin"

step "NVRAM 0xF6FA0000, 256 KiB (separate copy, for diffing across runs)"
"$python" "$root/proto/acl_read_mem.py" 0xF6FA0000 0x40000 "$out/nvram-f6fa0000.bin"

if [ "$quick" = 0 ]; then
    step "DRAM 0x00000000, 64 MiB (about 2.5 min)"
    "$python" "$root/proto/acl_read_mem.py" 0x00000000 0x4000000 "$out/ram-00000000.bin"
else
    echo "  (skipped: --quick)"
fi

step "hashes and compression"
( cd "$out" && sha256sum ./*.bin > sha256.txt && gzip -9 ./*.bin )

{
    echo "Pantum device snapshot, $stamp"
    echo
    echo "Device id: $(tr -d '\0' < "$out/device-id.txt" | head -1)"
    echo
    echo "Files:"
    ( cd "$out" && ls -la --time-style=+ | sed 's/^/  /' )
    echo
    echo "Contains device secrets (TLS private key, SNMP community, web"
    echo "password) in the flash and NVRAM images. Not for git."
} > "$out/MANIFEST.txt"

step "done"
du -sh "$out"
echo "manifest: dumps/firmware/$stamp/MANIFEST.txt"
