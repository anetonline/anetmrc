# ANETMRC — MRC 1.3.6 Chat Client for DOS BBSes


**PLEASE, Report ANY bug/issues/concerns to StingRay @ a-net-online.lol or a-net-online @ proton.me**


**ANETMRC** is a full MRC (Multi-Relay Chat) 1.3 client for any DOS-native BBS
that produces a `DOOR.SYS` dropfile and runs a FOSSIL driver. It lets users on
your BBS join the live MRC chat network (`na-multi.relaychat.net:5000`) and
talk in real-time with users on other connected BBSes worldwide.

The project is a two-process design:

| Component | Runs on | Job |
|---|---|---|
| **`anetmrc.exe`** | 16-bit DOS (OpenWatcom) | The door itself — reads DOOR.SYS, renders the chat UI over the serial line via FOSSIL, captures keys. |
| **`anetmrc_bridge.exe`** | Win32 (mingw32, i686) | Bridge process — holds the TCP connection to the MRC server, translates packets, handles TLS/CTCP. |
| **`config.exe`** | Win32 | One-time configuration utility — writes `MRCBBS.DAT`. |

The two processes talk through per-node text files (`ANETDOS[N].OUT` /
`ANETDOS[N].IN`) so the DOS door never has to touch a socket.

One bridge serves **all** nodes at once — you only launch `anetmrc_bridge.exe`
once on the BBS machine and every node that the sysop boots up will attach to
it automatically.

---

## Features

- **Complete MRC 1.3 implementation** — handshake, CAPABILITIES, USERIP,
  TERMSIZE, BBSMETA, IAMHERE/IMALIVE/PING-PONG, INFO* fields, NOTME, CTCP
  (VERSION / TIME / PING / CLIENTINFO), STATUS (AFK/BACK/LASTSEEN), TRUST,
  IDENTIFY / REGISTER / UPDATE, ROOMPASS / ROOMCONFIG, TOPIC, WHOON,
  CHATTERS, LIST, USERS, CHANNEL, BBSES, MOTD, BANNERS, ROUTING, CHANGELOG,
  TOPICS, USERLIST, INFO N, QUICKSTATS.
- **Multi-node** — a single bridge process serves up to 8 nodes
  simultaneously; each node gets its own pair of bridge files.
- **Per-user settings** — handle, color, prefix/suffix, text color, custom
  enter/leave messages, theme, and a per-user twit (ignore) list, all saved
  to `MRCUSER.DAT` keyed by BBS username.
- **Mentions** — your handle is tracked with case-insensitive word-boundary
  matching; mention counter on row 2 of the chat screen, `/mentions`
  command shows last mentioner.
- **Scrollback** — 40-message ring buffer, Up/Down navigation.
- **Tab autocomplete** — users in the current room are completed from a
  live user list.
- **Themes** — six ANSI themes for the chat rule row.
- **CTCP** — responds to VERSION, TIME, PING, CLIENTINFO; sends arbitrary
  CTCP via `/ctcp`.
- **AFK / BACK** — `/afk [message]` and `/back`; automatic AWAY after 10
  minutes of inactivity.
- **Local mode** — `anetmrc.exe --local` runs the door against stdin/stdout
  for sysop-side testing (no FOSSIL, no COM port).

---

## Architecture

```
    ┌─────────────────────┐          ┌──────────────────────┐
    │  DOS user's term    │          │  MRC network         │
    │  (Telnet / SSH)     │          │  na-multi.relaychat  │
    └──────────┬──────────┘          └───────────┬──────────┘
               │ serial (via                     │ TCP 5000
               │  FOSSIL)                        │
    ┌──────────▼──────────┐          ┌───────────▼──────────┐
    │   anetmrc.exe       │  files   │  anetmrc_bridge.exe  │
    │   (16-bit DOS door) ◄─────────►│  (Win32 helper)      │
    │                     │ ANETDOS* │                      │
    └─────────────────────┘ .OUT/.IN └──────────────────────┘
```

### Bridge files

| File | Writer | Reader | Purpose |
|---|---|---|---|
| `ANETDOS.OUT` (node 0/1) / `ANETDOS2.OUT` … `ANETDOS8.OUT` | DOS door | Win32 helper | Commands going out to MRC |
| `ANETDOS.IN` / `ANETDOS2.IN` … `ANETDOS8.IN` | Win32 helper | DOS door | Lines (pipe-coded ANSI) coming from MRC |

