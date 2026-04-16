# ANETMRC — Installation Guide

This guide walks you through: **JUMP TO STEP 2 IF YOU ARE NOT BUILDING FROM SOURCE**

1. Building the DOS door (`anetmrc.exe`) and the Win32 helper
   (`anetmrc_bridge.exe` + `config.exe`)
2. Configuring the BBS identity
3. Installing ANETMRC into your BBS as a door
4. Running single-node vs. multi-node setups
5. Sanity testing

The build scripts target **Linux + mingw32** (for the helper) and
**Linux + OpenWatcom 2.0** (for the DOS door). Both toolchains also run on
Windows if you prefer.

---

## 1. Prerequisites

### One-time: install the toolchains

**OpenWatcom 2.0** — for the 16-bit DOS door.

- Download from <https://github.com/open-watcom/open-watcom-v2/releases>
- Install; add `$WATCOM/binl64` (Linux) or `%WATCOM%\binnt64` (Windows) to
  your `PATH`.
- Make sure these env vars are exported: `WATCOM`, `INCLUDE`, `EDPATH`,
  `WIPFC`.

Verify:
```sh
wcc -h | head -1        # should print "Open Watcom C x86 16-bit..."
```

**mingw32 (i686-w64-mingw32-gcc)** — for the Win32 helper.

- Linux: `apt install mingw-w64` (Debian/Ubuntu) or `dnf install
  mingw32-gcc-c++` (Fedora).
- Windows: MSYS2 with `mingw-w64-i686-gcc`.

Verify:
```sh
i686-w64-mingw32-gcc --version
```

### A FOSSIL driver on the BBS host

The door speaks serial through INT 14h via a FOSSIL driver. Any of these
work:

