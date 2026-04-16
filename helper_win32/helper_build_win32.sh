#!/bin/bash
# Build the Win32 bridge (anetmrc_bridge.exe) and the config utility (config.exe)
# with the mingw32 cross compiler.

# Stop on any error, unset variable, or failed pipe stage.
set -euo pipefail

# Output directory for both .exe artefacts.
OUTDIR="build_helper_win32"
mkdir -p "$OUTDIR"

# 32-bit mingw-w64 toolchain. Override with CC=... if your install uses a different prefix.
CC="${CC:-i686-w64-mingw32-gcc}"

# Build the bridge:
#   -O2          standard optimization
#   -Wall -Wextra show all warnings (the code builds clean under both)
#   -DSECURITY_WIN32  required by Windows SChannel headers (secur32.h)
#   sources       helper_main.c + helper_protocol.c
#   -lws2_32      Winsock 2 (TCP)
#   -lsecur32     SSPI / SChannel (TLS)
#   -lcrypt32     certificate store access (TLS validation)
$CC -O2 -Wall -Wextra -DSECURITY_WIN32 \
  -o "$OUTDIR/anetmrc_bridge.exe" \
  helper_main.c helper_protocol.c \
  -lws2_32 -lsecur32 -lcrypt32

# Report the bridge build.
echo "Built: $OUTDIR/anetmrc_bridge.exe"

# Build the BBS-identity config utility. Standalone, no external libraries needed.
$CC -O2 -Wall -Wextra \
  -o "$OUTDIR/config.exe" \
  config_main.c

# Report the config build.
echo "Built: $OUTDIR/config.exe"
