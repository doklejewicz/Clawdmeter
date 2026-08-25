#!/bin/bash
# Clawdmeter self-extracting setup bundle — see daemon/BOOTSTRAP.md.
# Everything from the __ARCHIVE_BELOW__ line onward is a raw tar.gz appended
# by tools/build_setup_bundle.sh; this header just unpacks and runs it.
# Safe to read before running — this text is where the readable part ends;
# past the marker it's binary.
set -e
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
ARCHIVE_LINE=$(awk '/^__ARCHIVE_BELOW__$/{print NR + 1; exit}' "$0")
tail -n +"$ARCHIVE_LINE" "$0" | tar xz -C "$TMPDIR"
cd "$TMPDIR"
exec ./install.sh
__ARCHIVE_BELOW__