- **X00** — classic, widely tested
- **BNU** — small and fast (It's what I use)
- **ADF / SIO** — multi-line
- **NetFoss** — a modern FOSSIL-over-telnet wrapper for Windows-hosted
  BBSes

Install and load whichever you already use for other doors.

---

## 2. Building

Clone or unpack the source into a working directory. Layout:

```
/path/to/anetmrc/
├── dosdoor/              ← the 16-bit door
├── helper_win32/         ← the Win32 bridge + config tool
├── MRC-DEV-DOCS.TXT      ← protocol reference
├── README.md
└── INSTALL.md
```

### Build the DOS door

```sh
cd dosdoor
bash build_fossil_dos.sh
```

Output: `dosdoor/build/anetmrc.exe` (~60 KB, single-binary door).

### Build the Win32 helper + config utility

```sh
cd ../helper_win32
bash helper_build_win32.sh
```

Output:
- `helper_win32/build_helper_win32/anetmrc_bridge.exe` (~200 KB)
- `helper_win32/build_helper_win32/config.exe` (~40 KB)

Both are statically-linked Windows console apps (no runtime DLLs needed
beyond `ws2_32` which ships with Windows).

---

## 3. Configure the BBS identity

**Once** per BBS, run the config utility on the Windows host. It writes
`MRCBBS.DAT` into the current working directory.

```
cd \path\to\anetmrc
config.exe
```

It will prompt for seven fields:

| Field | Example | Required? |
|---|---|---|
| BBS Display Name | `A-Net Online BBS` | Yes |
| BBS Internal Name | `ANET_ONLINE` *(auto-suggested, no spaces)* | Yes |
| SysOp Handle | `StingRay` | Yes |
| BBS Description | `Est. 1994 · fido, mrc, files` | Yes |
| Telnet Address | `a-net.online:1337` | Yes |
| SSH Address | `a-net.online:1338` | Optional |
| Website URL | `https://a-net.online` | Optional |

The file is human-readable (key=value) — you can edit it later with any
text editor or re-run `config.exe` (it loads existing values and lets you
press Enter to keep each).

If `MRCBBS.DAT` is missing or has blank required fields, the bridge
refuses to start.

---

## 4. Deploy the runtime

Create a **single directory** for the runtime. Everything below runs from
it — the bridge, the DOS door, and the MRC config — because they share
state through files in the current working directory.

Suggested layout:

```
C:\BBS\DOORS\ANETMRC\
├── anetmrc.exe              ← from dosdoor/build/
├── anetmrc_bridge.exe       ← from helper_win32/build_helper_win32/
├── config.exe               ← from helper_win32/build_helper_win32/
├── MRCBBS.DAT               ← written by config.exe
├── mrc_banner.ans           ← (optional) ANSI banner for the bridge console * SIZE MATTERS!(79x15) Cannot exceed
└── *.OUT / *.IN / .LOG      ← auto-created at runtime
```

Runtime files auto-generated:

| File | Written by | What it is |
|---|---|---|
| `MRCUSER.DAT` | door | Per-user settings (handle, color, twit list, …) |
| `ANETDOS.OUT` / `ANETDOS[N].OUT` | door | Commands flowing DOS → bridge |
| `ANETDOS.IN` / `ANETDOS[N].IN` | bridge | Lines flowing bridge → DOS |
| `anetmrc_bridge.log` | bridge | Diagnostic log (only in `--verbose` mode) |

---

## 5. Launch the bridge

One bridge process serves **all** of your BBS nodes. Start it once — as a
Windows service, scheduled task, or simply in a console window.

```
anetmrc_bridge.exe
```

With diagnostic logging:

```
anetmrc_bridge.exe --verbose
```

For a legacy single-node host:

```
anetmrc_bridge.exe -node 1     # only serves COM1 / node 0-1
```

The bridge polls every `ANETDOS*.OUT` in the directory every tick. When a
new one appears (i.e., a user just launched the door on a new node), the
bridge picks it up automatically. No restart required when adding nodes.

---

## 6. Wire the door into your BBS

Add ANETMRC to your BBS's door menu using whatever mechanism your BBS
supports. It needs:

- A DOOR.SYS dropfile (standard — most BBSes emit this automatically)
- The current working directory set to the runtime folder
- The user's COM port to be online with a FOSSIL driver loaded

### Example — Spitfire `DOORS.SF` entry

```
;Name       Path                          Parms           Shell  Multi  Prompt
ANETMRC     C:\BBS\DOORS\ANETMRC          anetmrc.exe     N      Y      Y
```

### Example — Synchronet / Mystic / etc.

```
[ANETMRC]
Name       = MRC Chat
Path       = C:\BBS\DOORS\ANETMRC\
CmdLine    = anetmrc.exe --dropfile=C:\path\to\door.sys
NodeInfo   = DOOR.SYS
```

### Example — generic DOS BBS batch file

```bat
@echo off
cd C:\BBS\DOORS\ANETMRC
anetmrc.exe --dropfile=C:\path\to\door.sys
```

Whatever your BBS writes as DOOR.SYS line 1 (typically `COM1:`, `COM2:`, …) becomes the node number. 
The door reads that and picks `ANETDOS[N].OUT/.IN` automatically.

---

## 7. Sysop sanity test (no BBS required)

Run the door locally against your console to verify everything builds and
talks to the bridge:

```
anetmrc.exe --local
```

This bypasses FOSSIL and reads from stdin / writes to stdout. Node 0 is
used (so `ANETDOS.OUT` / `ANETDOS.IN`). With the bridge running in
another window, you should see the handle entry screen, main menu, and be
able to connect and chat.

---

## 8. Optional: bridge-console banner

If you'd like the bridge's console window to show an ANSI banner on
startup, drop a file named `mrc_banner.ans` (CP437 ANSI art) next to
`anetmrc_bridge.exe`. Its absence is non-fatal — the bridge logs a short
note and starts normally. (do not exceed 79x15)

---

## 9. Updating

Re-run the two build scripts and drop the new `.exe` files into your
runtime directory. `MRCBBS.DAT` and `MRCUSER.DAT` are preserved across
upgrades.

If the protocol version changes in a future release, the bridge's
`version_info` field will say so — check `anetmrc_bridge.log` on startup.

---

## 10. Uninstall

Stop the bridge, remove the runtime directory, remove the BBS door
menu entry. Nothing is written outside the runtime folder.
