#!/bin/bash
# Take a screenshot from the Clawdmeter display.
# Usage: ./screenshot.sh [output.png] [port]
# Default port: /dev/cu.usbmodem101 on macOS, /dev/ttyACM0 on Linux. Only
# used as a direct fallback — see below.

OUTPUT="${1:-screenshot.png}"
if [ -z "$2" ]; then
    case "$(uname -s)" in
        Darwin) PORT="/dev/cu.usbmodem101" ;;
        *)      PORT="/dev/ttyACM0" ;;
    esac
else
    PORT="$2"
fi

TMPRAW=$(mktemp /tmp/screenshot_XXXXXX.raw)
TMPDIMS=$(mktemp /tmp/screenshot_XXXXXX.dims)
trap "rm -f '$TMPRAW' '$TMPDIMS'" EXIT

# claude-usage-daemon.sh (if running) owns the serial port continuously —
# opening it ourselves too would race it and corrupt the capture (confirmed
# live: intermittent "device reports readiness to read but returned no
# data" / truncated transfers while the daemon was connected). When it's
# running, ask IT to capture the frame via a small request/response file
# handoff under ~/.config/claude-usage-monitor/ instead of touching the
# port ourselves; $PORT is unused in that path since the daemon already
# knows its own port. Falls back to opening the port directly only when the
# daemon isn't running at all (e.g. a bare board with no daemon installed).
CONFIG_DIR="$HOME/.config/claude-usage-monitor"
SCREENSHOT_REQUEST_FILE="$CONFIG_DIR/screenshot.request"
SCREENSHOT_META_FILE="$CONFIG_DIR/screenshot.response.meta"
SCREENSHOT_RAW_FILE="$CONFIG_DIR/screenshot.response.raw"
SCREENSHOT_PROGRESS_FILE="$CONFIG_DIR/screenshot.progress"

# Renders an updating progress bar on stderr (stdout stays clean/scriptable).
# $2 empty/0 means "no total yet" — shows a waiting indicator instead of a
# bar, since the daemon doesn't know the frame size until the device's
# header line arrives.
_progress() {
    [ -t 2 ] || return 0
    local cur="$1" total="${2:-0}" width=30
    if [ "$total" -gt 0 ]; then
        local filled=$((width * cur / total))
        [ "$filled" -gt "$width" ] && filled=$width
        printf '\r[%-*s] %3d%% (%d/%d bytes)  ' "$width" \
            "$(printf '%*s' "$filled" '' | tr ' ' '#')" "$((cur * 100 / total))" "$cur" "$total"
    else
        printf '\rWaiting for device...  '
    fi
}

# One request/response round; does NOT retry itself. Returns 0 on success,
# 1 on a transient failure worth retrying (device didn't answer / capture
# stalled), 2 on a failure retrying won't fix (device error, unsupported
# board, daemon currently on BLE not serial).
request_via_daemon() {
    mkdir -p "$CONFIG_DIR"
    rm -f "$SCREENSHOT_META_FILE" "$SCREENSHOT_RAW_FILE" "$SCREENSHOT_PROGRESS_FILE"
    local token="$$-$(date +%s%N)"
    printf '%s\n' "$token" > "$SCREENSHOT_REQUEST_FILE.tmp" \
        && mv -f "$SCREENSHOT_REQUEST_FILE.tmp" "$SCREENSHOT_REQUEST_FILE"

    # Generous enough to exceed the daemon's own worst-case internal
    # timeout across every board (largest no-PSRAM frame, 480x480x2 bytes,
    # daemon-side deadline ~= 8 + 460800/3000 =~ 162s at 115200 baud).
    local deadline=$((SECONDS + 200)) meta="" prog cur total
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ -f "$SCREENSHOT_PROGRESS_FILE" ]; then
            prog=$(cat "$SCREENSHOT_PROGRESS_FILE" 2>/dev/null)
            case "$prog" in
                "$token "*) read -r _ cur total <<< "$prog"; _progress "$cur" "$total" ;;
            esac
        fi
        if [ -f "$SCREENSHOT_META_FILE" ]; then
            meta=$(cat "$SCREENSHOT_META_FILE")
            case "$meta" in "$token "*) break ;; esac
        fi
        sleep 0.3
    done
    [ -t 2 ] && printf '\n' >&2

    case "$meta" in
        "$token "*) ;;
        *)
            rm -f "$SCREENSHOT_REQUEST_FILE"
            echo "Daemon didn't respond in time" >&2
            return 1
            ;;
    esac

    local _tok status w h size
    read -r _tok status w h size <<< "$meta"
    case "$status" in
        OK)
            cp "$SCREENSHOT_RAW_FILE" "$TMPRAW"
            echo "${w}x${h}" > "$TMPDIMS"
            echo "Captured ${w}x${h} ($size bytes) via daemon"
            return 0
            ;;
        SCREENSHOT_ERR)
            echo "Device reported a screenshot error" >&2
            return 2 ;;
        SCREENSHOT_UNSUPPORTED)
            echo "This board has no PSRAM and doesn't support screenshots" >&2
            return 2 ;;
        NOT_ON_SERIAL)
            echo "Daemon is on BLE, not USB serial — plug the board's USB cable in and retry" >&2
            return 2 ;;
        TIMEOUT_HEADER)
            echo "Device didn't respond" >&2
            return 1 ;;
        TIMEOUT_PAYLOAD)
            echo "Capture stalled partway through" >&2
            return 1 ;;
        *)
            echo "Unexpected daemon response: $meta" >&2
            return 2 ;;
    esac
}

