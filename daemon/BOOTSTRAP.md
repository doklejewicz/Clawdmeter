# Bootstrap the daemon from an already-flashed device

If you have a Clawdmeter device flashed with `get-setup` support but a
*machine* that doesn't have this repo cloned yet, the device can hand you
`install.sh` and everything it needs directly over the serial connection —
no network access, no git, nothing beyond bash + coreutils (`stty`, `cat`,
`head`, `tail`) already on virtually any Linux box.

This only works if the firmware you flashed has the setup bundle loaded onto
its SPIFFS partition — see "Building/updating the bundle" below. It's a
separate step from the normal firmware flash, so a board flashed before this
feature existed (or before the bundle was last rebuilt) will report
`SETUP_UNSUPPORTED`.

## Running it

Get `tools/get_setup.sh` onto the target machine by whatever means you have
handy (`scp`, a thumb drive, pasting it into an editor) — it's a standalone
script and doesn't need the rest of the repo. Find your port first if
unsure (`ls /dev/ttyUSB* /dev/ttyACM*`, or check `dmesg` after plugging in),
then:

```bash
./get_setup.sh [/dev/ttyUSB0]
```

It puts the port in raw mode, asks the device for the bundle, verifies the
transfer against the device's own MD5 before running anything (retrying up
to 6 times — this link is a little lossy at 115200 baud with no flow
control, so an occasional failed attempt is expected), and runs the result.
`setup.sh` unpacks itself (it's a small shell header with a tar.gz appended
after it — see `tools/setup_stub.sh`) and hands off to `install.sh`.

**If the board isn't actually connected at that path when you run this**,
the script's `[ -c "$PORT" ]` check catches it and fails loudly ("not a
character device"). Without that check, opening the path for read-write
would silently *create a plain file* there instead of failing — bash's `<>`
redirection implies O_CREAT — permanently clobbering that path until the
stray file is removed and the board replugged. If you're seeing
`stty: ...: Inappropriate ioctl for device` right now, that's exactly what
already happened: `sudo rm /dev/ttyUSB0` (or whatever path), replug the
board, and confirm `ls -l` shows a character device (`crw-...`) before
retrying.

## What's in the bundle

`install.sh`, `daemon/claude-usage-daemon.sh`,
`daemon/claude-usage-daemon.service`, `daemon/clawdmeter_sessions.py`, and
`daemon/SESSIONS.md` — everything `install.sh` needs, with the same relative
layout it expects (`install.sh` at the top, `daemon/` alongside it).

## Building/updating the bundle

The bundle is a separate flash step from the normal firmware upload, and
isn't rebuilt automatically — only needed when the files it contains change:

```bash
./tools/build_setup_bundle.sh                    # writes firmware/data/setup.sh
pio run -d firmware -e <env> -t uploadfs         # flashes it to the SPIFFS partition
```

`uploadfs` and the normal app upload (`-t upload`) are independent — flashing
new firmware doesn't touch SPIFFS, and vice versa. A fresh board needs both
at least once for `get-setup` to have anything to serve.
