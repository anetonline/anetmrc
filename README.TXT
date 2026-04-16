===============================================================================
                       ANETMRC  v1.3.6
              MRC 1.3 Chat Client for DOS BBSes
                  by StingRay / A-Net Online BBS
===============================================================================

  REPORT BUGS / ISSUES / SUGGESTIONS:
    StingRay@a-net-online.lol    or    a-net-online@proton.me


ABOUT
-----

  ANETMRC is a full MRC (Multi-Relay Chat) 1.3 client for any DOS-native
  BBS that produces a DOOR.SYS dropfile and runs a FOSSIL driver.  It
  lets users on your BBS join the live MRC network and chat in real time
  with users on other BBSes around the world.

  The default MRC server is:
      na-multi.relaychat.net : 5000


TWO-PROCESS DESIGN
------------------

  anetmrc.exe        - 16-bit DOS door.  Reads DOOR.SYS, draws the chat UI
                       on the serial line via FOSSIL, captures keystrokes.

  anetmrc_bridge.exe - Win32 helper (runs on the BBS host machine).  Holds
                       the TCP / TLS connection to the MRC server and
                       translates MRC packets to and from the door.

  config.exe         - Win32 one-time setup tool.  Writes MRCBBS.DAT with
                       your BBS identity (name, sysop, telnet/ssh, etc.).

  The door and bridge talk to each other through plain text files in the
  current working directory.  The door never touches a socket.

  One bridge serves ALL of your BBS nodes at the same time.  You only
  launch anetmrc_bridge.exe once, and every node you start after that
  attaches to it automatically.


ARCHITECTURE
------------

   DOS user terminal                        MRC network
   (telnet / SSH)                           na-multi.relaychat.net:5000
        |                                        |
        |  serial line (via FOSSIL)              |  TCP 5000
        |                                        |
        v                                        v
    +------------------+   bridge files   +------------------------+
    |  anetmrc.exe     |<---------------->|  anetmrc_bridge.exe    |
    |  16-bit DOS door |  ANETDOS*.OUT    |  Win32 TCP helper      |
    |                  |  ANETDOS*.IN     |                        |
    +------------------+                  +------------------------+


BRIDGE FILES
------------

  ANETDOS.OUT     (node 0 / 1)    DOS -> bridge  (commands)
  ANETDOS2.OUT    (node 2)
  ANETDOS3.OUT    (node 3)
  ...
  ANETDOS8.OUT    (node 8)

  ANETDOS.IN      (node 0 / 1)    bridge -> DOS  (chat text)
  ANETDOS2.IN     (node 2)
  ...
  ANETDOS8.IN     (node 8)

  The node number comes from the DOS door's COM port (COM1 = node 0/1,
  COM3 = node 3, and so on).  The bridge uses the same mapping - no
  configuration is required.


FEATURES
--------

  * Complete MRC 1.3 protocol: handshake, CAPABILITIES, USERIP, TERMSIZE,
    BBSMETA, IAMHERE / IMALIVE / PING / PONG, INFO fields, NOTME, CTCP
    (VERSION, TIME, PING, CLIENTINFO), STATUS (AFK, BACK, LASTSEEN),
    TRUST, IDENTIFY / REGISTER / UPDATE, ROOMPASS / ROOMCONFIG, TOPIC,
    WHOON, CHATTERS, LIST, USERS, CHANNEL, BBSES, MOTD, BANNERS,
    ROUTING, CHANGELOG, TOPICS, USERLIST, INFO, QUICKSTATS.

  * Multi-node: one bridge serves up to 8 nodes simultaneously.
    Each node has its own pair of bridge files.

  * Per-user settings kept in MRCUSER.DAT, keyed by BBS username:
    handle, handle color, prefix / suffix, text color, custom
    enter / leave messages, theme, and a 10-name ignore (twit) list.

  * Mentions - a yellow star marks lines in which you were mentioned.
    The "Mntn:" counter on the status row tracks how many new mentions
    have arrived.  The /mentions command opens a dedicated pager
    showing every mention from this session with PgUp / PgDn / Home /
    End navigation.

  * Scrollback - 40-message ring buffer, PgUp / PgDn and Up / Down arrow
    keys step backwards or forwards through the view.  Home / End jump
    to the oldest / newest message.

  * Tab autocomplete - press Tab to complete a handle from the live
    users-in-room list.

  * Six ANSI themes for the chat rule row (gray, blue, green, cyan,
    red, magenta).

  * CTCP - the door responds to VERSION, TIME, PING, CLIENTINFO, and
    sends arbitrary CTCP queries via /ctcp.

  * AFK / BACK - /afk [message] and /back.  Automatic AWAY after 10
    minutes of no input.

  * Local mode - "anetmrc.exe --local" runs the door against stdin /
    stdout for sysop-side testing without a BBS and without FOSSIL.