if systemctl --user is-active --quiet claude-usage-daemon 2>/dev/null; then
    echo "Taking screenshot via claude-usage-daemon..."
    STATUS=1
    for attempt in 1 2 3; do
        request_via_daemon
        STATUS=$?
        [ "$STATUS" -eq 0 ] && break
        [ "$STATUS" -eq 2 ] && break   # not worth retrying
        [ "$attempt" -lt 3 ] && echo "Retrying ($((attempt + 1))/3)..."
    done
    if [ "$STATUS" -ne 0 ]; then
        echo "Screenshot capture failed"
        exit 1
    fi
else
    # Use pio's bundled python if pyserial isn't on the system python.
    PY="python3"
    if ! python3 -c "import serial" 2>/dev/null; then
        if [ -x "$HOME/.platformio/penv/bin/python" ]; then
            PY="$HOME/.platformio/penv/bin/python"
        fi
    fi

    echo "Taking screenshot from $PORT..."

    "$PY" - "$PORT" "$TMPRAW" "$TMPDIMS" << 'PYEOF'
import serial, sys, time

port_path, raw_path, dims_path = sys.argv[1], sys.argv[2], sys.argv[3]

# This cheap CH340-over-USB link has no flow control and is known to stall
# or drop bytes silently for a few seconds at a time (see BOOTSTRAP.md /
# get_setup.sh for the same issue on the setup-bundle transfer). A full
# frame on PSRAM-free boards streams strip-by-strip interleaved with
# on-device render time, so gaps between chunks are normal — give it a
# generous per-read timeout and a few whole-capture retries rather than
# failing on the first stall.
ATTEMPTS = 4
READ_TIMEOUT = 25

for attempt in range(1, ATTEMPTS + 1):
    try:
        port = serial.Serial(port_path, 115200, timeout=READ_TIMEOUT)
        port.reset_input_buffer()
        port.write(b"screenshot\n")
        port.flush()

        w = h = raw_size = None
        while True:
            line = port.readline().decode("utf-8", errors="replace").strip()
            if line.startswith("SCREENSHOT_START"):
                parts = line.split()
                w, h, raw_size = int(parts[1]), int(parts[2]), int(parts[3])
                break
            if line == "SCREENSHOT_ERR":
                print("Device reported screenshot error", file=sys.stderr)
                sys.exit(1)
            if line == "":
                raise TimeoutError("no SCREENSHOT_START header")

        show_progress = sys.stderr.isatty()
        data = b""
        while len(data) < raw_size:
            chunk = port.read(min(4096, raw_size - len(data)))
            if not chunk:
                if show_progress:
                    print(file=sys.stderr)
                raise TimeoutError(f"got {len(data)} of {raw_size} bytes")
            data += chunk
            if show_progress:
                pct = len(data) * 100 // raw_size
                width = 30
                filled = min(width, width * len(data) // raw_size)
                bar = "#" * filled + " " * (width - filled)
                print(f"\r[{bar}] {pct:3d}% ({len(data)}/{raw_size} bytes)  ",
                      end="", file=sys.stderr, flush=True)
        if show_progress:
            print(file=sys.stderr)

        for _ in range(10):
            line = port.readline().decode("utf-8", errors="replace").strip()
            if line == "SCREENSHOT_END":
                break

        port.close()
        with open(raw_path, "wb") as f:
            f.write(data)
        with open(dims_path, "w") as f:
            f.write(f"{w}x{h}\n")
        print(f"Captured {w}x{h} ({len(data)} bytes)")
        sys.exit(0)

    except (TimeoutError, serial.SerialException) as e:
        print(f"Attempt {attempt}/{ATTEMPTS} failed: {e}", file=sys.stderr)
        try:
            port.close()
        except Exception:
            pass
        if attempt < ATTEMPTS:
            time.sleep(1)

print("All attempts failed", file=sys.stderr)
sys.exit(1)
PYEOF

    if [ $? -ne 0 ]; then
        echo "Screenshot capture failed"
        exit 1
    fi
fi

DIMS=$(cat "$TMPDIMS")
ffmpeg -y -f rawvideo -pixel_format rgb565le -video_size "$DIMS" \
    -i "$TMPRAW" -update 1 -frames:v 1 "$OUTPUT" 2>/dev/null || true


if [ -f "$OUTPUT" ]; then
    echo "Saved: $OUTPUT ($DIMS)"
else
    echo "Error: conversion failed"
    exit 1
fi
