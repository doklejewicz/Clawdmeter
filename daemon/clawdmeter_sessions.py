#!/usr/bin/env python3
"""Clawdmeter session-awareness sidecar — Claude Code hook listener.

Maintains a live table of open Claude Code sessions (state, context %, todo and
subagent counts) driven by Claude Code's hook events, and projects it onto the
compact wire format the firmware reads from the SS GATT characteristic
(4c41555a-4465-7669-6365-000000000005). See issue #135.

Runs two ways:

- **Standalone (Linux)** — `python3 clawdmeter_sessions.py`, normally spawned
  as a child of `claude-usage-daemon.sh` rather than run directly (see
  daemon/SESSIONS.md). Serves HTTP on 127.0.0.1:<hook_port> and atomically
  writes `~/.claude/clawdmeter-sessions.json` on every state change; the bash
  daemon ships that file's payload over BLE on its existing 5 s tick.
- **Library** — the Python daemon (macOS/Windows) can import `SessionTable`,
  `fit_payload`, etc. and run the listener in-process.

Design rules carried from the issue:

- The listener is a read-only observer: it answers 204 No Content and never
  blocks or approves anything.
- Liveness comes from the session roster (`<config-dir>/sessions/<pid>.json`),
  NOT from activity timeouts — a chat blocked on a permission prompt is silent
  and must survive indefinitely (§4.2). 30 s grace before acting on roster
  absence; 6 h staleness sweep as backstop.
- The current tool is NOT cleared on PostToolUse (cleared on Stop and
  UserPromptSubmit), and `ntools` counts OPEN tool_use_ids, not a running
  total.
- Rows leave the host already sorted: (bucket, -last_event_at).
- Wire codes are append-only; ENDED rows never leave the host.

Python 3 stdlib only — no pip installs required.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---------------------------------------------------------------------------
# Wire constants (§3, §5 of issue #135). State codes are append-only: they
# cross the BLE boundary, so renumbering would desync host/firmware pairs.
# ---------------------------------------------------------------------------

STATE_STARTING = 0
STATE_IDLE = 1
STATE_THINKING = 2
STATE_RESPONDING = 3
STATE_RUNNING_TOOL = 4
STATE_COMPACTING = 5
STATE_WAITING_PERMISSION = 6
STATE_WAITING_QUESTION = 7
STATE_WAITING_INPUT = 8
STATE_ERROR = 9
STATE_ENDED = 10  # never sent to the device

WAITING_STATES = frozenset(
    (STATE_WAITING_PERMISSION, STATE_WAITING_QUESTION, STATE_WAITING_INPUT, STATE_ERROR)
)
WORKING_STATES = frozenset(
    (STATE_THINKING, STATE_RESPONDING, STATE_RUNNING_TOOL, STATE_COMPACTING)
)

MODEL_CODES = (("opus", 1), ("sonnet", 2), ("haiku", 3), ("fable", 4))
EFFORT_CODES = (("low", 1), ("medium", 2), ("high", 3), ("xhigh", 4), ("max", 5))

TOOL_CODES = {
    "Bash": 1,
    "Read": 2,
    "Edit": 3,
    "Write": 4,
    "Grep": 5,
    "Glob": 6,
    "Task": 7,
    "WebFetch": 8,
    "WebSearch": 9,
}

# ---------------------------------------------------------------------------
# Tunables
# ---------------------------------------------------------------------------

DEFAULT_BUDGET_BYTES = 180   # conservative fit target; see sessions_budget_bytes
LABEL_FLOOR = 8              # labels never elide below this many characters
# ASCII on purpose: the firmware's Styrene fonts cover 32..126 only, so a real
# U+2026 renders as tofu on the device. Same UTF-8 byte count (3), so the
# payload byte-budget math is unaffected.
ELLIPSIS = "..."

ROSTER_GRACE_S = 30          # roster absence tolerated this long (first hook may
                             # beat the roster file)
STALE_SWEEP_S = 6 * 3600     # backstop for an unreadable roster
SWEEP_INTERVAL_S = 5

DEFAULT_WINDOW = 200_000     # context window heuristic (§4.3)
ONE_M = 1_000_000

TRANSCRIPT_TAIL_BYTES = 2 * 1024 * 1024
MAX_BODY_BYTES = 8 * 1024 * 1024

CONFIG_FILE = os.path.join(
    os.path.expanduser("~"), ".config", "claude-usage-monitor", "config"
)
DEFAULT_SESSIONS_FILE = os.path.join(
    os.path.expanduser("~"), ".claude", "clawdmeter-sessions.json"
)

# Hook events the state machine consumes. Verified against the Claude Code
# hooks reference (code.claude.com/docs/en/hooks) — every name below is a real
# event. PostToolUseFailure is not in the issue's table but must pop its
# tool_use_id like PostToolUse, or a failed tool leaves ntools stuck.
HOOK_EVENTS = (
    "SessionStart",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "PostToolUseFailure",
    "PermissionRequest",
    "PermissionDenied",
    "Notification",
    "MessageDisplay",
    "Stop",
    "StopFailure",
    "PreCompact",
    "PostCompact",
    "SubagentStart",
    "SubagentStop",
    "SessionEnd",
)
_HANDLED = frozenset(HOOK_EVENTS)


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# ---------------------------------------------------------------------------
# Config file (same file + format as the daemons: `key = value`, # comments)
# ---------------------------------------------------------------------------

def read_config_value(key, path=None):
    """Last-wins `key = value` lookup; trailing comments stripped. None if unset."""
    path = path or CONFIG_FILE
    pattern = re.compile(r"^\s*" + re.escape(key) + r"\s*=\s*(.*)$")
    val = None
    try:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                m = pattern.match(line.rstrip("\r\n"))
                if m:
                    v = re.sub(r"\s*(#.*)?$", "", m.group(1)).strip()
                    val = v
    except OSError:
        return None
    return val or None


def read_config_dirs(path=None):
    """Claude config dirs to consult for rosters/transcripts (default ~/.claude)."""
    raw = read_config_value("config_dirs", path)
    home = os.path.expanduser("~")
    if not raw:
        return [os.path.join(home, ".claude")]
    dirs = []
    for part in raw.split(","):
        part = part.strip()
        if not part:
            continue
        if part == "~":
            part = home
        elif part.startswith("~/"):
            part = os.path.join(home, part[2:])
        dirs.append(part)
    return dirs or [os.path.join(home, ".claude")]


# ---------------------------------------------------------------------------
# Pure helpers (unit-tested)
# ---------------------------------------------------------------------------

def state_bucket(state):
    """§2.2: 0 waiting, 1 working, 2 idle. Sort key is (bucket, -last_event_at)."""
    if state in WAITING_STATES:
        return 0
    if state in WORKING_STATES:
        return 1
    return 2


def model_code(model_str):
    if not model_str:
        return 0
    lowered = model_str.lower()
    for name, code in MODEL_CODES:
        if name in lowered:
            return code
    return 0


def effort_code(effort_str):
    if not effort_str:
        return 0
    lowered = effort_str.lower()
    for name, code in EFFORT_CODES:
        if name == lowered:
            return code
    return 0


def tool_code(tool_name):
    return TOOL_CODES.get(tool_name or "", 0)


PROMPT_LABEL_MAX = 80  # raw capture cap; elide_label narrows further to the wire budget

# The harness weaves context blocks (<ide_opened_file>, <ide_selection>,
# <system-reminder>, ...) directly into the prompt text a UserPromptSubmit
# hook receives — not just editor-side display. Left in, a session opened by
# clicking a file (no typed message at all) shows its label as raw IDE
# plumbing instead of falling through to a real name. Stripped generically by
# tag shape rather than a hardcoded tag list, since the harness can add more.
_TAG_BLOCK_RE = re.compile(r"<([a-zA-Z][\w-]*)\b[^>]*>.*?</\1>", re.DOTALL)


def clean_prompt_label(text):
    """First-user-prompt label: strip injected context tag blocks, collapse
    whitespace to single spaces, and cap length. Returns None for anything
    not worth using (empty/non-string/only-tags)."""
    if not isinstance(text, str):
        return None
    text = _TAG_BLOCK_RE.sub(" ", text)
    cleaned = " ".join(text.split())
    if not cleaned:
        return None
    return cleaned[:PROMPT_LABEL_MAX]


def elide_label(label, max_chars):
    """Middle-elide to max_chars characters, preserving the trailing
    discriminator — `clawdmeter-36` and `clawdmeter-2c` must stay distinct."""
    if len(label) <= max_chars:
        return label
    if max_chars < len(ELLIPSIS):
        return label[:max_chars]
    remaining = max_chars - len(ELLIPSIS)
    tail = (remaining + 1) // 2   # round up: keep the trailing discriminator
    head = remaining - tail
    return label[:head] + ELLIPSIS + label[len(label) - tail:]


def encode_payload(rows):
    return json.dumps({"ss": rows}, separators=(",", ":"), ensure_ascii=False)


def fit_payload(rows, budget):
    """Fit already-sorted rows into `budget` bytes (UTF-8): first shrink labels
    (middle-elide, 8-char floor), then drop rows from the tail — never from the
    front, so a waiting chat is never the one dropped (§5)."""
    def nbytes(s):
        return len(s.encode("utf-8"))

    for n in range(len(rows), -1, -1):
        subset = [list(r) for r in rows[:n]]
        originals = [r[1] for r in subset]
        max_label = max((len(lbl) for lbl in originals), default=LABEL_FLOOR)
        for cap in range(max(max_label, LABEL_FLOOR), LABEL_FLOOR - 1, -1):
            for row, orig in zip(subset, originals):
                row[1] = elide_label(orig, cap)
            payload = encode_payload(subset)
            if nbytes(payload) <= budget:
                return payload
        # Even at the label floor these rows don't fit -> drop the tail row.
    return encode_payload([])


def compute_window(observed_tokens, model_str, pinned_k=None):
    """Context window heuristic (§4.3): 200k default, 1M on a `[1m]` marker,
    snap up to the next 1M multiple if observed usage exceeds the assumption.
    A pinned `context_window_k` is fact, not a guess — no snap-up."""
    if pinned_k:
        return int(pinned_k) * 1000
    window = ONE_M if (model_str and "[1m]" in model_str) else DEFAULT_WINDOW
    if observed_tokens and observed_tokens > window:
        window = ((observed_tokens + ONE_M - 1) // ONE_M) * ONE_M
    return window


def context_percent(observed_tokens, model_str, pinned_k=None):
    """-1 when unknown; otherwise 0..100 against the heuristic window."""
    if observed_tokens is None:
        return -1
    window = compute_window(observed_tokens, model_str, pinned_k)
    if window <= 0:
        return -1
    return max(0, min(100, round(observed_tokens * 100 / window)))


def tokens_k(observed_tokens):
    """Absolute context tokens in 1k units, rounded half-up (deterministic —
    Python's round() would banker-round 160500 down). -1 when unknown."""
    if observed_tokens is None:
        return -1
    return int((observed_tokens + 500) // 1000)


def munge_cwd(cwd):
    """Claude Code's project-dir munging: every non-alphanumeric becomes '-'.
    ('/home/x/JBT Marel/Clawdmeter' -> '-home-x-JBT-Marel-Clawdmeter')"""
    return re.sub(r"[^A-Za-z0-9]", "-", cwd)


def short_sid(session_id):
    """2 hex chars, stable for the session's life. Session ids are UUIDs, so
    the first two chars are already hex; hash as a fallback. A collision only
    degrades the reorder animation (§5) — cosmetic."""
    prefix = session_id[:2].lower()
    if re.fullmatch(r"[0-9a-f]{2}", prefix):
        return prefix
    return hashlib.md5(session_id.encode("utf-8")).hexdigest()[:2]


# ---------------------------------------------------------------------------
# Transcript reading (context %, model) — §4.3
# ---------------------------------------------------------------------------

def read_context_from_transcript(path):
    """Newest non-sidechain assistant record's usage:
    input + cache_read_input + cache_creation_input tokens.
    Returns (tokens, model_str, effort_str), or (None, None, None) when
    unreadable/absent. Reads only the file tail — transcripts grow to many MB.
    "effort" is a top-level field on the same record (reasoning effort: low/
    medium/high/xhigh/max), not nested under "message"."""
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as fh:
            if size > TRANSCRIPT_TAIL_BYTES:
                fh.seek(size - TRANSCRIPT_TAIL_BYTES)
                fh.readline()  # discard the partial line
            data = fh.read()
    except OSError:
        return (None, None, None)

    for line in reversed(data.decode("utf-8", "replace").splitlines()):
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        if not isinstance(rec, dict) or rec.get("type") != "assistant":
            continue
        if rec.get("isSidechain"):
            continue  # subagent turns would inflate the count
        message = rec.get("message")
        if not isinstance(message, dict):
            continue
        usage = message.get("usage")
        if not isinstance(usage, dict):
            continue
        tokens = 0
        for field in ("input_tokens", "cache_read_input_tokens", "cache_creation_input_tokens"):
            v = usage.get(field)
            if isinstance(v, (int, float)):
                tokens += int(v)
        return (tokens, message.get("model"), rec.get("effort"))
    return (None, None, None)


def _user_message_text(message):
    """Human-authored text from a transcript "user" record's message.
    content is either a plain string or a list of blocks; a turn that's
    purely a tool_result (the harness's own reply to a tool call, not
    something typed) has no text blocks and yields ""."""
    if not isinstance(message, dict):
        return ""
    content = message.get("content")
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = [b.get("text", "") for b in content
                 if isinstance(b, dict) and b.get("type") == "text"]
        return " ".join(p for p in parts if p)
    return ""


def read_first_prompt_from_transcript(path):
    """The session's first non-sidechain "user" turn, read from the START of
    the file forward — unlike every other reader here, which wants the
    newest record. A resumed old session's first hook of the CURRENT daemon
    process's lifetime is not the session's first prompt ever; without this,
    resuming a past chat (or the sidecar restarting mid-conversation) makes
    whatever gets typed next masquerade as "the first prompt" instead of
    whatever actually opened the chat, possibly weeks earlier.

    Only the first turn is ever considered, usable or not (mirrors the
    live-hook path's stickiness — see Session.first_prompt_seen): a noisy
    first turn must not let a later turn claim the label either.

    Returns None if no user turn exists yet at all (transcript missing/empty
    — the session may still be genuinely brand new, so the caller must NOT
    treat this as "seen" and should keep waiting for the live hook); ""
    if a first turn exists but cleans to nothing (pure harness noise);
    otherwise the cleaned text."""
    try:
        fh = open(path, "r", encoding="utf-8", errors="replace")
    except OSError:
        return None
    with fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            if not isinstance(rec, dict) or rec.get("type") != "user":
                continue
            if rec.get("isSidechain"):
                continue
            return clean_prompt_label(_user_message_text(rec.get("message"))) or ""
    return None


def read_custom_title_from_transcript(path):
    """Newest "custom-title" record's title - the name the user set by
    renaming the chat in the editor's session list. The harness re-emits
    this event repeatedly (not just once), so a tail read reliably picks up
    the latest one without needing to scan the whole file. Returns None
    when unreadable/absent/blank."""
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as fh:
            if size > TRANSCRIPT_TAIL_BYTES:
                fh.seek(size - TRANSCRIPT_TAIL_BYTES)
                fh.readline()  # discard the partial line
            data = fh.read()
    except OSError:
        return None

    for line in reversed(data.decode("utf-8", "replace").splitlines()):
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        if not isinstance(rec, dict) or rec.get("type") != "custom-title":
            continue
        title = clean_prompt_label(rec.get("customTitle"))
        if title:
            return title
    return None


# ---------------------------------------------------------------------------
# Session roster (liveness + names) — §4.2
# ---------------------------------------------------------------------------

def _proc_starttime(pid):
    """Field 22 (starttime) of /proc/<pid>/stat, or None if the pid is gone.
    comm (field 2) may contain spaces/parens, so split after the last ')'."""
    try:
        with open(f"/proc/{pid}/stat", "rb") as fh:
            stat = fh.read().decode("ascii", "replace")
        rest = stat[stat.rindex(")") + 2:].split()
        return rest[19]
    except (OSError, ValueError, IndexError):
        return None


def pid_alive(pid, proc_start=None):
    """Is this roster entry's process still running? Roster files can outlive a
    crashed process, so presence alone isn't liveness. The roster records
    procStart (jiffies, /proc/<pid>/stat field 22) precisely so pid reuse can
    be told apart from the original process."""
    try:
        pid = int(pid)
    except (TypeError, ValueError):
        return False
    if pid <= 0:
        return False
    if os.path.isdir("/proc"):
        start = _proc_starttime(pid)
        if start is None:
            return False
        if proc_start is not None and str(proc_start) != start:
            return False  # pid was reused by another process
        return True
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError:
        return False


def load_roster(config_dirs):
    """Union of `<dir>/sessions/*.json` across config dirs, keyed by sessionId.
    Returns (roster, readable). readable=False means no roster dir could be
    listed at all — liveness must not be enforced then (§9: sessions linger,
    nothing disappears wrongly)."""
    roster = {}
    readable = False
    for cdir in config_dirs:
        sdir = os.path.join(cdir, "sessions")
        try:
            names = os.listdir(sdir)
        except OSError:
            continue
        readable = True
        for fn in names:
            if not fn.endswith(".json"):
                continue
            try:
                with open(os.path.join(sdir, fn), encoding="utf-8") as fh:
                    rec = json.load(fh)
            except (OSError, ValueError):
                continue
            if isinstance(rec, dict) and rec.get("sessionId"):
                roster[rec["sessionId"]] = rec
    return roster, readable


# ---------------------------------------------------------------------------
# The session table + state machine (§4.1)
# ---------------------------------------------------------------------------

class Session:
    __slots__ = (
        "session_id", "sid", "state", "state_since", "last_event_at",
        "roster_name", "cwd", "transcript_path", "current_tool", "open_tools",
        "nagents", "tdone", "ttotal", "ctx", "tok", "model", "effort", "missing_since",
        "first_prompt", "first_prompt_seen", "custom_title",
    )

    def __init__(self, session_id, now):
        self.session_id = session_id
        self.sid = short_sid(session_id)
        self.state = STATE_STARTING
        self.state_since = now
        self.last_event_at = now
        self.roster_name = None
        self.cwd = None
        self.transcript_path = None
        self.current_tool = None   # last tool NAME; survives PostToolUse
        self.open_tools = []       # OPEN tool_use_ids — concurrent, not cumulative
        self.nagents = 0
        self.tdone = 0
        self.ttotal = 0
        self.ctx = -1
        self.tok = -1  # context tokens in 1k units; -1 whenever ctx is -1
        self.model = 0
        self.effort = 0  # reasoning effort at the last transcript read; session_effort_t
        self.missing_since = None  # first time the roster didn't vouch for us
        # First user prompt, cleaned + capped (see _clean_prompt_label) - a
        # more useful label than the host's auto-derived "<dir>-xx" name, at
        # the cost of prompt text becoming device-visible (opt-in tradeoff,
        # see SESSIONS.md's privacy note).
        self.first_prompt = None
        # Set the instant the first UserPromptSubmit lands, independent of
        # whether it produced a usable first_prompt. Without this, a first
        # turn that's pure harness noise (e.g. a session opened by clicking a
        # file, no typed text — clean_prompt_label returns None) would leave
        # first_prompt None and let a LATER turn's text ("do it again") claim
        # the "first prompt" label instead — a mid-conversation aside standing
        # in for a name, not the first turn's own words.
        self.first_prompt_seen = False
        # User-set chat title (renamed in the editor's session list), read
        # from the transcript's "custom-title" events - see
        # read_custom_title_from_transcript(). Takes priority over
        # first_prompt: an intentional rename beats an inferred label.
        self.custom_title = None

    def label(self):
        if self.custom_title:
            return self.custom_title
        if self.first_prompt:
            return self.first_prompt
        if self.roster_name:
            return self.roster_name
        if self.cwd:
            base = os.path.basename(self.cwd.rstrip("/"))
            if base:
                return base
        return self.session_id[:8]


class SessionTable:
    """Hook-driven state machine over the live sessions. Thread-safe."""

    def __init__(self, pinned_window_k=None, config_dirs=None, now_fn=time.time):
        self.pinned_window_k = pinned_window_k
        self.config_dirs = config_dirs if config_dirs is not None else read_config_dirs()
        self.now_fn = now_fn
        self.sessions = {}  # session_id -> Session
        self._lock = threading.RLock()

    # -- event intake -------------------------------------------------------

    def handle_event(self, payload):
        """Apply one hook payload. Returns True if the table changed.
        Unknown events and malformed payloads are ignored gracefully."""
        if not isinstance(payload, dict):
            return False
        event = payload.get("hook_event_name")
        session_id = payload.get("session_id")
        if event not in _HANDLED or not isinstance(session_id, str) or not session_id:
            return False
        now = self.now_fn()
        with self._lock:
            if event == "SessionEnd":
                # Dropped immediately; ENDED rows never leave the host.
                return self.sessions.pop(session_id, None) is not None

            sess = self.sessions.get(session_id)
            if sess is None:
                sess = Session(session_id, now)
                self.sessions[session_id] = sess

            tp = payload.get("transcript_path")
            if isinstance(tp, str) and tp:
                sess.transcript_path = tp
            cwd = payload.get("cwd")
            if isinstance(cwd, str) and cwd:
                sess.cwd = cwd

            sess.last_event_at = now
            sess.missing_since = None  # a hook is proof of life
            self._apply(sess, event, payload, now)
            return True

    def _set_state(self, sess, state, now):
        if sess.state != state:
            sess.state = state
            sess.state_since = now

    def _apply(self, sess, event, payload, now):
        if event == "SessionStart":
            self._set_state(sess, STATE_IDLE, now)
            self._refresh_context(sess)

        elif event == "UserPromptSubmit":
            # A new turn: the previous turn's tool is genuinely over.
            sess.current_tool = None
            sess.open_tools.clear()
            self._set_state(sess, STATE_THINKING, now)
            if not sess.first_prompt_seen:
                sess.first_prompt_seen = True
                cleaned = clean_prompt_label(payload.get("prompt"))
                if cleaned:
                    sess.first_prompt = cleaned

        elif event == "PreToolUse":
            tool = payload.get("tool_name")
            tuid = payload.get("tool_use_id")
            if tuid is None or tuid not in sess.open_tools:
                sess.open_tools.append(tuid)
            if isinstance(tool, str) and tool:
                sess.current_tool = tool
            if tool == "AskUserQuestion":
                self._set_state(sess, STATE_WAITING_QUESTION, now)
            else:
                self._set_state(sess, STATE_RUNNING_TOOL, now)

        elif event in ("PostToolUse", "PostToolUseFailure"):
            tuid = payload.get("tool_use_id")
            if tuid is not None and tuid in sess.open_tools:
                sess.open_tools.remove(tuid)
            elif tuid is None and sess.open_tools:
                sess.open_tools.pop()
            elif None in sess.open_tools:
                sess.open_tools.remove(None)
            # NOTE: sess.current_tool is deliberately NOT cleared here — between
            # two tools in one turn the state line would flicker (§4.1). It is
            # cleared on Stop and UserPromptSubmit.
            if event == "PostToolUse" and payload.get("tool_name") == "TodoWrite":
                todos = (payload.get("tool_input") or {}).get("todos")
                if isinstance(todos, list):
                    sess.ttotal = len(todos)
                    sess.tdone = sum(
                        1 for t in todos
                        if isinstance(t, dict) and t.get("status") == "completed"
                    )
            # Only tool-driven states advance here; a session waiting on a
            # permission prompt (or compacting) must not be clobbered by an
            # unrelated tool completing.
            if sess.state in (STATE_RUNNING_TOOL, STATE_WAITING_QUESTION):
                self._set_state(
                    sess,
                    STATE_RUNNING_TOOL if sess.open_tools else STATE_THINKING,
                    now,
                )

        elif event == "PermissionRequest":
            # AskUserQuestion also fires a PermissionRequest for its own
            # approval prompt - keep the more specific WAITING_QUESTION
            # rather than downgrading to the generic WAITING_PERMISSION.
            if sess.current_tool == "AskUserQuestion":
                self._set_state(sess, STATE_WAITING_QUESTION, now)
            else:
                self._set_state(sess, STATE_WAITING_PERMISSION, now)

        elif event == "PermissionDenied":
            self._set_state(sess, STATE_THINKING, now)

        elif event == "Notification":
            ntype = payload.get("notification_type")
            if ntype == "permission_prompt":
                if sess.current_tool == "AskUserQuestion":
                    self._set_state(sess, STATE_WAITING_QUESTION, now)
                else:
                    self._set_state(sess, STATE_WAITING_PERMISSION, now)
            elif ntype in ("agent_needs_input", "elicitation_dialog"):
                self._set_state(sess, STATE_WAITING_INPUT, now)
            elif ntype in ("idle_prompt", "agent_completed"):
                self._set_state(sess, STATE_IDLE, now)
            # other notification types (auth_success, ...) carry no state

        elif event == "MessageDisplay":
            self._set_state(sess, STATE_RESPONDING, now)

        elif event == "Stop":
            sess.current_tool = None
            sess.open_tools.clear()
            self._set_state(sess, STATE_IDLE, now)
            self._refresh_context(sess)

        elif event == "StopFailure":
            self._set_state(sess, STATE_ERROR, now)

        elif event == "PreCompact":
            self._set_state(sess, STATE_COMPACTING, now)

        elif event == "PostCompact":
            self._set_state(sess, STATE_IDLE, now)
            self._refresh_context(sess)

        elif event == "SubagentStart":
            sess.nagents += 1

        elif event == "SubagentStop":
            sess.nagents = max(0, sess.nagents - 1)

    # -- context (§4.3): hook-driven re-reads, never polled ------------------

    def _refresh_context(self, sess):
        path = sess.transcript_path or self._guess_transcript(sess)
        if not path:
            return
        # Backfill from history on the first call we can (normally
        # SessionStart): a resumed chat, or a session the daemon only just
        # started watching (sidecar restart mid-conversation), already has a
        # real first turn on disk that a live UserPromptSubmit would
        # otherwise never surface. None here means no user turn exists yet
        # (still genuinely new) — leave first_prompt_seen False so the live
        # hook gets its normal chance.
        if not sess.first_prompt_seen:
            found = read_first_prompt_from_transcript(path)
            if found is not None:
                sess.first_prompt_seen = True
                if found:
                    sess.first_prompt = found
        tokens, model_str, effort_str = read_context_from_transcript(path)
        if model_str:
            sess.model = model_code(model_str)
        if effort_str:
            sess.effort = effort_code(effort_str)
        sess.ctx = context_percent(tokens, model_str, self.pinned_window_k)
        # tok mirrors the SAME read: the same token sum in 1k units, not divided
        # by the window. Forced to -1 whenever ctx is -1 so the pair can never
        # disagree on the wire.
        sess.tok = tokens_k(tokens) if sess.ctx != -1 else -1
        title = read_custom_title_from_transcript(path)
        if title:
            sess.custom_title = title

    def _guess_transcript(self, sess):
        """Fallback when no hook carried transcript_path:
        <config-dir>/projects/<munged-cwd>/<session-id>.jsonl"""
        if not sess.cwd:
            return None
        munged = munge_cwd(sess.cwd)
        for cdir in self.config_dirs:
            cand = os.path.join(cdir, "projects", munged, sess.session_id + ".jsonl")
            if os.path.isfile(cand):
                return cand
        return None

    # -- liveness sweep (§4.2) ------------------------------------------------

    def sweep(self):
        """Roster-based liveness + discovery + 6 h staleness backstop + label
        refresh. Returns True if anything wire-visible changed."""
        now = self.now_fn()
        changed = False
        with self._lock:
            roster, readable = load_roster(self.config_dirs)

            for session_id, sess in list(self.sessions.items()):
                if now - sess.last_event_at > STALE_SWEEP_S:
                    del self.sessions[session_id]
                    changed = True
                    continue
                if not readable:
                    # Roster unreadable: liveness can't be confirmed either way.
                    # Sessions linger (6 h backstop above); nothing disappears
                    # wrongly.
                    continue
                rec = roster.get(session_id)
                if rec is not None and pid_alive(rec.get("pid"), rec.get("procStart")):
                    sess.missing_since = None
                    name = rec.get("name")
                    if isinstance(name, str) and name and name != sess.roster_name:
                        sess.roster_name = name
                        changed = True
                else:
                    if sess.missing_since is None:
                        sess.missing_since = now  # grace starts; not wire-visible
                    elif now - sess.missing_since > ROSTER_GRACE_S:
                        del self.sessions[session_id]
                        changed = True

            # Discovery: a roster entry with no session_id in our table yet
            # is a chat the daemon has never seen a hook from — either it was
            # already open before this process started, or it's been sitting
            # untouched ever since (nothing to react to). Without this it
            # stays invisible until it does something; every other session
            # gets backfilled from history at SessionStart (_refresh_context),
            # so give these the same treatment here instead of waiting.
            if readable:
                for session_id, rec in roster.items():
                    if session_id in self.sessions:
                        continue
                    if not pid_alive(rec.get("pid"), rec.get("procStart")):
                        continue
                    sess = Session(session_id, now)
                    name = rec.get("name")
                    if isinstance(name, str) and name:
                        sess.roster_name = name
                    cwd = rec.get("cwd")
                    if isinstance(cwd, str) and cwd:
                        sess.cwd = cwd
                    self._set_state(sess, STATE_IDLE, now)
                    self._refresh_context(sess)
                    self.sessions[session_id] = sess
                    changed = True
        return changed

    # -- projection (§5) ------------------------------------------------------

    def rows(self):
        """Full-label rows, already sorted: (bucket, -last_event_at).
        Row: [sid, label, state, ctx, elapsed_s, model, tool, ntools,
              nagents, tdone, ttotal, tok, effort] — append-only, like the
        codes."""
        now = self.now_fn()
        with self._lock:
            ordered = sorted(
                self.sessions.values(),
                key=lambda s: (state_bucket(s.state), -s.last_event_at),
            )
            return [
                [
                    s.sid,
                    s.label(),
                    s.state,
                    s.ctx,
                    max(0, int(now - s.state_since)),
                    s.model,
                    tool_code(s.current_tool),
                    len(s.open_tools),
                    s.nagents,
                    s.tdone,
                    s.ttotal,
                    s.tok,
                    s.effort,
                ]
                for s in ordered
            ]

    def project(self, budget=DEFAULT_BUDGET_BYTES):
        return fit_payload(self.rows(), budget)


# ---------------------------------------------------------------------------
# sessions.json handoff (Linux sidecar -> bash daemon)
# ---------------------------------------------------------------------------

def write_sessions_file(path, payload):
    """Atomic write (temp + rename). `payload` is the exact wire string; the
    bash daemon ships it verbatim, so it is stored as a string, not re-encoded."""
    doc = {"ts": round(time.time(), 3), "payload": payload}
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, separators=(",", ":"), ensure_ascii=False)
        fh.write("\n")
    os.replace(tmp, path)


# ---------------------------------------------------------------------------
# HTTP listener — loopback only, read-only observer
# ---------------------------------------------------------------------------

class HookServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, table, sessions_file, budget):
        super().__init__(addr, HookHandler)
        self.table = table
        self.sessions_file = sessions_file
        self.budget = budget
        self._publish_lock = threading.Lock()
        self._last_payload = None

    def publish(self):
        payload = self.table.project(self.budget)
        with self._publish_lock:
            if payload == self._last_payload:
                return
            self._last_payload = payload
            try:
                write_sessions_file(self.sessions_file, payload)
            except OSError as exc:
                log(f"sessions.json write failed: {exc}")