Node number is taken from the DOS door's COM port (COM1 → node 0, COM3 →
node 3, etc.). The helper uses the same mapping automatically — no
configuration required.

---

## Quick Start

*See [INSTALL.md](INSTALL.md) for the full build and setup walk-through.*

```sh
# Build the DOS door
cd dosdoor
bash build_fossil_dos.sh                 # → build/anetmrc.exe

# Build the Win32 helper + config tool
cd ../helper_win32
bash helper_build_win32.sh               # → build_helper_win32/anetmrc_bridge.exe + config.exe

# Configure the BBS identity (one time)
./build_helper_win32/config.exe          # writes MRCBBS.DAT

# Start the bridge on the BBS host
anetmrc_bridge.exe  # listens for any node

# Install anetmrc.exe into your BBS doors menu as usual
```

---

## Command Reference (inside the door)

### Chat & Navigation

| Command | Description |
|---|---|
| `/join <room>` | Switch to another room |
| `/list` | List all rooms on the network |
| `/chatters` | Show users in the current room |
| `/whoon` | Show all users online across the network |
| `/msg <user> <text>` | Private message |
| `/t <user> <text>` | Short form of /msg |
| `/me <action>` | Action / emote |
| `/b <text>` | Broadcast to all rooms |
| `/afk [message]` | Mark yourself AFK |
| `/back` | Clear AFK |
| `/mentions` | Show mention count, reset |
| `/quit` | Leave MRC and return to main menu |

### Appearance

| Command | Description |
|---|---|
| `/color 1-15` | Set handle color |
| `/prefix <text>` | Text before your handle (e.g. `|08[|11`) |
| `/suffix <text>` | Text after your handle |
| `/theme 1-6` | Rule-row theme (gray/blue/green/cyan/red/magenta) |

### Server Commands

| Command | Description |
|---|---|
| `/motd` | Show server MOTD |
| `/time` | Server time |
| `/version` | Server version |
| `/stats` | Server statistics |
| `/banners` | Show server banners |
| `/users` | User count |
| `/channel` | Channel info |
| `/bbses` | Connected BBS list |
| `/info <N>` | Detailed info about connected BBS number N |
| `/topic <text>` | Set current room topic |
| `/last` | Show last-seen of yourself |
| `/lastseen <user>` | Show last-seen of a user |
| `/topics` | List all room topics |
| `/routing` | Show routing info |
| `/changelog` | Server changelog |
| `/roompass <pw>` | Set password on current room (ops only) |
| `/roomconfig [param val]` | Configure current room |
| `/helpserver [topic]` | Ask the MRC server for help |
| `!<cmd>` | Send any server-helper command (`!weather`, `!fortune`, …) |

### Profile, Ignore & CTCP

| Command | Description |
|---|---|
| `/identify <pw>` | Log in to MRC Trust |
| `/register <pw> [email]` | Register with MRC Trust |
| `/update <param> <val>` | Update a trust field |
| `/trust [cmd]` | Manage trust (INFO, etc.) |
| `/twit <handle>` | Add a user to the ignore list (max 10) |
| `/twit` | Show current twit list |
| `/untwit <handle>` | Remove from ignore list |
| `/ctcp <user> VERSION\|TIME\|PING\|CLIENTINFO` | Send CTCP query |
| `/syshelp` | Condensed one-screen summary in chat |

### Keyboard Shortcuts

| Key | Action |
|---|---|
| `Up` / `Down` | Previous / next sent message (history) — or scroll by one line when already scrolled |
| `Home` / `End` | Jump to oldest / newest message |
| `Tab` | Autocomplete handle from users in room |
| `Left` / `Right` | Cycle text color backwards / forwards |
| `ESC` | Return to main menu (or resume live view if scrolled) |

---

## Configuration files

### `MRCBBS.DAT` (BBS-wide, produced by `config.exe`)

Key=value, `#` for comments. Fields:

```
bbs_name=ANET_ONLINE              # internal MRC name, no spaces
bbs_pretty=A-Net Online BBS       # display name (spaces OK)
sysop=StingRay                    # SysOp handle, sent in BBSMETA
description=Your BBS one-liner    # shown via INFO N
telnet=a-net.online:1337          # sent in INFOTEL
ssh=a-net.online:1338             # sent in INFOSSH (optional)
website=https://a-net.online      # sent in INFOWEB (optional)
```

### `MRCUSER.DAT` (per-user, written by the door)

