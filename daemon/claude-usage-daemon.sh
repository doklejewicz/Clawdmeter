#!/bin/bash
# Claude Usage Tracker Daemon (BLE or USB serial)
# Reads Claude Code OAuth token, polls usage via API, sends it to the ESP32
# over whichever transport is available: a physical USB-to-UART bridge such
# as CH340 if the board is plugged in and answers `identify` (no BLE pairing
# needed at all), falling back to BLE GATT otherwise. Both transports share
# the same payload-building code; only the last-mile send differs, and every
# reconnect cycle re-checks for serial first, so plugging the board in while
# it's running on BLE switches it over on the next cycle.
# Also supervises the optional session-awareness sidecar (clawdmeter_sessions.py,
# issue #135) as a child process when hook_port is configured — one process to
# start/stop/systemd-manage, since shipping session data is pointless if this
# daemon isn't running to ship it. Live sessions ship over both transports —
# the BLE SS characteristic or a plain serial line, whichever is active.
# See daemon/SESSIONS.md.
# Dependencies: curl, awk, bluetoothctl (BLE fallback only), python3 (sidecar only)

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    cat <<'EOF'
Claude Usage Tracker Daemon (BLE or USB serial)

Reads your Claude Code OAuth token, polls Anthropic for usage, and ships it
to the Clawdmeter ESP32 over whichever transport is available. USB serial
(any CH340-class USB-UART bridge) is tried first and needs no BLE pairing
at all — just the board plugged in; BLE GATT is the fallback when no
serial port answers. Also supervises the optional session-awareness
sidecar (clawdmeter_sessions.py) as a child process when hook_port is
configured — see daemon/SESSIONS.md for the full design. Live session
views work over either transport.

Usage: claude-usage-daemon.sh [-h|--help]

No other flags — this runs as a long-lived daemon (systemd unit
claude-usage-daemon, installed by ./install.sh) or directly in a terminal.

Environment:
  DEVICE_MAC    Skip BLE discovery and connect to this MAC directly
                (only used when falling back to BLE).

Config file: ~/.config/claude-usage-monitor/config ("key = value" per line,
"#" starts a trailing comment). Recognized keys:

  serial_port            Explicit device path (e.g. /dev/ttyUSB0) to use
                         for the USB serial transport instead of
                         auto-detecting. Unset (default) = probe every
                         /dev/ttyUSB*/ttyACM* with the firmware's
                         `identify` command and use whichever one answers
                         as a Clawdmeter.

  config_dirs           Comma-separated Claude config dirs to poll/watch.
                         Default: ~/.claude. Supports "~" and "~/...".
                         Multiple dirs let one device show several
                         accounts/projects at once WITHOUT merging their
                         data — each dir keeps its own separate credentials,
                         rosters, and transcripts; only this daemon's own
                         read-only view aggregates them. Useful for
                         per-project isolation with Claude Code in Docker:
                         give each project its own dir (e.g. ~/.claude-work),
                         mount only that one into that project's container,
                         and list every dir you want visible on the device
                         here. See "Docker / devcontainer sessions" below.

  hook_port              Loopback port for the session-awareness sidecar's
                         HTTP hook listener. Unset (default) = feature off:
                         the sidecar never starts, the device shows no
                         session data. Set by ./install.sh's prompt, or by
                         hand — see daemon/SESSIONS.md's setup steps (you
                         also need the matching hook block in each config
                         dir's settings.json).

  sessions_budget_bytes  Byte budget for the session-view wire payload
                         (sidecar-only setting). Default: 400. Labels
                         truncate toward an 8-char floor first, then
                         least-urgent rows drop, once a payload exceeds
                         this; raise it if you regularly run enough
                         concurrent sessions that names still feel
                         squeezed. Keep it under the ~514-byte ceiling of
                         the firmware's negotiated 517-byte BLE MTU.

  context_window_k       Pin the context-window-% heuristic to this many
                         kilotokens (sidecar-only setting) instead of the
                         200k/1M auto-detect. Usually leave unset.

  clock                  off (default) / auto / 12 / 24 — show a live
                         clock instead of the "Usage" title.

  chime                  on / off (default) — play the session-reset
                         chime through the board speaker (boards with one).

Docker / devcontainer sessions:
  Claude Code running inside a container needs three things to show up
  here, none of which require any code change — see daemon/SESSIONS.md:
    1. Hook connectivity: run the container with --network=host so its
       127.0.0.1 IS the host's 127.0.0.1 (hooks POST to a loopback URL).
    2. Correct liveness: run with --pid=host so the sidecar's pid_alive()
       (a plain /proc/<pid>/stat check) sees the container's processes
       under their real host PIDs instead of a container-local number
       that means nothing on the host.
    3. Visible config: bind-mount a Claude config dir into the container
       at its normal ~/.claude path, and list that dir (from the HOST'S
       path to it) in config_dirs above. Use a DEDICATED dir per project
       if you don't want different projects' sessions/credentials sharing
       state with each other — never bind-mount the same dir into
       containers you want isolated from one another.
  Both flags are Linux-only Docker features.
EOF
    exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SESSIONS_BIN="$SCRIPT_DIR/clawdmeter_sessions.py"
SESSIONS_PID=""

