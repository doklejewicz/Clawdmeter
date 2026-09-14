# Live session awareness — host setup

Shows your open Claude Code chats on the device: what each one is doing, how
full its context window is, todo/subagent counts — and, most importantly,
whether any of them is waiting on you. Design and rationale live in issue
[#135](https://github.com/HermannBjorgvin/Clawdmeter/issues/135).

**Off by default.** The session data comes from a Claude Code hook integration
you have to install; without it the device behaves exactly as it does today.

## How it works

```
claude-usage-daemon.sh (bash, one process — spawns the sidecar as a child)
│
├── clawdmeter_sessions.py (child) ◀──HTTP hooks (127.0.0.1)── Claude Code sessions
│      state machine + sort        ◀────context tokens──────── transcripts *.jsonl
│              │                   ◀───────liveness──────────── <config-dir>/sessions/<pid>.json
│              ▼ atomic write on change
│   ~/.claude/clawdmeter-sessions.json
│              │ 5 s tick, on change
└── BLE GATT SS …0005 ─────────────────────────────────────────▶ Clawdmeter firmware
```

The sidecar (`daemon/clawdmeter_sessions.py`, Python 3 stdlib only) listens on
loopback for hook POSTs, keeps a table of live sessions, sorts it
attention-first, fits it to a byte budget, and writes the finished wire payload
to `~/.claude/clawdmeter-sessions.json`. The bash daemon ships that payload to
the SS GATT characteristic whenever the file's content changes. The listener
is a read-only observer: it answers `204 No Content` and can never block a
tool call or approve a permission.

**One process on Linux.** `claude-usage-daemon.sh` spawns the sidecar as a
child (`start_sessions_sidecar()`) whenever `hook_port` is configured, and
kills it on exit — shipping session data over BLE is pointless if this daemon
isn't running to do the shipping, so there's no separate systemd unit to
start/stop/enable. A per-tick health check respawns the sidecar if it crashes
without restarting the whole daemon (and its BLE connection) over it. The
sidecar is still a standalone, independently-runnable script (see "Run the
sidecar" below) — useful for local debugging or the future macOS/Windows
integration, which will supervise it differently (those hosts' own daemons are
already Python; see "macOS / Windows" below).

## Setup (Linux)

`./install.sh` prompts for all of this (default yes). Manually:

1. **Config** — in `~/.config/claude-usage-monitor/config`:

   ```ini
   hook_port = 45999
   # context_window_k =          # optional: pin the context window (kilotokens)
   # sessions_budget_bytes = 400 # optional: payload byte budget
   ```

2. **Hook block** — merge into `~/.claude/settings.json` (and any other Claude
   config dir you use). The helper is idempotent and backs up the file first:

   ```bash
   python3 daemon/clawdmeter_sessions.py --install-hooks ~/.claude/settings.json http://127.0.0.1:45999/
   ```

   Or add it by hand — this is the exact block:

   ```json
   {
     "hooks": {
       "SessionStart":       [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "UserPromptSubmit":   [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "PreToolUse":         [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "PostToolUse":        [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "PostToolUseFailure": [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "PermissionRequest":  [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "PermissionDenied":   [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "Notification":       [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "MessageDisplay":     [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "Stop":               [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "StopFailure":        [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "PreCompact":         [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "PostCompact":        [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "SubagentStart":      [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "SubagentStop":       [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }],
       "SessionEnd":         [{ "hooks": [{ "type": "http", "url": "http://127.0.0.1:45999/", "async": true, "timeout": 5 }] }]
     }
   }
   ```

   If your settings.json already has hooks, keep them — Clawdmeter's entries
   are appended alongside, not instead. New sessions pick hooks up on start;
   already-running sessions keep their old hook set.

3. **Run it** — nothing extra to start. `claude-usage-daemon.sh` spawns the
   sidecar itself once `hook_port` is set, so the existing
   `claude-usage-daemon` systemd unit (`./install.sh`) covers both:

   ```bash
   systemctl --user restart claude-usage-daemon   # picks up a newly-set hook_port
   journalctl --user -u claude-usage-daemon -f    # sidecar logs to the same stream
   ```

   For local debugging, the sidecar still runs standalone in a terminal:
   `python3 daemon/clawdmeter_sessions.py`

4. **Verify** — open a Claude Code session, then:

   ```bash
   curl -s http://127.0.0.1:45999/                    # current wire payload (loopback debug)
   cat ~/.claude/clawdmeter-sessions.json             # what the daemon will ship
   ```

## Wire format

The payload is `{"ss":[...]}` with one positional row per session, already
sorted attention-first (waiting, working, idle; most recent first within each):

```
[sid, label, state, ctx, elapsed_s, model, tool, ntools, nagents, tdone, ttotal, tok, effort]
```

| # | Field | Meaning |
| - | --- | --- |
| 0 | `sid` | 2 hex chars, stable for the session's life (keys the reorder animation) |
| 1 | `label` | Display name, already middle-elided to fit the budget |
| 2 | `state` | State code 0–10 (issue #135 §3; append-only) |
| 3 | `ctx` | Context window used, percent; `-1` = unknown (firmware hides the bar) |
| 4 | `elapsed_s` | Seconds in the current state at write time |
| 5 | `model` | `0` unknown, `1` opus, `2` sonnet, `3` haiku, `4` fable |
| 6 | `tool` | `0` other/none, `1` Bash, `2` Read, `3` Edit, `4` Write, `5` Grep, `6` Glob, `7` Task, `8` WebFetch, `9` WebSearch |
| 7 | `ntools` | OPEN tool calls (concurrent, not cumulative) |
| 8 | `nagents` | Subagents currently in flight |
| 9–10 | `tdone` / `ttotal` | Todo counts; badge hidden when `ttotal` is 0 |
| 11 | `tok` | Context tokens used, in 1k units (rounded to nearest) — the absolute number behind `ctx`, from the same transcript read. `-1` exactly when `ctx` is `-1` |
| 12 | `effort` | Reasoning effort at the last transcript read: `0` unknown, `1` low, `2` medium, `3` high, `4` xhigh, `5` max. Read from the transcript's top-level `effort` field on the newest assistant record (sibling of `message`, not nested under it) |

Fields are append-only: firmware ignores indices it doesn't know, and new
fields only ever go on the end.

## Config reference

| Key | Default | Meaning |
| --- | --- | --- |
| `hook_port` | unset | Loopback port for the hook listener. **Unset = feature off** — the sidecar exits, the daemon sends nothing. |
| `context_window_k` | unset | Pin the context window in kilotokens (e.g. `200`, `1000`). Blank = heuristic: 200k default, 1M on a `[1m]` model marker, snap up to the next 1M multiple when observed usage exceeds the assumption. A pinned value disables the snap-up. |
| `sessions_budget_bytes` | `400` | Byte budget for the fitted payload. Labels truncate (prefix + "...") down to an 8-char floor first, then the least-urgent rows drop from the tail. Keep below the BLE MTU the device negotiates (the firmware requests 517; 400 leaves headroom under that). |
| `hook_trust_bind` | unset | Comma-separated `ip/prefixlen` entries (e.g. `172.18.0.1/16`, a Docker bridge gateway + subnet). Each gets its own extra hook listener bound to that ip, trusting peers from that CIDR — for a devcontainer on its own bridge network (not `network_mode: host`) that can't reach `127.0.0.1` on the host. Narrower than `--network=host`: only that specific subnet is trusted, not "any process on the host network." See "Reaching the sidecar without `--network=host`" below. |

The sidecar also honors `config_dirs` (shared with the daemons) to find session
rosters and transcripts across several Claude config dirs. Entries may be a
glob pattern (e.g. `~/.claude*`) instead of a literal path — it's expanded
against the filesystem and filtered to directories that exist right now, so
`~/.claude*` picks up `~/.claude-clientA`/`~/.claude-clientB` alongside
`~/.claude` without listing each by name. An unmatched pattern contributes
nothing (never a literal, un-expanded string), and duplicates across entries
are dropped.

## Docker / devcontainer sessions

Claude Code running inside a container can show up here too, with **no
compose/devcontainer changes required** for the common case — just a bind
mount:

1. **Visible config.** Bind-mount a Claude config dir into the container at
   its normal `~/.claude` path, and add that dir (from the *host's* path to
   it) to `config_dirs`.
2. **Liveness just works**, even without `--pid=host`. `pid_alive()` first
   tries a direct `/proc/<pid>/stat` match (the host-process case); if that
   fails, it falls back to scanning `/proc/*/status`' `NSpid` line — which
   lists a process's pid at every namespace nesting level, host first,
   innermost (container's own view) last — for a host pid whose innermost
   entry matches the roster's recorded pid *and* whose `starttime` (field 22,
   jiffies since boot — a real kernel value, not virtualized per namespace)
   matches the roster's `procStart`. That pair is enough to uniquely identify
   the real host-side process without sharing a PID namespace at all.
3. **Hook connectivity still needs a real network path** — a container's
   `127.0.0.1` is its own loopback, not the host's, so hooks (`PreToolUse`,
   `Stop`, etc. — the *live* per-event detail: current tool, todo counts)
   won't reach the sidecar over a plain bridge network. Either run the
   container with `--network=host` (Linux-only; simplest, but see the
   firewall-script caveat below), or use `hook_trust_bind` (below) to trust
   one specific Docker subnet without full host networking.

Example `devcontainer.json` (bind mount only — no `runArgs` needed for
liveness; `--network=host` is only needed for hook delivery, see below):

```jsonc
{
  "mounts": [
    "source=${localEnv:HOME}/.claude,target=${containerEnv:HOME}/.claude,type=bind"
  ]
}
```

**Keep separate projects on separate config dirs.** Bind-mounting the same
`~/.claude` into every container merges their credentials, rosters, and
transcripts into one pool — fine for "my personal projects," not fine for
"client A's container shouldn't see client B's session history." Give each
project (or client) its own dedicated dir instead — `~/.claude-clientA`,
`~/.claude-clientB` — each with its own separate login, mounted only into
that project's own container(s). Isolation happens at the mount (one dir per
project, never shared across containers you want kept apart); aggregation
happens at the daemon (list every dir you personally want visible in your
own `config_dirs` — that's a read-only view on your own device, not a merge
of the underlying data):

```ini
config_dirs = ~/.claude, ~/.claude-clientA, ~/.claude-clientB
```

or, equivalently, with a wildcard that also picks up any future `~/.claude-*`
dir without an edit:

```ini
config_dirs = ~/.claude*
```

Running `claude` directly (no container) against a specific dir works the
same way, just simpler — no mount needed:

```bash
CLAUDE_CONFIG_DIR=~/.claude-clientA claude
```

### Reaching the sidecar without `--network=host`

`--network=host` isn't always an option — a devcontainer may run a
`postStartCommand` that manages its **own** iptables (an egress-allowlist
firewall script, say), and `--network=host` would then point that script at
the *host's* real firewall instead of an isolated container namespace, since
it no longer has one. In that case, keep the container on its normal bridge
network and use `hook_trust_bind` instead:

1. Find the bridge network's gateway: `docker network inspect <network> --format '{{json .IPAM.Config}}'` (e.g. `{"Subnet":"172.18.0.0/16","Gateway":"172.18.0.1"}`).
2. Add `hook_trust_bind = 172.18.0.1/16` to the daemon config — this binds an
   *additional* listener on that gateway ip, trusting only peers from that
   subnet, alongside the always-on `127.0.0.1`-only listener host sessions use.
   No `runArgs`/compose changes needed — liveness already works without
   `--pid=host` (above), and this doesn't touch networking on the container
   side either, just adds a second listener on the host.
3. Point that container's own `settings.json` hook URLs at the gateway ip
   instead of `127.0.0.1` (e.g. `http://172.18.0.1:<hook_port>/`) — a config
   dir used exclusively by one container can safely hardcode this, since
   nothing else reads that `settings.json`.

This trusts an entire Docker subnet rather than one PID, so still prefer
`--network=host` when nothing else on that network namespace matters and
no `postStartCommand` there manages the network's own firewall rules.

## Notes

- **Privacy/security.** Hook payloads contain prompt and response text, so the
  listener binds `127.0.0.1` only and rejects non-loopback peers, by default.
  `hook_trust_bind` (see above) is an opt-in, per-entry exception to that —
  each configured subnet gets its own extra listener and is trusted just like
  loopback, so only add subnets you actually trust (a Docker network that's
  only your own containers, not something shared with untrusted tenants). The
  one exception to "no payload text reaches the device" at all: each session's label,
  in priority order, is (1) a custom title you set by renaming the chat in
  the editor's session list, (2) an `ai-title` — a short summary the editor
  generates on its own as the conversation develops, refined over time (e.g.
  "Test for second session" → "ui.cpp test for second session") — nobody
  typed this, but it's far more useful than the raw first message, (3) the
  first user prompt (cleaned + capped at 80 chars, see `clean_prompt_label()`),
  (4) a non-generic roster name if the host ever supplies one directly, (5)
  the host's generic "`<dir>-xx`" name, (6) the raw session id. Neither a
  rename nor an ai-title is stored in the hook payloads or the session
  roster — the editor writes them as `custom-title`/`ai-title` events
  directly into the transcript file, both re-emitted repeatedly, so a tail
  read (same `read_context_from_transcript` window) reliably picks up the
  latest wording on the next `SessionStart`/`Stop`/`PostCompact` hook.
  Whichever of these wins, it's visible to anyone near the device's screen.
  A session with none of the above and no context data yet (a window opened
  but never used) is filtered from the payload entirely rather than shown as
  a bare directory name — see `Session.is_placeholder()`. Everything else
  (state, counts, model, tool) stays metadata-only.
- **Liveness.** Sessions are considered alive while their roster entry
  (`<config-dir>/sessions/<pid>.json`) points at a running process — not on an
  activity timeout, so a chat parked on a permission prompt survives
  indefinitely. Roster absence is acted on after a 30 s grace; a 6 h staleness
  sweep backstops an unreadable roster.
- **Discovery.** New sessions normally show up the instant their first hook
  fires. The periodic sweep (every 5 s) also scans the roster for entries
  the daemon hasn't seen a hook from at all yet — a chat already open before
  the sidecar started, or one sitting untouched ever since — and adopts any
  with a live pid, backfilling state from history the same way `SessionStart`
  does. Without this, such a session stays invisible until it does something.
- **Context % is a heuristic** read from the transcript tail
  (`input_tokens + cache_read_input_tokens + cache_creation_input_tokens`),
  re-read on SessionStart/Stop/PostCompact. Good for a glanceable bar, not for
  quoting numbers.
- **macOS / Windows** — the Python daemons don't ship session data yet. The
  sidecar is importable as a library (`SessionTable`, `fit_payload`, …) for
  that integration; this round wires up Linux only.
- **Firmware support** — the device needs firmware with the SS characteristic
  (`…0005`) and a board with the session-views capability. Older firmware just
  never sees the data; the daemon stays silent about it.