INI-style — one `[BBS_USERNAME]` section per BBS login. Fields:

```
[STINGRAY]
handle=StingRay
handle_color=11
handle_prefix=|08[|12A|14NET|08]
handle_suffix=
text_color=7
enterroom=has joined the room
leaveroom=has left the room
theme=1
twit=Rude1,RudeJR2
```

All fields can be edited live via the settings screen (menu option 2) or by
the matching slash commands (`/color`, `/prefix`, `/suffix`, `/theme`, etc.).

### `DOOR.SYS`

Standard BBS dropfile. Only lines 1 (COM port), 2 (baud), 6 (user name),
and 10 (alias) are consulted; the rest is ignored.

---

## Command-Line Flags

### `anetmrc.exe` (DOS door)

| Flag | Effect |
|---|---|
| *(none)* | Normal door operation — reads DOOR.SYS from CWD |
| `--local` | Sysop test mode — bypass FOSSIL, use stdin/stdout |

### `anetmrc_bridge.exe` (Win32 helper)

| Flag | Effect |
|---|---|
| *(none)* | Auto-detect from DOOR.SYS if present, else multi-node mode |
| `-node N` | Fixed single-node mode for node N (1-8) |
| `--verbose` / `-d` | Enable diagnostic logging to `anetmrc_bridge.log` |

---

## Requirements

### Building
- **OpenWatcom** 2.0 (for the 16-bit DOS door) — any recent build works
- **mingw32** (i686-w64-mingw32-gcc) on Linux or Windows for the Win32 helper
- GNU `bash` to run the two build scripts

### Running
- Any DOS-native BBS that produces a standard `DOOR.SYS` (Spitfire, Mystic,
  Synchronet-DOS, Renegade, Wildcat, MBBS, …)
- A FOSSIL driver (X00, BNU, Silver Xpress, ADF, SIO, …) active on the
  user's COM port
- Windows 7 or newer on the BBS host for the bridge
- Outbound TCP to `na-multi.relaychat.net:5000` by default

---

## Troubleshooting

**"BBS configuration not found (MRCBBS.DAT missing)."**
Run `config.exe` from the same directory as `anetmrc_bridge.exe`.

**"DOS door hangs at 'Connecting to MRC server...'"**
Make sure `anetmrc_bridge.exe` is running and that `ANETDOS[N].OUT`/`.IN`
files are writable by both processes (same directory, no AV lock).

**"Mentions counter never increments"**
Your handle in `MRCUSER.DAT` must match the spelling other users type. The
matcher is case-insensitive but does require a word boundary (so "Sting"
would not match "StingRay" — it has to be the full handle).

**"Chat text wraps below row 24"**
The door assumes an 80×24 ANSI terminal. Use a terminal that honors
`ESC[H` and `ESC[K`. If you're getting CR+LF doubling, your FOSSIL driver
is probably inserting LF — disable its translation.

**Helper console shows "mrc_banner.ans not found (optional)"**
Harmless. Drop any ANSI banner file named `mrc_banner.ans` next to
`anetmrc_bridge.exe` and it'll be shown on the bridge console at startup.
 **(79x15 size limit)**

---

## Credits


- **Thanks to StackFault at The Bottomless Abyss BBS**

https://bbs.bottomlessabyss.net

telnet://bbs.bottomlessabyss.net:2023
ssh://bbs.bottomlessabyss.net:2222
https://status-na-multi.relaychat.net/


- **xbit of X-Bit BBS and 32-Bit BBS** - helping to test MRC (with lots of patience) and keeping Spitfire BBS Alive!
https://x-bit.org
telnet://x-bit.org
ssh://x-bit.org:22222

- **Codefenix of Constructive Chaos BBS**
Codefenix wrote and distributed an amazing MRC client and multiplexer called uMRC.
uMRC is by far one of the smoothest, stylish MRC clients out there to date!
https://conchaos.synchro.net • telnet://conchaos.synchro.net • ssh://conchaos.synchro.net
https://github.com/codefenix-dev/uMRC

- **StingRay / A-Net Online BBS** — design, testing, and dogfooding.
- **The MRC protocol** — Andrew Pamment (u-mrc author) — see
  `MRC-DEV-DOCS.TXT` for the full 1.3 specification.

https://a-net.online
telnet://a-net.online:1337
ssh://a-net.online:1338
---

## License

See [LICENSE](LICENSE).
