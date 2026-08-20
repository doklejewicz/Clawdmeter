#!/bin/bash
# Recovery script for a stuck Clawdmeter BLE bond/connection on Linux.
#
# BlueZ occasionally gets a device wedged after a firmware reflash (stale
# GATT attribute cache from before a characteristic was added/removed) or
# after enough rapid connect/disconnect churn (hung `bluetoothctl connect`
# processes, adapter internal state confusion). On a machine with more than
# one Bluetooth controller there's a second, nastier failure mode: `select`
# only pins a controller for the CURRENT bluetoothctl session — it is not a
# persistent system setting — so unless every session explicitly re-selects
# the controller you want, commands silently fall back to whatever BlueZ
# considers "default" right now, which can be a different, powered-off, or
# never-bonded controller. `bluetoothctl list`'s "[default]" tag is not
# trustworthy evidence of what a *fresh* session will actually use.
#
# This script always explicitly selects a controller (the first POWERED one,
# by default) inside every bluetoothctl session it opens, and re-pairs with
# an agent that stays alive for the whole handshake (letting the agent's
# bluetoothctl session exit mid-pairing is what causes "Accept pairing
# (yes/no)" to go unanswered and the pairing to silently fail).
#
# Usage: ./reset-ble.sh [MAC] [CONTROLLER_MAC]
#   MAC defaults to auto-discovery by device name via a scan.
#   CONTROLLER_MAC defaults to the first powered controller found.
#   If you have multiple controllers and pairing still won't hold even
#   right after this script reports Bonded, block the one you don't want:
#     rfkill list bluetooth   # find its index
#     rfkill block <index>    # no root needed for a soft block
#
# Dependencies: bluetoothctl

set -euo pipefail

DEVICE_NAME="Clawdmeter"
MAC="${1:-}"
CTRL="${2:-}"
MAC_RE='([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}'

# bluetoothctl's own prompt becomes "[<DeviceName>]> " once it focuses a
# device, so every subsequent output line can end up containing the device
# name too - a naive `grep "$DEVICE_NAME" | awk '{print $2}'` then matches
# prompt noise instead of the actual "Device <mac> <name>" listing line.
# Strip ANSI/CR noise, require the literal shape, extract by pattern.
extract_device_mac() {
    sed -r 's/\x1B\[[0-9;]*[a-zA-Z]//g' | tr -d '\r' \
        | grep -E "Device ${MAC_RE} .*${DEVICE_NAME}" \
        | grep -oE "$MAC_RE" | head -1
}

log() {
    echo "[$(date '+%H:%M:%S')] $1"
}

# --- 1. Kill anything stuck ------------------------------------------------
log "Killing any hung 'bluetoothctl connect' processes..."
pkill -f "bluetoothctl connect" 2>/dev/null || true

# --- 1b. Pick a controller ---------------------------------------------------
# Every controller BlueZ knows about, in order; pick the first one that's
# actually powered (an rfkill-blocked one shows Powered: no here).
if [ -z "$CTRL" ]; then
    while read -r c; do
        [ -z "$c" ] && continue
        if bluetoothctl show "$c" 2>/dev/null | grep -q "Powered: yes"; then
            CTRL="$c"
            break
        fi
    done < <(bluetoothctl list 2>/dev/null | awk '{print $2}')
    if [ -z "$CTRL" ]; then
        log "Error: no powered Bluetooth controller found."
        exit 1
    fi
fi
N_CTRL=$(bluetoothctl list 2>/dev/null | grep -c '^Controller' || true)
log "Using controller $CTRL${N_CTRL:+ (of $N_CTRL detected)}"
if [ "${N_CTRL:-0}" -gt 1 ]; then
    log "Note: multiple controllers present. This script explicitly selects"
    log "  $CTRL in every session below, but anything else that shells out to"
    log "  bluetoothctl (your own commands, other tools) will fall back to"
    log "  whatever BlueZ considers default right now unless it does the same."
fi

# --- 2. Find the MAC if not given ------------------------------------------
if [ -z "$MAC" ]; then
    log "No MAC given — scanning for '$DEVICE_NAME'..."
    {
        echo "select $CTRL"
        echo "scan on"
        sleep 8
        echo "quit"
    } | bluetoothctl >/dev/null 2>&1 || true
    MAC=$(bluetoothctl devices 2>/dev/null | extract_device_mac)
    if [ -z "$MAC" ]; then
        log "Error: couldn't find '$DEVICE_NAME' — is it powered on and advertising?"
        exit 1
    fi
    log "Found $MAC"
fi

# --- 3. Power-cycle the selected controller ---------------------------------
# Clears stale internal connection/state confusion without needing root
# (no `systemctl restart bluetooth` — that needs sudo).
log "Power-cycling $CTRL..."
{
    echo "select $CTRL"
    echo "power off"
    sleep 2
    echo "power on"
    sleep 2
    echo "quit"
} | bluetoothctl >/dev/null 2>&1 || true

# --- 4/5. Drop the stale bond and re-pair fresh, retrying a few times ------
# The agent must stay registered for the ENTIRE handshake — piping commands
# through a bluetoothctl session that exits (via stdin EOF) right after
# `pair` deregisters the agent mid-negotiation, and any "Accept pairing
# (yes/no)" prompt that appears after that point never gets answered. The
# `sleep`s here keep the producer side (and therefore the bluetoothctl
# session reading it) alive long enough to cover both the pairing handshake
# and a possible authorization prompt. `select` is repeated as the first
# command of this same session since it doesn't persist across sessions.
#
# Pairing itself is flaky in practice (ConnectionAttemptFailed, or a
# bonded=0 link that the firmware then drops) even with all of the above
# right — a few automatic attempts saves re-running this script by hand.
MAX_ATTEMPTS=4
BONDED=0
for attempt in $(seq 1 "$MAX_ATTEMPTS"); do
    log "Pairing attempt $attempt/$MAX_ATTEMPTS..."
    {
        echo "select $CTRL"
        echo "remove $MAC"
    } | bluetoothctl >/dev/null 2>&1 || true
    {
        echo "select $CTRL"
        echo "scan on"
        sleep 8
        echo "scan off"
    } | bluetoothctl >/dev/null 2>&1 || true
    {
        echo "select $CTRL"
        echo "agent NoInputNoOutput"
        echo "default-agent"
        echo "pair $MAC"
        sleep 5
        echo "yes"          # answers "Accept pairing (yes/no)" if it appears
        sleep 5
        echo "trust $MAC"
        sleep 1
        echo "quit"
    } | bluetoothctl >/dev/null 2>&1 || true

    sleep 1
    STATE=$(bluetoothctl info "$MAC" 2>/dev/null || true)
    if echo "$STATE" | grep -q "Bonded: yes"; then
        BONDED=1
        break
    fi
    log "Not bonded yet, retrying..."
    sleep 2
done

# --- 6. Report ---------------------------------------------------------------
echo "$STATE" | grep -E "Name|Paired|Bonded|Trusted|Connected"

if [ "$BONDED" = 1 ]; then
    log "Bonded. Re-run your daemon now."
    exit 0
else
    log "Still not bonded after $MAX_ATTEMPTS attempts — try a physical power-cycle of the board (unplug/replug USB) and run this script again."
    exit 1
fi