DEVICE_NAME="Clawdmeter"
MAC_RE='([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}'
DEVICE_MAC="${DEVICE_MAC:-}"  # auto-discovered if empty
SERVICE_UUID="4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID="4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID="4c41555a-4465-7669-6365-000000000004"
SS_CHAR_UUID="4c41555a-4465-7669-6365-000000000005"  # live session rows (issue #135)
SESSIONS_FILE="$HOME/.claude/clawdmeter-sessions.json"
POLL_INTERVAL=60
TICK=5
SAVED_MAC_FILE="$HOME/.config/claude-usage-monitor/ble-address"
CONFIG_FILE="$HOME/.config/claude-usage-monitor/config"
REFRESH_FLAG="/tmp/claude-usage-refresh-$$"
DBUS_DEST="org.bluez"
NOTIFY_PID=""
SS_CHAR_PATH=""
LAST_SESSIONS_SIG=""

log() {
    echo "[$(date '+%H:%M:%S')] $1"
}

# Last-wins `key = value` lookup in the shared config file; trailing comments
# stripped. Empty if unset. Mirrors clawdmeter_sessions.py's own
# read_config_value() and install.sh's current_config_value() — three
# implementations of the same one-liner because bash/python don't share code,
# not because the format differs.
read_config_value() {
    local key="$1"
    [ -f "$CONFIG_FILE" ] || return 0
    grep -E "^[[:space:]]*${key}[[:space:]]*=" "$CONFIG_FILE" | tail -1 \
        | tr -d '\r' \
        | sed -E "s/^[[:space:]]*${key}[[:space:]]*=[[:space:]]*//; s/[[:space:]]*(#.*)?\$//"
}

# Start the sidecar iff the feature is configured (hook_port set) and it
# isn't already running. Safe to call repeatedly — used both at startup and
# as a per-tick health check, so a crashed sidecar is respawned without
# restarting this whole daemon (and its BLE connection) over it.
start_sessions_sidecar() {
    local port
    port=$(read_config_value hook_port)
    [ -z "$port" ] && return 0   # feature off — nothing to run
    if [ -n "$SESSIONS_PID" ] && kill -0 "$SESSIONS_PID" 2>/dev/null; then
        return 0   # already running
    fi
    log "Starting session-awareness sidecar (hook_port=$port)..."
    python3 "$SESSIONS_BIN" &
    SESSIONS_PID=$!
}

stop_sessions_sidecar() {
    [ -n "$SESSIONS_PID" ] || return 0
    if kill -0 "$SESSIONS_PID" 2>/dev/null; then
        kill "$SESSIONS_PID" 2>/dev/null
        wait "$SESSIONS_PID" 2>/dev/null
    fi
    SESSIONS_PID=""
}

# --- Multi config-dir support ---------------------------------------------
# Claude Code can run against more than one config dir (e.g. ~/.claude for a
# personal plan and ~/.claude-work for a work plan, selected via
# CLAUDE_CONFIG_DIR). The daemon polls each configured dir's token every cycle
# and shows whichever plan is "active" (the one whose usage moved most recently
# — see build_active_payload()). Per-dir state persists across calls for
# that decision.
declare -A PREV_S       # last session % seen per dir (detects a rise = activity)
declare -A LAST_ACTIVE  # poll-sequence number of the last observed rise (0 = never)
POLL_SEQ=0              # monotonic poll counter — recency ordering that's immune to
                        # wall-clock resolution and NTP steps (polls are 60s apart, but
                        # a counter is unambiguous even if two land in the same second)

# Read the `config_dirs` option: a comma-separated list of Claude config dirs.
# Defaults to "~/.claude" so existing single-plan setups are unchanged. Tildes
# and $HOME are expanded; blanks trimmed. Echoes one resolved dir per line.
read_config_dirs() {
    local raw=""
    if [ -f "$CONFIG_FILE" ]; then
        raw=$(grep -E '^[[:space:]]*config_dirs[[:space:]]*=' "$CONFIG_FILE" | tail -1 \
            | tr -d '\r' \
            | sed -E 's/^[[:space:]]*config_dirs[[:space:]]*=[[:space:]]*//; s/[[:space:]]*(#.*)?$//')
    fi
    [ -z "$raw" ] && raw="$HOME/.claude"
    local IFS=','
    local d
    for d in $raw; do
        d=$(echo "$d" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')
        [ -z "$d" ] && continue
        case "$d" in
            "~")   d="$HOME" ;;
            "~/"*) d="$HOME/${d#\~/}" ;;
        esac
        echo "$d"
    done
}

# Read the OAuth access token from a specific config dir's credentials file.
# The file can hold many "accessToken" fields — one per OAuth integration (MCP
# servers, design tools, etc.) — so we must isolate the claudeAiOauth object
# first and pull ITS accessToken. A bare grep for "accessToken" would return
# every token concatenated, yielding an invalid Bearer header and constant 401s.
read_token_for() {
    local dir="$1"
    grep -o '"claudeAiOauth"[[:space:]]*:[[:space:]]*{[^}]*}' "$dir/.credentials.json" 2>/dev/null \
        | grep -o '"accessToken":"[^"]*"' | head -1 | cut -d'"' -f4
}

# Read the `chime` option from the config file. Echoes one of: off|on.
# Defaults to "off" so the device stays silent until the user opts in.
read_chime_setting() {
    local val=""
    if [ -f "$CONFIG_FILE" ]; then
        val=$(grep -E '^[[:space:]]*chime[[:space:]]*=' "$CONFIG_FILE" | tail -1 \
            | tr -d '\r' \
            | sed -E 's/^[[:space:]]*chime[[:space:]]*=[[:space:]]*//; s/[[:space:]]*(#.*)?$//' \
            | tr '[:upper:]' '[:lower:]')
    fi
    case "$val" in
        on) echo "on" ;;
        *)  echo "off" ;;
    esac
}