class HookHandler(BaseHTTPRequestHandler):
    server_version = "ClawdmeterSessions/1"
    protocol_version = "HTTP/1.1"

    def _is_loopback(self):
        return self.client_address[0] in ("127.0.0.1", "::1", "::ffff:127.0.0.1")

    def _read_body(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        if length <= 0:
            return b""
        body = b""
        remaining = length
        while remaining > 0:
            chunk = self.rfile.read(min(remaining, 65536))
            if not chunk:
                break
            remaining -= len(chunk)
            if len(body) < MAX_BODY_BYTES:
                body += chunk  # oversize tails are drained but not kept
        return body

    def do_POST(self):
        if not self._is_loopback():
            self.send_error(403)
            return
        body = self._read_body()
        payload = None
        if body:
            try:
                payload = json.loads(body.decode("utf-8", "replace"))
            except ValueError:
                payload = None
        if payload is not None and self.server.table.handle_event(payload):
            self.server.publish()
        # 204 always: a read-only observer never blocks or approves anything.
        self.send_response(204)
        self.end_headers()

    def do_GET(self):
        # Loopback debugging aid: current wire payload.
        if not self._is_loopback():
            self.send_error(403)
            return
        body = self.server.table.project(self.server.budget).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass  # hooks arrive constantly; keep the journal quiet


def _sweeper(server, stop_event):
    while not stop_event.wait(SWEEP_INTERVAL_S):
        try:
            if server.table.sweep():
                server.publish()
        except Exception as exc:  # never let the sweeper die silently
            log(f"sweep error: {exc}")


# ---------------------------------------------------------------------------
# Hook installation (used by install.sh; also runnable by hand — SESSIONS.md)
# ---------------------------------------------------------------------------

def install_hooks(settings_path, url):
    """Idempotently merge the Clawdmeter HTTP hook block into a Claude Code
    settings.json. Existing hooks are preserved; ours is appended per event."""
    settings_path = os.path.expanduser(settings_path)
    try:
        with open(settings_path, encoding="utf-8") as fh:
            settings = json.load(fh)
    except FileNotFoundError:
        settings = {}
    except ValueError:
        print(f"error: {settings_path} is not valid JSON; refusing to modify it",
              file=sys.stderr)
        return 1
    if not isinstance(settings, dict):
        print(f"error: {settings_path} is not a JSON object; refusing to modify it",
              file=sys.stderr)
        return 1

    hooks = settings.setdefault("hooks", {})
    if not isinstance(hooks, dict):
        print(f"error: 'hooks' in {settings_path} is not an object; refusing to modify it",
              file=sys.stderr)
        return 1

    changed = False
    for event in HOOK_EVENTS:
        entries = hooks.setdefault(event, [])
        if not isinstance(entries, list):
            continue
        present = any(
            isinstance(h, dict) and h.get("type") == "http" and h.get("url") == url
            for entry in entries if isinstance(entry, dict)
            for h in (entry.get("hooks") or [])
        )
        if not present:
            entries.append(
                {"hooks": [{"type": "http", "url": url, "async": True, "timeout": 5}]}
            )
            changed = True

    if not changed:
        print(f"Clawdmeter session hooks already present in {settings_path}")
        return 0

    if os.path.exists(settings_path):
        shutil.copy2(settings_path, settings_path + ".clawdmeter-backup")
    directory = os.path.dirname(settings_path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    tmp = settings_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(settings, fh, indent=2)
        fh.write("\n")
    os.replace(tmp, settings_path)
    print(f"Installed Clawdmeter session hooks into {settings_path}")
    return 0


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Clawdmeter session-awareness sidecar (Claude Code hook listener)"
    )
    parser.add_argument("--port", type=int, default=None,
                        help="listen port (default: hook_port from the daemon config; "
                             "0 = OS-assigned)")
    parser.add_argument("--config", default=None,
                        help=f"daemon config file (default: {CONFIG_FILE})")
    parser.add_argument("--sessions-file", default=None,
                        help=f"handoff file (default: {DEFAULT_SESSIONS_FILE})")
    parser.add_argument("--budget", type=int, default=None,
                        help="payload byte budget (default: sessions_budget_bytes "
                             f"from config, else {DEFAULT_BUDGET_BYTES})")
    parser.add_argument("--install-hooks", nargs=2, metavar=("SETTINGS_JSON", "URL"),
                        help="merge the hook block into a Claude Code settings.json "
                             "and exit")
    args = parser.parse_args(argv)

    if args.install_hooks:
        return install_hooks(*args.install_hooks)

    config_path = args.config or CONFIG_FILE

    port = args.port
    if port is None:
        raw = read_config_value("hook_port", config_path)
        if raw is None:
            log("hook_port is not set in the config — live session awareness is off. "
                f"Set it in {config_path} to enable. Exiting.")
            return 0
        try:
            port = int(raw)
        except ValueError:
            log(f"hook_port '{raw}' is not a number. Exiting.")
            return 1

    pinned_k = None
    raw = read_config_value("context_window_k", config_path)
    if raw:
        try:
            pinned_k = int(raw)
        except ValueError:
            log(f"ignoring non-numeric context_window_k '{raw}' (heuristic stays on)")

    budget = args.budget
    if budget is None:
        raw = read_config_value("sessions_budget_bytes", config_path)
        try:
            budget = int(raw) if raw else DEFAULT_BUDGET_BYTES
        except ValueError:
            budget = DEFAULT_BUDGET_BYTES

    sessions_file = args.sessions_file or DEFAULT_SESSIONS_FILE
    table = SessionTable(pinned_window_k=pinned_k, config_dirs=read_config_dirs(config_path))

    try:
        server = HookServer(("127.0.0.1", port), table, sessions_file, budget)
    except OSError as exc:
        log(f"cannot bind 127.0.0.1:{port}: {exc}")
        return 1

    bound_port = server.server_address[1]
    log(f"listening on http://127.0.0.1:{bound_port}/ "
        f"(budget {budget} bytes -> {sessions_file})")
    server.publish()  # start from a clean, current file (clears stale sessions)

    stop_event = threading.Event()
    sweeper = threading.Thread(target=_sweeper, args=(server, stop_event), daemon=True)
    sweeper.start()

    def _shutdown(signum, frame):
        log("shutting down")
        stop_event.set()
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT, _shutdown)

    try:
        server.serve_forever(poll_interval=0.5)
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
