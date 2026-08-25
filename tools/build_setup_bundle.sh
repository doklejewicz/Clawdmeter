#!/bin/bash
# Packages the host-side setup files into a self-extracting
# firmware/data/setup.sh (tools/setup_stub.sh header + a raw tar.gz
# appended after its __ARCHIVE_BELOW__ marker). PlatformIO's
# `pio run -e <env> -t uploadfs` then flashes it verbatim to the board's
# SPIFFS partition; the device serves it back out over serial byte-for-byte
# (see the `get-setup` command in main.cpp), so a machine with nothing but a
# serial connection can bootstrap the daemon straight from an
# already-flashed device — download it, then just run it. See
# daemon/BOOTSTRAP.md.
#
# Run this, then `pio run -d firmware -e <env> -t uploadfs`, whenever the
# bundled files (or the stub) change. Not run automatically by the normal
# firmware build — the bundle only needs updating when daemon/install.sh
# actually change, not on every firmware iteration.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$SCRIPT_DIR/firmware/data"
OUT_FILE="$OUT_DIR/setup.sh"
TMP_TAR=$(mktemp /tmp/clawdmeter_bundle_XXXXXX.tar.gz)
trap 'rm -f "$TMP_TAR"' EXIT

mkdir -p "$OUT_DIR"

cd "$SCRIPT_DIR"
tar czf "$TMP_TAR" \
    install.sh \
    daemon/claude-usage-daemon.sh \
    daemon/claude-usage-daemon.service \
    daemon/clawdmeter_sessions.py \
    daemon/SESSIONS.md

cat "$SCRIPT_DIR/tools/setup_stub.sh" "$TMP_TAR" > "$OUT_FILE"
chmod +x "$OUT_FILE"

echo "Wrote $OUT_FILE ($(du -h "$OUT_FILE" | cut -f1))"
echo "Now run: pio run -d firmware -e <env> -t uploadfs"