# Read the `clock` option from the config file. Echoes one of: off|auto|12|24.
# Defaults to "off" so existing setups keep showing "Usage" until opted in.
read_clock_setting() {
    local val=""
    if [ -f "$CONFIG_FILE" ]; then
        val=$(grep -E '^[[:space:]]*clock[[:space:]]*=' "$CONFIG_FILE" | tail -1 \
            | tr -d '\r' \
            | sed -E 's/^[[:space:]]*clock[[:space:]]*=[[:space:]]*//; s/[[:space:]]*(#.*)?$//' \
            | tr '[:upper:]' '[:lower:]')
    fi
    case "$val" in
        off|auto|12|24) echo "$val" ;;
        *)              echo "off" ;;
    esac
}

# Best-effort 12h/24h detection from the locale. Echoes 12 or 24 (default 24).
detect_hour_format() {
    local tfmt
    tfmt=$(locale -k LC_TIME 2>/dev/null | grep -E '^t_fmt=')
    case "$tfmt" in
        *%p*|*%r*|*%I*) echo 12 ;;
        *)              echo 24 ;;
    esac
}

# The powered controller's own MAC (not the peripheral's), for `bluetoothctl
# select` — see mac_to_dbus_path's comment. `select` only pins a controller
# for the CURRENT bluetoothctl session, so every session below re-issues it
# rather than relying on whatever BlueZ considers "default" right now.
powered_controller_mac() {
    local ctrl
    while read -r ctrl; do
        [ -z "$ctrl" ] && continue
        if bluetoothctl show "$ctrl" 2>/dev/null | grep -q "Powered: yes"; then
            echo "$ctrl"
            return 0
        fi
    done < <(bluetoothctl list 2>/dev/null | awk '{print $2}')
    return 1
}

# Pull a device's MAC out of raw bluetoothctl output robustly. bluetoothctl's
# own prompt becomes "[<DeviceName>]> " once it focuses a device — since our
# device's name is literally "Clawdmeter", every subsequent output line then
# contains "Clawdmeter" too, and a naive `grep "$DEVICE_NAME" | awk '{print
# $2}'` matches the wrong line (prompt noise, not the actual device listing)
# and returns garbage instead of a MAC. Strip ANSI/CR noise first, then
# require the literal "Device <mac> ... <name>" shape before extracting the
# MAC by pattern, not by field position.
extract_device_mac() {
    sed -r 's/\x1B\[[0-9;]*[a-zA-Z]//g' | tr -d '\r' \
        | grep -E "Device ${MAC_RE} .*${DEVICE_NAME}" \
        | grep -oE "$MAC_RE" | head -1
}

# Convert MAC to D-Bus path: AA:BB:CC:DD:EE:FF -> dev_AA_BB_CC_DD_EE_FF
mac_to_dbus_path() {
    # On a host with more than one Bluetooth controller, GetManagedObjects's
    # ordering is not guaranteed to put the one actually in use first — a
    # naive `head -1` can silently pick a different, powered-off controller,
    # in which case every path built from it points at a device object that
    # was never bonded/connected. Prefer whichever controller is actually
    # powered; only fall back to "first listed" if none report powered
    # (matches this script's behavior before this check existed).
    local adapter powered_adapter
    for adapter in $(busctl call org.bluez / org.freedesktop.DBus.ObjectManager GetManagedObjects 2>/dev/null | grep -o '/org/bluez/hci[0-9]*' | sort -u); do
        if busctl get-property org.bluez "$adapter" org.bluez.Adapter1 Powered 2>/dev/null | grep -q "true"; then
            powered_adapter="$adapter"
            break
        fi
    done
    adapter="${powered_adapter:-$(busctl call org.bluez / org.freedesktop.DBus.ObjectManager GetManagedObjects 2>/dev/null | grep -o '/org/bluez/hci[0-9]*' | head -1)}"
    adapter=${adapter:-/org/bluez/hci0}
    echo "${adapter}/dev_$(echo "$1" | tr ':' '_')"
}

# Check if device is connected via D-Bus
is_connected() {
    local path
    path=$(mac_to_dbus_path "$DEVICE_MAC")
    busctl get-property "$DBUS_DEST" "$path" org.bluez.Device1 Connected 2>/dev/null | grep -q "true"
}

# Load saved MAC address
load_mac() {
    if [ -n "$DEVICE_MAC" ]; then return 0; fi
    if [ -f "$SAVED_MAC_FILE" ]; then
        DEVICE_MAC=$(head -1 "$SAVED_MAC_FILE" | tr -d '\r\n ')
        if [[ "$DEVICE_MAC" =~ ^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$ ]]; then
            return 0
        fi
        log "Cached MAC is malformed, discarding"
        rm -f "$SAVED_MAC_FILE"
        DEVICE_MAC=""
    fi
    return 1
}

# Save MAC for fast reconnect
save_mac() {
    mkdir -p "$(dirname "$SAVED_MAC_FILE")"
    echo "$DEVICE_MAC" > "$SAVED_MAC_FILE"
}

