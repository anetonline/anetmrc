# Changelog

## [1.3.8] - 2026-04-18

### Fixed
- **`--local` colliding with COM1/node 1.**  `--local` was hardcoded to
  `comm_port = 0`, which resolved to `ANETDOS.OUT/IN` — the same files
  COM1 uses.  Running `--local` alongside a live node 1 made the two
  doors share one bridge slot, so each could see the other's traffic
  (including connection state, identify replies, and chat).  `--local`
  now uses a dedicated `ANETDOS0.OUT/IN` pair, and the bridge reuses
  slot 1 (previously dormant) to service it.  The `[N<slot>]` prefix
  has been added to every `helper_log` line to make multi-node logs
  unambiguous.

### Changed
- **Unknown `/command` is forwarded to the MRC server.**  Previously,
  any `/` command the door did not recognise produced
  `ERR Unknown: /foo (try /help)`.  With new server-side helpers being
  added regularly (e.g. `!list`, `!welcome`, `!games`), the door now
  converts unknown `/foo args` to `!foo args` and sends it through the
  bridge; the helper's unknown-`!`-command catch-all already forwards
  to the server as a room message and captures the response via WHOON
  routing.  If the server doesn't know the command, it returns its own
  error.  No client updates needed as the MRC helper command set grows.
- Version strings bumped to `1.3.8`.


## [1.3.7] - 2026-04-17

### Fixed
- **CTCP reply leak to non-target users.**  The MRC relay broadcasts every
  `ctcp_echo_channel` packet to every client listening on that pseudo-room,
  and each client is responsible for filtering by field&nbsp;4 (`to_user`).
  The `[CTCP-REPLY]` display path in `handle_mrc_packet` never enforced
  this gate, so any user on ANETMRC would see CTCP replies directed at
  somebody else on the network.  The fix adds a `to_user == my_nick` check
  before `bridge_write` for both the SERVER-sourced branch and the
  user-to-user branch in `helper_protocol.c`.  The `[CTCP]` request path
  was already correctly gated on `ctcp_target`, so that direction is
  unchanged.

### Changed
- Version strings bumped to `1.3.7` in `helper_common.h`
  (`ANETMRC_VERSION_INFO` sent in the handshake and in CTCP VERSION
  replies), the help-screen banner in `dosdoor/main.c`, and the top-of-file
  banners in `README.TXT`, `INSTALL.TXT`, and `FILE_ID.DIZ`.


## [1.3.6] - 2026-04-16

### Fixed
- **`/chatters` and `/list` mangled join/part notices and `!games` output.**
  When the `chatters_notme_count` or `rooms_notme_count` window was still
  open from a prior `/chatters` / `/list`, unrelated server content
  (join/part broadcasts, `!games` response tables, flags-legend footers)
  was being pushed through the column-extract formatters and losing
  characters.  Added `is_list_flags_legend()` and `looks_like_chatters_row()`
  detectors in `helper_protocol.c`; non-data rows now pass through
  `passthrough_truncate()` (which preserves original spacing up to
  78&nbsp;visible columns) instead of going through the split formatters.
- **Auto-MOTD mis-firing on every MOTD line.**  The trigger previously
  matched any body containing the substring "Welcome to", which the real
  MOTD includes multiple times.  Gate tightened to require both
  "Welcome to" **and** "the room is" so it only matches the genuine
  post-identify join banner.

### Changed
- Version strings bumped to `1.3.6`.


## [1.3.5] - 2026-04-15

Initial packaged release of ANETMRC on the current build infrastructure.
Baseline functionality:

- 16-bit DOS door (`anetmrc.exe`) built with OpenWatcom, compact memory
  model, Pentium target.  Uses FOSSIL serial I/O, reads `DOOR.SYS` for
  COM port and user identity, falls back to `--local` (stdin/stdout) for
  DOSEMU testing.
- Win32 bridge (`anetmrc_bridge.exe`) built with mingw-w64 i686.
  Maintains the TCP connection to an MRC relay
  (`na-multi.relaychat.net:5000` by default) and translates between the
  7-field tilde-separated MRC protocol and the door's pipe-coded
  (`|NN`) file bridge.
- One-time configuration utility (`config.exe`) that writes
  `MRCBBS.DAT` (BBS-wide identity: `bbs_name`, `bbs_pretty`, `sysop`,
  `description`, `telnet`, `ssh`, `website`, MRC server/port,
  `show_motd`).  Per-user state (`handle`, `handle_color`,
  prefix/suffix, `text_color`, enter/leave room messages, theme,
  twit list) is written to `MRCUSER.DAT` by the door.
- MRC 1.3 protocol: handshake, `IAMHERE` every 60&nbsp;s,
  `IMALIVE` only in response to server `PING`, `CAPABILITIES`,
  `USERIP`, `TERMSIZE`, `BBSMETA`, `INFODSC/INFOTEL/INFOSSH/INFOWEB/
  INFOSYS` sent on connect.
- Chat features: `/join`, `/list`, `/chatters`, `/whoon`, `/msg`
  (+ `/t`, `/r` shortcuts), `/me`, `/b` broadcast, `/afk` / `/back`,
  `/mentions`, `/twit` / `/untwit`, `/color`, `/prefix`, `/suffix`,
  `/theme`.  Server commands: `/motd`, `/time`, `/version`, `/stats`,
  `/banners`, `/users`, `/channel`, `/bbses`, `/info N`, `/topic`,
  `/last`, `/lastseen`, `/topics`, `/routing`, `/changelog`,
  `/roompass`, `/roomconfig`, `/helpserver`, `!<command>` for any
  server helper.
- MRC Trust: `/identify`, `/register`, `/update`, `/trust`.
- CTCP (VERSION, TIME, PING, CLIENTINFO) via `ctcp_echo_channel`,
  both request and reply direction.
- Multi-node single-bridge architecture: `node_state_t g_nodes[9]`
  with `g_cur_node` pointer swapping; one bridge process polls every
  `ANETDOS*.OUT` in its CWD and activates node slots on demand.
  Per-node bridge file pairs `ANETDOS[N].OUT/.IN` keyed off the door's
  COM port.
- Scrollback: 100-line ring with pre-wrapped entries, PgUp/PgDn/Home/
  End navigation, new-messages-while-scrolled counter, auto-resume on
  ESC or Enter.
- Mentions: starred lines in the scrollback, pageable `/mentions` log
  (`MRCMENT.LOG`), on-screen counter.  Twit filter suppresses messages
  from handles on the per-user twit list.
- Tab-complete handles from the current room's user list.
- 80&times;24 ANSI screen layout, 6 border themes (gray / blue /
  green / cyan / red / magenta), 15 handle colors, 15 text colors,
  optional prefix/suffix decoration around the handle.
- Password masking for `/identify`, `/register`, `/roompass`,
  `/update password`.  Up-arrow sent-message recall suppresses
  password-bearing commands from history.


## Notes

[1.3.8]: https://github.com/YOURNAME/anetmrc/releases/tag/v1.3.8
[1.3.7]: https://github.com/YOURNAME/anetmrc/releases/tag/v1.3.7
[1.3.6]: https://github.com/YOURNAME/anetmrc/releases/tag/v1.3.6
[1.3.5]: https://github.com/YOURNAME/anetmrc/releases/tag/v1.3.5