QUICK START
-----------

  Full build and install walkthrough: INSTALL.TXT

  Short form:

  1. Build the DOS door (if building from source):
        cd dosdoor
        bash build_fossil_dos.sh
              -> build/anetmrc.exe

  2. Build the Win32 helper + config tool:
        cd ../helper_win32
        bash helper_build_win32.sh
              -> build_helper_win32/anetmrc_bridge.exe
              -> build_helper_win32/config.exe

  3. Configure the BBS identity, once:
        config.exe                 (writes MRCBBS.DAT)

  4. Start the bridge on the BBS host:
        anetmrc_bridge.exe         (serves every node)

  5. Add anetmrc.exe to your BBS doors menu the same way you add any
     other DOS door.  No special wrapper is needed.


COMMAND REFERENCE (inside the door)
-----------------------------------

  CHAT & NAVIGATION:

     /join <room>       Switch to another room
     /list              List all rooms on the network
     /chatters          Users in the current room
     /whoon             Everybody online across MRC
     /msg <user> <t>    Private message     (short form: /t)
     /me <action>       Emote / action
     /b <text>          Broadcast to every room
     /afk [message]     Mark yourself AFK
     /back              Clear AFK
     /mentions          Open the mentions pager
     /quit              Return to main menu

  APPEARANCE:

     /color 1-15        Set handle color
     /prefix <text>     Text before your handle (e.g. |08[|11)
     /suffix <text>     Text after your handle
     /theme 1-6         Rule-row theme (gray, blue, green, cyan,
                        red, magenta)

  SERVER COMMANDS:

     /motd              Message of the day
     /time              Server time
     /version           Server version
     /stats             Server statistics
     /banners           Server banner listing
     /users             Active user count
     /channel           Channel / room info
     /bbses             Connected BBSes
     /info <N>          Detail on connected BBS number N
     /topic <text>      Set current room topic
     /last              Your last-seen time
     /lastseen <user>   A user's last-seen time
     /topics            List all room topics
     /routing           Routing info
     /changelog         Server changelog
     /roompass <pw>     Password-protect the current room (ops only)
     /roomconfig        Configure current room
     /helpserver [topic]  Ask the MRC server for help
     !<cmd>             Send any helper command (!weather, !fortune, ...)

  PROFILE, IGNORE, CTCP:

     /identify <pw>              Log in to MRC Trust
     /register <pw> [email]      Register with MRC Trust
     /update <param> <val>       Update a trust field
     /trust [cmd]                Manage trust (INFO, ...)
     /twit <handle>              Add to ignore list (max 10)
     /twit                       Show the ignore list
     /untwit <handle>            Remove from ignore list
     /ctcp <user> VERSION|TIME|PING|CLIENTINFO
                                 Send a CTCP query
     /syshelp                    One-screen help summary

  KEYBOARD SHORTCUTS:

     Up / Down        History of messages you sent
                        (or scroll the view by one line when scrolled)
     PgUp / PgDn      Scroll one page back / forward
     Home / End       Jump to oldest / newest message
     Tab              Autocomplete handle from the users-in-room list
     Left / Right     Cycle text color back / forward
     ESC              Return to main menu
                        (or resume live view if scrolled)


CONFIGURATION FILES
-------------------

  MRCBBS.DAT  (BBS-wide, written by config.exe)

      Key=value, # starts a comment.  Example:

         bbs_name=ANET_ONLINE              # MRC internal name, no spaces
         bbs_pretty=A-Net Online BBS       # Display name (spaces OK)
         sysop=StingRay                    # SysOp handle (sent in BBSMETA)
         description=Your BBS one-liner    # Shown via /info N
         telnet=a-net.online:1337          # Sent in INFOTEL
         ssh=a-net.online:1338             # Sent in INFOSSH (optional)
         website=https://a-net.online      # Sent in INFOWEB (optional)


  MRCUSER.DAT  (per-user, written by the door)

      INI style - one section per BBS login.  Example:

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


  DOOR.SYS   (standard BBS dropfile)

      ANETMRC only reads four lines:
         Line 1   COM port (e.g. COM1:) -> node number
         Line 2   baud rate
         Line 6   user name
         Line 10  alias


COMMAND-LINE FLAGS
------------------

  anetmrc.exe  (DOS door)

     (none)         Normal door operation - reads DOOR.SYS from CWD
     --local        Sysop test mode - bypass FOSSIL, use stdin / stdout


  anetmrc_bridge.exe  (Win32 helper)

     (none)         Auto-detect from DOOR.SYS if present,
                    otherwise multi-node mode
     -node N        Fixed single-node mode for node N (1 - 8)
     --verbose /-d  Diagnostic logging to anetmrc_bridge.log


REQUIREMENTS
------------

  BUILDING (if compiling from source):

     * OpenWatcom 2.0           for the 16-bit DOS door
     * mingw32 (i686-w64-mingw32-gcc)
                                for the Win32 bridge and config tool
     * GNU bash                 to run the two build scripts

  RUNNING:

     * Any DOS-native BBS that writes a standard DOOR.SYS
         (Spitfire, Mystic, Synchronet-DOS, Renegade, Wildcat, MBBS, ...)
     * A FOSSIL driver (X00, BNU, Silver Xpress, ADF, SIO, ...) on the
         user's COM port
     * Windows 7 or newer on the BBS host, for the bridge
     * Outbound TCP to na-multi.relaychat.net : 5000  (default)


TROUBLESHOOTING
---------------

  "BBS configuration not found (MRCBBS.DAT missing)."
     Run config.exe from the same directory as anetmrc_bridge.exe.

  "DOS door hangs at 'Connecting to MRC server...'"
     anetmrc_bridge.exe must be running and both processes must share
     the same directory for the ANETDOS[N].OUT / .IN files.  Make
     sure antivirus is not locking them.

  "Mentions counter never increments"
     Your handle in MRCUSER.DAT must match the spelling other users
     type.  The match is case-insensitive but requires a word boundary,
     so "Sting" will not match "StingRay" - the full handle is needed.

  "Chat text wraps below row 24"
     The door assumes an 80 x 24 ANSI terminal.  If you see CR+LF
     doubling, your FOSSIL driver is inserting an extra LF - disable
     its line translation.

  "mrc_banner.ans not found (optional)"
     Harmless.  Drop any 79 x 15 ANSI banner file named
     mrc_banner.ans next to anetmrc_bridge.exe and it will be shown
     on the bridge console at startup.


CREDITS
-------

  Thanks to StackFault  at  The Bottomless Abyss BBS
     https://bbs.bottomlessabyss.net
     telnet://bbs.bottomlessabyss.net:2023
     ssh://bbs.bottomlessabyss.net:2222
     https://status-na-multi.relaychat.net/

  xbit  of  X-Bit BBS  and  32-Bit BBS
     Helping test MRC (with lots of patience) and keeping Spitfire
     BBS alive!
     https://x-bit.org
     telnet://x-bit.org
     ssh://x-bit.org:22222

  Codefenix  of  Constructive Chaos BBS
     Wrote and distributed uMRC - one of the smoothest and most
     stylish MRC clients out there to date.
     https://conchaos.synchro.net
     telnet://conchaos.synchro.net
     ssh://conchaos.synchro.net
     https://github.com/codefenix-dev/uMRC

  StingRay  /  A-Net Online BBS
     Design, testing, and dogfooding.
     https://a-net.online
     telnet://a-net.online:1337
     ssh://a-net.online:1338

  The MRC protocol
     Andrew Pamment (author of u-mrc).  The full 1.3 specification
     is NOT shipped with this release - it is not needed to build or
     run ANETMRC.  If you want it for protocol-level development, get
     it from the u-mrc project upstream.


LICENSE
-------

  See the LICENSE file.


===============================================================================
                    END OF README - thanks for reading!
===============================================================================