# Find a Clawdmeter the system already knows about — paired first, then merely
# connected — WITHOUT an LE advertising scan. bluez only lists devices this host
# has bonded/connected to, so we can't accidentally grab a stranger's advertising
# unit. The device is a bonded BLE HID keyboard you pair once anyway, so we never
# scan by name. Sets DEVICE_MAC + caches it on success; returns non-zero if none.
find_system_device_mac() {
    local found=""
    local mode ctrl
    ctrl=$(powered_controller_mac)
    for mode in Paired Connected; do
        if [ -n "$ctrl" ]; then
            found=$( { echo "select $ctrl"; echo "devices $mode"; } | bluetoothctl 2>/dev/null | extract_device_mac)
        else
            found=$(bluetoothctl devices "$mode" 2>/dev/null | extract_device_mac)
        fi
        [ -n "$found" ] && break
    done
    if [ -n "$found" ]; then
        DEVICE_MAC="$found"
        save_mac
        log "Using system-known device: $DEVICE_MAC"
        return 0
    fi
    return 1
}

# Connect to the device
connect_device() {
    log "Connecting to $DEVICE_MAC..."

    local ctrl
    ctrl=$(powered_controller_mac)
    if [ -n "$ctrl" ]; then
        # Trust + connect in one session so `select` (session-local, see
        # powered_controller_mac's comment) applies to both.
        { echo "select $ctrl"; echo "trust $DEVICE_MAC"; echo "connect $DEVICE_MAC"; sleep 3; echo "quit"; } | bluetoothctl &>/dev/null
    else
        # No controller reported itself powered (shouldn't normally happen) —
        # fall back to whatever bluetoothctl considers default.
        bluetoothctl trust "$DEVICE_MAC" &>/dev/null
        bluetoothctl connect "$DEVICE_MAC" &>/dev/null
    fi
    sleep 2

    if is_connected; then
        log "Connected"
        return 0
    fi
    log "Connection failed"
    # Drop the cached MAC so the next loop re-derives it from bluez's paired/
    # connected list (see find_system_device_mac). We deliberately do NOT
    # `bluetoothctl remove` here: the daemon now only ever connects to a device
    # the system already knows, so unpairing it on a transient failure would
    # make it undiscoverable and strand the daemon.
    if [ -f "$SAVED_MAC_FILE" ] && [ "$(cat "$SAVED_MAC_FILE")" = "$DEVICE_MAC" ]; then
        log "Invalidating cached MAC, will re-derive from paired/connected devices"
        rm -f "$SAVED_MAC_FILE"
    fi
    DEVICE_MAC=""
    return 1
}

# Find a GATT characteristic path by UUID via D-Bus
find_char_path_by_uuid() {
    local target_uuid="$1"
    local dev_path
    dev_path=$(mac_to_dbus_path "$DEVICE_MAC")

    busctl tree "$DBUS_DEST" 2>/dev/null | grep -o "${dev_path}/service[0-9a-f]*/char[0-9a-f]*" | while read -r char_path; do
        local uuid
        uuid=$(busctl get-property "$DBUS_DEST" "$char_path" org.bluez.GattCharacteristic1 UUID 2>/dev/null | tr -d '"' | awk '{print $2}')
        if [ "$uuid" = "$target_uuid" ]; then
            echo "$char_path"
            return 0
        fi
    done
}

# Subscribe to refresh-request notifications. The ESP fires this when it
# has no usage data yet (e.g. after a fresh boot). Daemon awk drops a flag
# file that the inner loop picks up on its next 5s tick.
#
# Implementation notes:
# - dbus-monitor must be running BEFORE we call StartNotify, because busctl
#   exits immediately, the subscription tears down within milliseconds, and
#   the ESP's notify fires inside that brief window.
# - stdbuf -oL forces line-buffered stdout on dbus-monitor; without it,
#   glibc switches to block buffering when stdout is a pipe and signals
#   never reach awk until ~4KB accumulates.
# - The pipeline runs in a setsid'd child so we can kill the whole process
#   group (dbus-monitor + awk) atomically. Killing only awk leaves
#   dbus-monitor orphaned, and `wait $!` in bash waits on the whole job
#   until every pipeline member exits, hanging the daemon.
start_notify_subscriber() {
    local req_path
    req_path=$(find_char_path_by_uuid "$REQ_CHAR_UUID")
    if [ -z "$req_path" ]; then
        log "Refresh char not found, skipping notify subscriber"
        return 1
    fi

    setsid bash -c "stdbuf -oL dbus-monitor --system \"type='signal',interface='org.freedesktop.DBus.Properties',path='$req_path',member='PropertiesChanged'\" 2>/dev/null | awk -v flag='$REFRESH_FLAG' '/Value/ { system(\"touch \" flag); fflush() }'" &
    NOTIFY_PID=$!

    # Give dbus-monitor a moment to register its match rule, then trigger
    # the GATT subscription that causes the ESP to fire its notify.
    sleep 0.3
    busctl call "$DBUS_DEST" "$req_path" org.bluez.GattCharacteristic1 StartNotify >/dev/null 2>&1

    log "Refresh subscriber started (pgid=$NOTIFY_PID)"
}

stop_notify_subscriber() {
    if [ -n "$NOTIFY_PID" ]; then
        # Kill the whole process group (setsid made NOTIFY_PID the leader).
        # Don't wait — we don't care about exit status and waiting can hang
        # if any group member is slow to exit.
        kill -TERM -"$NOTIFY_PID" 2>/dev/null
        NOTIFY_PID=""
    fi
    rm -f "$REFRESH_FLAG"
}

