#!/bin/bash
# Downloads the self-extracting setup bundle from an already-flashed
# Clawdmeter device's `get-setup` serial command and runs it. See
# daemon/BOOTSTRAP.md for what this is and why it exists (bootstrapping the
# daemon on a machine that doesn't have this repo cloned yet).
#
# Deliberately bash + coreutils only (stty, cat, head, tail, md5sum) — no
# python/pyserial dependency, so it runs on a genuinely bare machine.
#
# Usage: ./get_setup.sh [/dev/ttyUSB0]
set -u

PORT="${1:-/dev/ttyUSB0}"
[ -c "$PORT" ] || { echo "not a character device: $PORT" >&2; exit 1; }
stty -F "$PORT" 115200 raw -echo

# Verify-and-retry, not just a length check: this link has no flow control,
# and a dropped byte mid-transfer doesn't shorten what the reader receives
# (it just reads further into the trailing SETUP_END marker to make up the
# count) — only a content hash catches that.
#
# The whole response (header + payload + trailer) is captured by a single
# continuous `cat`, then parsed out of the resulting file afterward — NOT
# read header-line-first via the `read` builtin and then handed off to a
# separately forked `head -c` for the payload. That split leaves a fork/exec
# -sized gap where nothing is draining the kernel's tty input buffer.
# Confirmed on hardware: a single continuous `cat` capture of this link is
# always bit-perfect — even two back-to-back requests on one never-closed
# fd came back byte-for-byte correct — while the split-read approach
# corrupted or lost responses repeatedly under load. `cat` is backgrounded
# so we can stop it as soon as the trailing marker shows up instead of
# always waiting out the full timeout.
for attempt in 1 2 3 4 5 6; do
    exec 3<>"$PORT"
    : > raw.bin
    cat <&3 > raw.bin &
    catpid=$!
    echo get-setup >&3

    deadline=$((SECONDS + 12))
    while [ "$SECONDS" -lt "$deadline" ]; do
        grep -aq 'SETUP_END\|SETUP_UNSUPPORTED\|SETUP_ERR' raw.bin 2>/dev/null && break
        sleep 0.1
    done
    kill "$catpid" 2>/dev/null
    wait "$catpid" 2>/dev/null
    exec 3<&-

    # SETUP_START may not be the very first bytes (the firmware's own
    # debug/log output shares the same serial stream and isn't synchronized
    # with command responses), so scan for it rather than assuming line 1.
    offset=$(grep -abo 'SETUP_START\|SETUP_UNSUPPORTED\|SETUP_ERR' raw.bin | head -1 | cut -d: -f1)
    if [ -z "$offset" ]; then
        echo "attempt $attempt: no response from device" >&2
        continue
    fi

    header_line=$(tail -c +"$((offset + 1))" raw.bin | head -n1 | tr -d '\r\n')
    case "$header_line" in
        SETUP_UNSUPPORTED|SETUP_ERR)
            echo "device reported: $header_line" >&2
            exit 1
            ;;
    esac

    read -r _ size want_md5 <<< "$header_line"
    header_line_bytes=$(tail -c +"$((offset + 1))" raw.bin | head -n1 | wc -c)
    payload_start=$((offset + header_line_bytes + 1))
    tail -c +"$payload_start" raw.bin | head -c "$size" > setup.sh

    got_md5=$(md5sum setup.sh | cut -d' ' -f1)
    if [ "$got_md5" = "$want_md5" ]; then
        echo "Downloaded setup.sh ($size bytes, checksum verified)"
        exec bash setup.sh
    fi
    echo "attempt $attempt: checksum mismatch (transfer corrupted), retrying..." >&2
done

echo "Failed after 6 attempts — the transfer keeps getting corrupted." >&2
echo "Try a slower/different USB cable or port, or re-run this script." >&2
exit 1
