#!/bin/bash
# Build the 16-bit DOS door (anetmrc.exe) with OpenWatcom.

# Stop on any error, unset variable, or failed pipe stage.
set -euo pipefail

# Output directory for the .exe and an intermediate directory for .obj files.
OUTDIR="build"; OBJDIR="$OUTDIR/obj"; mkdir -p "$OUTDIR" "$OBJDIR"

# Locate the OpenWatcom install. $WATCOM overrides; otherwise a sensible default.
OW_REL="${WATCOM:-/mnt/hdd2/open/rel}"; export WATCOM="$OW_REL"

# Put the 64-bit host tools on PATH (wcc, wlink) — Linux x86_64 host.
if [ -d "$WATCOM/binl64" ]; then export PATH="$WATCOM/binl64:$PATH"; fi
# Fallback for 32-bit host installations.
if [ -d "$WATCOM/binl" ]; then export PATH="$WATCOM/binl:$PATH"; fi

# Assemble the INCLUDE path (semicolon-separated, Watcom convention) by
# picking up whichever header dirs the install actually has.
inc_list=""
for d in "$WATCOM/h" "$WATCOM/h/dos" "$WATCOM/h/os2" "$WATCOM/h/nt"; do [ -d "$d" ] || continue; if [ -z "$inc_list" ]; then inc_list="$d"; else inc_list="$inc_list;$d"; fi; done
export INCLUDE="$inc_list"

# Translation units that make up the door. Order is not significant; wlink merges.
SRCS=(main.c fossil.c dropfile.c bridge.c); OBJS=()

# Compile each .c to a .obj. Flags:
#   -bt=dos  target DOS
#   -mc     compact memory model (small code, large data — fits 64K DGROUP)
#   -os     optimize for size (binary has to live within DOS memory)
#   -5      target 80586 (Pentium) instructions
for src in "${SRCS[@]}"; do obj="$OBJDIR/$(basename "${src%.c}").obj"; wcc -bt=dos -mc -os -5 -fo="$obj" "$src"; OBJS+=("$obj"); done

# Generate the Watcom linker script on the fly.
LNKFILE="$OUTDIR/anetirc_dosdoor.lnk"

# system dos          — emit a DOS MZ executable
# name ...            — output path for anetmrc.exe
# option map=...      — produce a link map alongside the exe (useful for DGROUP debugging)
# option stack=32k    — give the door a generous stack (nested slash-command handlers)
# file <obj>          — one per translation unit
{ echo "system dos"; echo "name $OUTDIR/anetmrc.exe"; echo "option map=$OUTDIR/anetirc_dosdoor.map"; echo "option stack=32k"; for obj in "${OBJS[@]}"; do echo "file $obj"; done; } > "$LNKFILE"

# Invoke the linker with the generated script.
wlink @"$LNKFILE"

# Report the result to the caller.
echo "Built: $OUTDIR/anetmrc.exe"