# Write data to the RX characteristic via D-Bus
write_gatt() {
    local char_path="$1"
    local data="$2"

    # Convert string to byte array for D-Bus: "hi" -> 0x68 0x69
    local bytes=""
    for ((i = 0; i < ${#data}; i++)); do
        local byte
        byte=$(printf "0x%02x" "'${data:$i:1}")
        bytes="$bytes $byte"
    done
    local count=${#data}

    busctl call "$DBUS_DEST" "$char_path" org.bluez.GattCharacteristic1 \
        WriteValue "aya{sv}" "$count" $bytes 0 2>/dev/null
}

# UTF-8-safe GATT write. write_gatt() above converts per CHARACTER, which is
# fine for the all-ASCII usage payload but corrupts multi-byte UTF-8 (e.g. the
# "…" in middle-elided session labels: printf "'…" yields the code point 0x2026,
# not bytes). od emits the actual byte values, whatever the locale.
write_gatt_bytes() {
    local char_path="$1"
    local data="$2"
    local bytes count
    bytes=$(printf '%s' "$data" | od -An -v -tu1 | tr -s ' \n' ' ')
    count=$(printf '%s' "$data" | wc -c)
    # shellcheck disable=SC2086  # $bytes is a deliberate word list
    busctl call "$DBUS_DEST" "$char_path" org.bluez.GattCharacteristic1 \
        WriteValue "aya{sv}" "$count" $bytes 0 2>/dev/null
}

# --- USB serial transport ---------------------------------------------------
# The alternative to BLE: the firmware also accepts the same usage JSON (and,
# separately, session-list JSON — see maybe_send_sessions_serial() below) one
# line at a time over its USB-UART bridge (see check_serial_cmd() in
# main.cpp) and answers identify/each payload with a line of its own, so no
# BLE pairing is needed at all. Both transports listen unconditionally on
# the device — this only decides which one *this daemon* uses.
SERIAL_ACK_TIMEOUT=3

# Open $1 at the firmware's fixed baud rate on fd 9, raw mode, no echo.
# Every serial helper below opens-writes-reads-closes per call rather than
# holding the fd open across polls — polls are 60s apart, so the repeated
# open cost is irrelevant, and a fresh open avoids ever reading stale bytes
# left over from a previous exchange (see tools/get_setup.sh's drain gotcha).
_open_serial() {
    local dev="$1"
    [ -c "$dev" ] || return 1
    stty -F "$dev" 115200 raw -echo 2>/dev/null || return 1
    exec 9<>"$dev" 2>/dev/null || return 1
}

_close_serial() {
    exec 9<&- 2>/dev/null
}

# Send $2 (a bare command or a JSON payload line) to $1 and scan replies for
# $3 (a literal substring, e.g. '"device":"Clawdmeter"' or '"ack":true') for
# up to $SERIAL_ACK_TIMEOUT seconds. Other lines (boot logs, view_state
# debug prints — this is the same stream main.cpp's Serial.print debugging
# uses) are skipped rather than assumed to be the answer, same rationale as
# tools/get_setup.sh scanning for SETUP_START instead of trusting line 1.
_serial_roundtrip() {
    local dev="$1" line="$2" want="$3"
    _open_serial "$dev" || return 1
    printf '%s\n' "$line" >&9
    local deadline=$((SECONDS + SERIAL_ACK_TIMEOUT)) reply found=1
    while [ "$SECONDS" -lt "$deadline" ]; do
        IFS= read -r -t 1 -u 9 reply || continue
        case "$reply" in
            *"$want"*) found=0; break ;;
        esac
    done
    _close_serial
    return $found
}

# Auto-detect the Clawdmeter's serial port: an explicit `serial_port` config
# override, or every /dev/ttyUSB*/ttyACM* probed with `identify` until one
# answers as a Clawdmeter (mirrors the Windows serial daemon's
# candidate_serial_ports() + is_clawdmeter_identity()). No Bluetooth-port
# filtering needed here — unlike Windows COM ports, BlueZ doesn't expose
# bonded devices as ttyUSB/ttyACM nodes.
find_serial_port() {
    local override
    override=$(read_config_value serial_port)
    if [ -n "$override" ]; then
        _serial_roundtrip "$override" "identify" '"device":"Clawdmeter"' && echo "$override"
        return
    fi
    local dev
    for dev in /dev/ttyUSB* /dev/ttyACM*; do
        [ -e "$dev" ] || continue
        if _serial_roundtrip "$dev" "identify" '"device":"Clawdmeter"'; then
            echo "$dev"
            return 0
        fi
    done
    return 1
}

# Send one usage payload over serial and wait for {"ack":true}.
write_serial() {
    local dev="$1" payload="$2"
    _serial_roundtrip "$dev" "$payload" '"ack":true'
}

# --- Live session awareness (issue #135) -----------------------------------
# The session sidecar (spawned above by start_sessions_sidecar(); see
# SESSIONS.md) listens for Claude Code hook events and writes an
# already-fitted wire payload to ~/.claude/clawdmeter-sessions.json on every
# session state change. On the existing 5s tick, ship that payload — over the
# BLE SS characteristic or a plain serial line, whichever transport is active
# (see maybe_send_sessions() / maybe_send_sessions_serial() below) — whenever
# the file's content changed. Fully inert when the file doesn't exist
# (feature off / sidecar not running) — no errors, no log spam.

# sessions.json holds {"ts":..., "payload":"<wire string>"}. The payload is
# stored as a string so the exact bytes the sidecar fitted to the budget are
# what goes out, unchanged by either transport. Echoes it, or empty if the
# file is missing/unparseable. Shared by both maybe_send_sessions functions.
_read_sessions_payload() {
    PYTHONIOENCODING=utf-8 python3 -c 'import json,sys
try:
    print(json.load(open(sys.argv[1]))["payload"])
except Exception:
    pass' "$SESSIONS_FILE" 2>/dev/null
}

maybe_send_sessions() {
    [ -f "$SESSIONS_FILE" ] || return 0
    local sig
    sig=$(md5sum "$SESSIONS_FILE" 2>/dev/null | awk '{print $1}')
    [ -z "$sig" ] && return 0
    [ "$sig" = "$LAST_SESSIONS_SIG" ] && return 0
    # Resolve the SS char lazily, at most once per changed payload, so a
    # sidecar started after connect gets picked up without running busctl
    # tree on every idle tick.
    if [ -z "$SS_CHAR_PATH" ]; then
        SS_CHAR_PATH=$(find_char_path_by_uuid "$SS_CHAR_UUID")
        if [ -z "$SS_CHAR_PATH" ]; then
            LAST_SESSIONS_SIG="$sig"  # firmware without SS: retry on next change
            return 0
        fi
        log "GATT SS path: $SS_CHAR_PATH"
    fi
    local payload
    payload=$(_read_sessions_payload)
    [ -z "$payload" ] && { LAST_SESSIONS_SIG="$sig"; return 0; }
    if write_gatt_bytes "$SS_CHAR_PATH" "$payload"; then
        LAST_SESSIONS_SIG="$sig"
    fi
    return 0
}

# Same idea over USB serial: the payload string already IS the full
# {"ss":[...]}  JSON object (see clawdmeter_sessions.py), so it's sent as
# just another line — check_serial_cmd() in main.cpp routes it away from the
# usage-payload path by spotting the "ss" key before parsing. Boards without
# BOARD_HAS_SESSION_VIEWS reply {"ack":false}, so write_serial fails there
# and this quietly no-ops on the next changed payload, same as the BLE path
# hitting a firmware without the SS characteristic.
maybe_send_sessions_serial() {
    local dev="$1"
    [ -f "$SESSIONS_FILE" ] || return 0
    local sig
    sig=$(md5sum "$SESSIONS_FILE" 2>/dev/null | awk '{print $1}')
    [ -z "$sig" ] && return 0
    [ "$sig" = "$LAST_SESSIONS_SIG" ] && return 0
    local payload
    payload=$(_read_sessions_payload)
    [ -z "$payload" ] && { LAST_SESSIONS_SIG="$sig"; return 0; }
    if write_serial "$dev" "$payload"; then
        LAST_SESSIONS_SIG="$sig"
    fi
    return 0
}

# Build the device payload for one OAuth token. Echoes the JSON payload on
# success (empty + non-zero return on failure). Pure: no logging, no send —
# build_active_payload() owns picking the active plan, and the caller (a
# transport loop) owns actually sending it.
build_payload_for_token() {
    local token="$1"
    [ -z "$token" ] && return 1
    local now
    now=$(date +%s)

    # Optional clock. When enabled, send a local wall-clock epoch (UTC epoch shifted
    # by the timezone offset, so gmtime() on-device reads local) plus the hour format.
    local clock clock_fragment=""
    clock=$(read_clock_setting)
    if [ "$clock" != "off" ]; then
        local tz off_sec local_epoch tf
        tz=$(date +%z)            # e.g. +0200 or -0500
        off_sec=$(( (10#${tz:1:2} * 3600) + (10#${tz:3:2} * 60) ))
        [ "${tz:0:1}" = "-" ] && off_sec=$(( -off_sec ))
        local_epoch=$(( now + off_sec ))
        case "$clock" in
            12) tf=12 ;;
            24) tf=24 ;;
            *)  tf=$(detect_hour_format) ;;
        esac
        clock_fragment=",\"t\":$local_epoch,\"tf\":$tf"
    fi

    local headers
    headers=$(curl -s -D - -o /dev/null \
        "https://api.anthropic.com/v1/messages" \
        -H "Authorization: Bearer $token" \
        -H "anthropic-version: 2023-06-01" \
        -H "anthropic-beta: oauth-2025-04-20" \
        -H "Content-Type: application/json" \
        -H "User-Agent: claude-code/2.1.5" \
        -d '{"model":"claude-haiku-4-5-20251001","max_tokens":1,"messages":[{"role":"user","content":"hi"}]}' \
        2>/dev/null) || return 1

    local s5h_util overage_util overage_reset status
    s5h_util=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-utilization" | tr -d '\r' | awk '{print $2}')
    overage_util=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-overage-utilization" | tr -d '\r' | awk '{print $2}')
    overage_reset=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-overage-reset" | tr -d '\r' | awk '{print $2}')
    status=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-status" | tr -d '\r' | awk '{print $2}')
    status=${status:-unknown}

    # Optional reset chime. When enabled, tell the firmware it may sound the
    # session-reset chime by adding "c":1 to the payload (additive, off by default).
    local chime chime_fragment=""
    chime=$(read_chime_setting)
    [ "$chime" = "on" ] && chime_fragment=",\"c\":1"

    local payload
    if [ -n "$s5h_util" ]; then
        # Pro/Max account — 5h/7d windows
        local s7d_util s5h_reset s7d_reset s5h_status
        s5h_reset=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-reset" | tr -d '\r' | awk '{print $2}')
        s7d_util=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-7d-utilization" | tr -d '\r' | awk '{print $2}')
        s7d_reset=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-7d-reset" | tr -d '\r' | awk '{print $2}')
        s5h_status=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-status" | tr -d '\r' | awk '{print $2}')
        s5h_util=${s5h_util:-0}; s5h_reset=${s5h_reset:-0}
        s7d_util=${s7d_util:-0}; s7d_reset=${s7d_reset:-0}
        s5h_status=${s5h_status:-unknown}
        payload=$(awk -v u5="$s5h_util" -v r5="$s5h_reset" -v u7="$s7d_util" -v r7="$s7d_reset" -v st="$s5h_status" -v now="$now" -v clk="$clock_fragment" -v chm="$chime_fragment" \
            'BEGIN {
                sp = sprintf("%.0f", u5 * 100);
                sr = (r5 - now) / 60; sr = sr > 0 ? sprintf("%.0f", sr) : 0;
                wp = sprintf("%.0f", u7 * 100);
                wr = (r7 - now) / 60; wr = wr > 0 ? sprintf("%.0f", wr) : 0;
                printf "{\"s\":%s,\"sr\":%s,\"w\":%s,\"wr\":%s,\"st\":\"%s\",\"acct\":\"pro\"%s%s,\"ok\":true}", sp, sr, wp, wr, st, clk, chm;
            }')
    else
        # Enterprise account — spending-limit model
        overage_util=${overage_util:-0}; overage_reset=${overage_reset:-0}
        # Compute period info via python3 (awk lacks date arithmetic)
        local period_info
        period_info=$(python3 - "$now" "$overage_reset" <<'PYEOF'
import sys, datetime, calendar, json
now, reset_ts = float(sys.argv[1]), float(sys.argv[2])
dt_end = datetime.datetime.fromtimestamp(reset_ts)
pm = dt_end.month - 1 or 12
py = dt_end.year if dt_end.month > 1 else dt_end.year - 1
pd = min(dt_end.day, calendar.monthrange(py, pm)[1])
dt_start = dt_end.replace(year=py, month=pm, day=pd)
period_len = reset_ts - dt_start.timestamp()
tp = max(0, min(100, int(round((now - dt_start.timestamp()) / period_len * 100)))) if period_len > 0 else 0
pd_days = int(round(period_len / 86400))
rd = f"{dt_end.strftime('%b')} {dt_end.day}"
print(json.dumps({"tp": tp, "pd": pd_days, "rd": rd}))
PYEOF
)
        payload=$(awk -v ou="$overage_util" -v or_="$overage_reset" -v st="$status" -v now="$now" -v pi="$period_info" -v clk="$clock_fragment" -v chm="$chime_fragment" \
            'BEGIN {
                sp = sprintf("%.0f", ou * 100);
                sr = (or_ - now) / 60; sr = sr > 0 ? sprintf("%.0f", sr) : 0;
                # Extract tp, pd, rd from period_info JSON (simple regex)
                tp = 0; pd = 30; rd = "";
                match(pi, /"tp": *([0-9]+)/, a); if (RSTART) tp = a[1];
                match(pi, /"pd": *([0-9]+)/, b); if (RSTART) pd = b[1];
                match(pi, /"rd": *"([^"]+)"/, c); if (RSTART) rd = c[1];
                printf "{\"s\":%s,\"sr\":%s,\"w\":0,\"wr\":0,\"st\":\"%s\",\"acct\":\"ent\",\"tp\":%s,\"pd\":%s,\"rd\":\"%s\"%s%s,\"ok\":true}", sp, sr, st, tp, pd, rd, clk, chm;
            }')
    fi

    printf '%s' "$payload"
    return 0
}

# Extract the integer session % ("s") from a built payload, or 0.
_payload_session_pct() {
    echo "$1" | grep -o '"s":[0-9]*' | head -1 | cut -d: -f2
}

# Poll every configured config dir and decide which plan is "active" —
# shared by both transports, which differ only in how they send the result.
# "Active" = the plan whose session % rose most recently (recent API
# activity); a rise stamps LAST_ACTIVE so the choice is sticky and survives
# window resets (a drop to 0 isn't activity). Before any rise is seen
# (startup), fall back to the plan with the highest current session %.
# Echoes the chosen payload on success; empty + non-zero return if no
# config dir yielded one this cycle.
build_active_payload() {
    POLL_SEQ=$((POLL_SEQ + 1))

    local -a dirs
    mapfile -t dirs < <(read_config_dirs)

    local -A cycle_payload cycle_s
    local dir token payload s
    for dir in "${dirs[@]}"; do
        token=$(read_token_for "$dir")
        if [ -z "$token" ]; then
            log "No token in $dir; skipping"
            continue
        fi
        payload=$(build_payload_for_token "$token") || { log "API call failed for $dir"; continue; }
        [ -z "$payload" ] && continue
        s=$(_payload_session_pct "$payload"); s=${s:-0}
        cycle_payload["$dir"]="$payload"
        cycle_s["$dir"]="$s"
        # A rise in session % since the previous poll means this plan was just used.
        if [ -n "${PREV_S[$dir]:-}" ] && (( s > PREV_S[$dir] )); then
            LAST_ACTIVE["$dir"]=$POLL_SEQ
        fi
        PREV_S["$dir"]="$s"
    done

    if [ ${#cycle_payload[@]} -eq 0 ]; then
        log "No usable config dir this cycle"
        return 1
    fi

    # Pick the active dir: most recent activity wins; ties (and the no-activity
    # startup case) broken by highest current session %.
    local best_dir="" best_active=-1 best_s=-1 a
    for dir in "${!cycle_payload[@]}"; do
        a=${LAST_ACTIVE[$dir]:-0}
        s=${cycle_s[$dir]}
        if (( a > best_active )) || (( a == best_active && s > best_s )); then
            best_active=$a; best_s=$s; best_dir=$dir
        fi
    done

    if [ ${#dirs[@]} -gt 1 ]; then
        log "Active plan: $best_dir (s=$best_s)"
    fi
    printf '%s' "${cycle_payload[$best_dir]}"
    return 0
}

# Poll and send over BLE — thin wrapper kept for the BLE loop's call site.
poll_ble() {
    local payload
    payload=$(build_active_payload) || return 1
    log "Sending: $payload"
    write_gatt "$RX_CHAR_PATH" "$payload" || { log "Write failed"; return 1; }
    return 0
}

cleanup() {
    stop_notify_subscriber
    stop_sessions_sidecar
    log "Daemon stopped"
    exit 0
}

trap cleanup INT TERM

log "=== Claude Usage Tracker Daemon (BLE or USB serial) ==="
log "Poll interval: ${POLL_INTERVAL}s"

start_sessions_sidecar

BACKOFF=1

while true; do
    # Prefer USB serial: no BLE pairing needed, and this runs at the top of
    # every reconnect cycle, so plugging the board in while running on BLE
    # picks it up the next time this loop comes back around (worst case,
    # one poll cycle later — see the mid-session check below).
    SERIAL_PORT=$(find_serial_port)
    if [ -n "$SERIAL_PORT" ]; then
        log "Clawdmeter found on $SERIAL_PORT (USB serial)"
        BACKOFF=1
        LAST_POLL=0
        LAST_SESSIONS_SIG=""  # resend current sessions to this freshly-seen device
        while [ -e "$SERIAL_PORT" ]; do
            NOW=$(date +%s)
            if (( NOW - LAST_POLL >= POLL_INTERVAL )); then
                payload=$(build_active_payload) && {
                    log "Sending via serial: $payload"
                    if write_serial "$SERIAL_PORT" "$payload"; then
                        LAST_POLL=$NOW
                    else
                        log "Serial write failed, reconnecting..."
                        break
                    fi
                }
            fi
            # No refresh-request equivalent on this transport (see
            # check_serial_cmd() in main.cpp) — plain interval polling.
            maybe_send_sessions_serial "$SERIAL_PORT"
            start_sessions_sidecar   # no-op if already running; respawns if it crashed
            sleep "$TICK"
        done
        log "Serial device gone, reconnecting..."
        sleep 2
        continue
    fi

    # --- BLE fallback ---------------------------------------------------
    # Find the device: only a device the system already knows (paired/connected).
    # We never scan by name, so we can't grab a stranger's or the wrong nearby
    # unit. Pair the device once first (it's a bonded BLE HID keyboard anyway).
    if ! load_mac; then
        find_system_device_mac || {
            log "No USB serial and no paired/connected '$DEVICE_NAME'; waiting ${BACKOFF}s (not scanning)..."
            sleep "$BACKOFF"
            BACKOFF=$((BACKOFF < 60 ? BACKOFF * 2 : 60))
            continue
        }
    fi

    # Connect if not connected
    if ! is_connected; then
        connect_device || {
            log "Retrying in ${BACKOFF}s..."
            sleep "$BACKOFF"
            BACKOFF=$((BACKOFF < 60 ? BACKOFF * 2 : 60))
            continue
        }
    fi

    # Find the GATT characteristic
    RX_CHAR_PATH=$(find_char_path_by_uuid "$RX_CHAR_UUID")
    if [ -z "$RX_CHAR_PATH" ]; then
        log "Error: RX characteristic not found, retrying..."
        sleep 5
        continue
    fi
    log "GATT RX path: $RX_CHAR_PATH"

    # Characteristic paths change across reconnects; re-resolve SS lazily and
    # resend the current session payload to the freshly connected device.
    SS_CHAR_PATH=""
    LAST_SESSIONS_SIG=""

    BACKOFF=1  # reset backoff on successful connection

    start_notify_subscriber

    # Poll loop: tick every $TICK seconds. Poll Anthropic when the
    # interval has elapsed OR when the ESP requested a refresh.
    LAST_POLL=0
    while is_connected; do
        NOW=$(date +%s)
        if [ -f "$REFRESH_FLAG" ] || (( NOW - LAST_POLL >= POLL_INTERVAL )); then
            # Checked here (once per poll, not every $TICK) rather than in the
            # inner loop's condition — find_serial_port() round-trips an
            # `identify` to every candidate port, which isn't worth paying
            # every 5s just to catch a cable being plugged in slightly sooner.
            if [ -n "$(find_serial_port)" ]; then
                log "USB serial now available, switching over"
                break
            fi
            if [ -f "$REFRESH_FLAG" ]; then
                log "Refresh requested by device"
                rm -f "$REFRESH_FLAG"
            fi
            poll_ble && LAST_POLL=$NOW
        fi
        maybe_send_sessions
        start_sessions_sidecar   # no-op if already running; respawns if it crashed
        sleep "$TICK"
    done

    stop_notify_subscriber
    log "Device disconnected, reconnecting..."
    sleep 2
done
