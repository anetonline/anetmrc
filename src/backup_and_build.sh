#!/bin/bash
# Snapshot the source trees, then build both the DOS door and the Win32 helper.
# Keeps the latest $KEEP rolling snapshots (default 3) so disk use stays bounded.

# Stop on any error, unset variable, or failed pipe stage.
set -euo pipefail

# Project root and where backups accumulate.
ROOT="/mnt/hdd2/AI"
BACKUPS="$ROOT/backups"

# How many snapshots to retain; override with KEEP=N.
KEEP=${KEEP:-3}

# Optional label that gets prefixed onto the backup directory name.
LABEL=${1:-snapshot}

# Timestamp suffix so snapshots are ordered lexicographically by creation time.
TS=$(date +%Y%m%d-%H%M%S)
DEST="$BACKUPS/${LABEL}-${TS}"

# Ensure the backup root exists before writing into it.
mkdir -p "$BACKUPS"

# Announce the snapshot location.
echo "==> Backing up to $DEST"
mkdir -p "$DEST"
# Mirror the DOS door source tree.
cp -r "$ROOT/dosdoor"     "$DEST/"
# Mirror the Win32 helper source tree.
cp -r "$ROOT/helper_win32" "$DEST/"
echo "    OK — dosdoor + helper_win32 saved"

# Enforce a minimum of 2 snapshots so a recovery is always possible.
if [ "$KEEP" -lt 2 ]; then KEEP=2; fi
echo "==> Pruning old backups (keep $KEEP)"
cd "$BACKUPS"
# List snapshots newest-first, skip the first $KEEP, delete the rest.
ls -1dt */ 2>/dev/null | tail -n +$((KEEP + 1)) | while read -r old; do
    echo "    removing $old"
    rm -rf -- "$old"
done
# Show which snapshots survive the prune.
echo "    remaining:"
ls -1dt "$BACKUPS"/*/ 2>/dev/null | while read -r b; do echo "    $(basename "$b")"; done

echo ""
# Run the DOS door build (OpenWatcom). `tail -3` keeps the output short.
echo "==> Building DOS door"
cd "$ROOT/dosdoor"
bash build_fossil_dos.sh | tail -3

echo ""
# Run the Win32 helper + config build (mingw32).
echo "==> Building Win32 helper"
cd "$ROOT/helper_win32"
bash helper_build_win32.sh | tail -3

echo ""
echo "Done."
