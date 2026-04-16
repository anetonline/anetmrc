/* helper_protocol.c — ANETMRC Win32 bridge/multiplexer connection to the MRC
 * server, protocol encode/decode, and the DOS-door file protocol. */
#include "helper_common.h"
#include "helper_protocol.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <security.h>
#include <schannel.h>

static void bridge_write(const char *fmt, ...);
static void bridge_write_listing(const char *fmt, ...);

/* Console and UI state */
static HANDLE g_hout = NULL;
static int g_helper_row = 17;
static WORD g_cur_attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

/* Logging */
static FILE *g_log = NULL;

/* MRC protocol state */
typedef struct {
    SOCKET sock;
    int connected;
    int use_tls;
    int tls_ready;

    CredHandle cred;
    CtxtHandle ctx;
    int have_cred;
    int have_ctx;
    SecPkgContext_StreamSizes sizes;

    char server[128];
    int port;

    char bbs_name[64];
    char bbs_pretty[64];
    char version_info[64];

    char user[32];
    char nick[32];
    char room[32];
    char last_room[32];

    char recvbuf[8192];
    int recv_used;

    unsigned char tls_raw[32768];
    int tls_raw_used;

    unsigned char plainq[16384];
    int plainq_used;
    int plainq_pos;
} mrc_state_t;

/* Per-node state for simultaneously connected nodes. */
#define MAX_NODES 9
typedef struct {
    int active;              /* 1 = slot is in use */
    int node_num;            /* 0 = default (no suffix), 2-8 = numbered */
    char bridge_out[32];     /* ANETDOS[N].OUT — commands from door */
    char bridge_in[32];      /* ANETDOS[N].IN  — lines to door */

    mrc_state_t mrc;
    int in_room;

    /* routing flags */
    int show_whoon_to_dos;
    int whoon_lines_left;
    int show_chatters_to_dos;
    int chatters_notme_count; /* NOTME lines to pass clean while awaiting CHATTERS USERLIST */
    int rooms_notme_count;    /* NOTME lines to pass clean while awaiting /list WHOON block */
    int show_motd_to_dos;
    int motd_fired;          /* 1 after auto-MOTD fired this session (reset on disconnect) */
    int identify_sent;       /* 1 after door sent IDENTIFY; cleared when USERNICK confirms */

    /* handle style (survive reconnects) */
    int handle_color;
    char handle_prefix[32];
    char handle_suffix[32];
    int text_color;

    /* enter/leave room messages */
    char enterroom_msg[80];
    char leaveroom_msg[80];

    /* last known server:port for QUICKSTATS */
    char last_server[128];
    int  last_port;

    /* IAMHERE timer */
    ULONGLONG last_iamhere;

    /* last user-initiated chat activity (for AWAY detection) */
    ULONGLONG last_cmd_time;
} node_state_t;

static node_state_t  g_nodes[MAX_NODES];
static node_state_t *g_cur_node = &g_nodes[0];  /* set before every per-node call */

/* Per-node accessors through g_cur_node. */
#define g_mrc                  (g_cur_node->mrc)
#define g_in_room              (g_cur_node->in_room)
#define g_show_whoon_to_dos    (g_cur_node->show_whoon_to_dos)
#define g_whoon_lines_left     (g_cur_node->whoon_lines_left)
#define g_show_chatters_to_dos (g_cur_node->show_chatters_to_dos)
#define g_chatters_notme_count (g_cur_node->chatters_notme_count)
#define g_rooms_notme_count    (g_cur_node->rooms_notme_count)
#define g_show_motd_to_dos     (g_cur_node->show_motd_to_dos)
#define g_motd_fired           (g_cur_node->motd_fired)
#define g_identify_sent        (g_cur_node->identify_sent)
#define g_handle_color         (g_cur_node->handle_color)
#define g_handle_prefix        (g_cur_node->handle_prefix)
#define g_handle_suffix        (g_cur_node->handle_suffix)
#define g_text_color           (g_cur_node->text_color)
#define g_enterroom_msg        (g_cur_node->enterroom_msg)
#define g_leaveroom_msg        (g_cur_node->leaveroom_msg)
#define g_last_server          (g_cur_node->last_server)
#define g_last_port            (g_cur_node->last_port)
#define g_last_cmd_time        (g_cur_node->last_cmd_time)
#define g_node_num             (g_cur_node->node_num)
#define g_bridge_out           (g_cur_node->bridge_out)
#define g_bridge_in            (g_cur_node->bridge_in)

/* Forward declarations */
static void safe_copy(char *dst, size_t dstsz, const char *src);
static void mrc_send_message_room(const char *text);
static void mrc_send_message_user(const char *touser, const char *text);
static void helper_console_goto(short x, short y);
static void helper_ui_log(const char *fmt, ...);
static void helper_set_attr(WORD attr);
static void helper_console_clear_below(void);
static void helper_clear_screen(void);
static void helper_clear_line_from_cursor(void);
static void helper_write_ansi(const char *s);
static void helper_apply_sgr_list(const int *vals, int count);

/* ============================================================================
   UTILITY FUNCTIONS
   ============================================================================ */

static WORD ansi_fg_to_attr(int n) {
    static const WORD map[8] = {
        0,
        FOREGROUND_BLUE,
        FOREGROUND_GREEN,
        FOREGROUND_GREEN | FOREGROUND_BLUE,
        FOREGROUND_RED,
        FOREGROUND_RED | FOREGROUND_BLUE,
        FOREGROUND_RED | FOREGROUND_GREEN,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE
    };
    return map[n & 7];
}

static WORD ansi_bg_to_attr(int n) {
    static const WORD map[8] = {
        0,
        BACKGROUND_BLUE,
        BACKGROUND_GREEN,
        BACKGROUND_GREEN | BACKGROUND_BLUE,
        BACKGROUND_RED,
        BACKGROUND_RED | BACKGROUND_BLUE,
        BACKGROUND_RED | BACKGROUND_GREEN,
        BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE
    };
    return map[n & 7];
}

static void helper_set_attr(WORD attr) {
    g_cur_attr = attr;
    SetConsoleTextAttribute(g_hout, g_cur_attr);
}

static void helper_apply_sgr_list(const int *vals, int count) {
    int i;
    WORD fg = g_cur_attr & (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    WORD bg = g_cur_attr & (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE | BACKGROUND_INTENSITY);

    if (count == 0) {
        helper_set_attr(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        return;
    }

    for (i = 0; i < count; ++i) {
        int v = vals[i];
        if (v == 0) {
            fg = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
            bg = 0;
        } else if (v == 1) {
            fg |= FOREGROUND_INTENSITY;
        } else if (v == 22) {
            fg &= ~FOREGROUND_INTENSITY;
        } else if (v >= 30 && v <= 37) {
            fg = ansi_fg_to_attr(v - 30) | (fg & FOREGROUND_INTENSITY);
        } else if (v >= 90 && v <= 97) {
            fg = ansi_fg_to_attr(v - 90) | FOREGROUND_INTENSITY;
        } else if (v >= 40 && v <= 47) {
            bg = ansi_bg_to_attr(v - 40);
        }
    }

    helper_set_attr(fg | bg);
}

static void helper_clear_line_from_cursor(void) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    DWORD written;
    COORD pos;
    if (!GetConsoleScreenBufferInfo(g_hout, &info)) return;
    pos = info.dwCursorPosition;
    FillConsoleOutputCharacterA(g_hout, ' ', info.dwSize.X - pos.X, pos, &written);
    FillConsoleOutputAttribute(g_hout, g_cur_attr, info.dwSize.X - pos.X, pos, &written);
}

static void helper_clear_screen(void) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    DWORD written;
    COORD home = {0, 0};
    if (!GetConsoleScreenBufferInfo(g_hout, &info)) return;
    FillConsoleOutputCharacterA(g_hout, ' ', info.dwSize.X * info.dwSize.Y, home, &written);
    FillConsoleOutputAttribute(g_hout, g_cur_attr, info.dwSize.X * info.dwSize.Y, home, &written);
    helper_console_goto(0, 0);
}

static void helper_write_ansi(const char *s) {
    DWORD written;
    while (*s) {
        if ((unsigned char)s[0] == 27 && s[1] == '[') {
            int vals[16], nvals = 0, cur = 0, have = 0;
            s += 2;
            while (*s && *s != 'm' && *s != 'H' && *s != 'f' && *s != 'J' && *s != 'K') {
                if (*s >= '0' && *s <= '9') { cur = cur * 10 + (*s - '0'); have = 1; }
                else if (*s == ';') { vals[nvals++] = have ? cur : 0; cur = 0; have = 0; }
                ++s;
            }
            if (*s == 'm') {
                if (have || nvals == 0) vals[nvals++] = cur;
                helper_apply_sgr_list(vals, nvals);
                ++s;
                continue;
            }
            if (*s == 'H' || *s == 'f') {
                int row = 1, col = 1;
                COORD c;
                if (nvals >= 1) row = vals[0];
                if (nvals >= 2) col = vals[1];
                c.X = (SHORT)((col > 0 ? col : 1) - 1);
                c.Y = (SHORT)((row > 0 ? row : 1) - 1);
                SetConsoleCursorPosition(g_hout, c);
                ++s;
                continue;
            }
            if (*s == 'J') { helper_clear_screen(); ++s; continue; }
            if (*s == 'K') { helper_clear_line_from_cursor(); ++s; continue; }
        }
        WriteConsoleA(g_hout, s, 1, &written, NULL);
        ++s;
    }
}

static void helper_console_goto(short x, short y) {
    COORD c; c.X = x; c.Y = y;
    SetConsoleCursorPosition(g_hout, c);
}

static void helper_console_clear_below(void) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    DWORD written;
    COORD start;
    int cols, rows, y;
    if (!g_hout) return;
    if (!GetConsoleScreenBufferInfo(g_hout, &info)) return;
    cols = info.dwSize.X;
    rows = info.dwSize.Y;
    for (y = 16; y < rows; ++y) {
        start.X = 0; start.Y = (SHORT)y;
        FillConsoleOutputCharacterA(g_hout, ' ', cols, start, &written);
    }
    helper_console_goto(0, 16);
}

/* Update the status line just below the ANS banner. */
static void helper_status_update(const char *msg) {
    COORD pos;
    DWORD written;
    char buf[128];
    if (!g_hout) return;
    pos.X = 0; pos.Y = 16;
    SetConsoleCursorPosition(g_hout, pos);
    snprintf(buf, sizeof(buf), "Status: %-70s\r\n", msg);
    WriteConsoleA(g_hout, buf, (DWORD)strlen(buf), &written, NULL);
    if (!helper_verbose && g_helper_row <= 18) g_helper_row = 18;
}

static void helper_ui_log(const char *fmt, ...) {
    char buf[1024];
    char line[1032];
    va_list ap;
    DWORD written;
    if (!g_hout) return;
    if (!helper_verbose) return;   /* quiet unless -d/--verbose */
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    snprintf(line, sizeof(line), "%s\r\n", buf);
    helper_console_goto(0, (SHORT)g_helper_row);
    WriteConsoleA(g_hout, line, (DWORD)strlen(line), &written, NULL);
    g_helper_row++;
    if (g_helper_row >= 45) {
        helper_console_clear_below();
        g_helper_row = 18;
    }
}

/* Display the optional mrc_banner.ans ANSI banner on the helper console. */
static void helper_show_ans_banner(void) {
    FILE *fp;
    char buf[1024];
    if (!g_hout) return;
    fp = fopen("mrc_banner.ans", "rb");
    if (!fp) { helper_ui_log("mrc_banner.ans not found (optional)"); return; }
    helper_clear_screen();
    helper_set_attr(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    while (fgets(buf, sizeof(buf), fp)) helper_write_ansi(buf);
    fclose(fp);
    g_helper_row = 16;
}

/* ============================================================================
   LOGGING AND BRIDGE
   ============================================================================ */

#define LOG_V(fmt,...) do { if (helper_verbose) helper_log(fmt, ##__VA_ARGS__); } while(0)

/* ============================================================================
   BBS CONFIGURATION
   ============================================================================ */

#define MRCBBS_DAT "MRCBBS.DAT"

typedef struct {
    char bbs_name[64];
    char bbs_pretty[64];
    char sysop[64];
    char description[128];
    char telnet[128];
    char ssh[128];
    char website[128];
    char server[128];       /* MRC server hostname, e.g. na-multi.relaychat.net */
    int  port;              /* MRC server port, default 5000 */
    int  show_motd;         /* 1 = auto-fetch MOTD once per session after join */
} mrcbbs_cfg_t;

static mrcbbs_cfg_t g_bbscfg;
static int g_bbscfg_loaded = 0;

static void mrcbbs_trim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' '))
        s[--n] = 0;
}

/* Load MRCBBS.DAT into g_bbscfg; returns 1 on success, 0 if missing/incomplete. */
static int load_mrcbbs_config(void) {
    FILE *fp;
    char line[512], *eq, *k, *v;

    memset(&g_bbscfg, 0, sizeof(g_bbscfg));
    fp = fopen(MRCBBS_DAT, "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp)) {
        mrcbbs_trim(line);
        if (!line[0] || line[0] == '#') continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        k = line;
        v = eq + 1;
        while (*k == ' ') ++k;
        while (*v == ' ') ++v;
        if      (strcmp(k, "bbs_name")    == 0) strncpy(g_bbscfg.bbs_name,    v, sizeof(g_bbscfg.bbs_name)-1);
        else if (strcmp(k, "bbs_pretty")  == 0) strncpy(g_bbscfg.bbs_pretty,  v, sizeof(g_bbscfg.bbs_pretty)-1);
        else if (strcmp(k, "sysop")       == 0) strncpy(g_bbscfg.sysop,       v, sizeof(g_bbscfg.sysop)-1);
        else if (strcmp(k, "description") == 0) strncpy(g_bbscfg.description, v, sizeof(g_bbscfg.description)-1);
        else if (strcmp(k, "telnet")      == 0) strncpy(g_bbscfg.telnet,      v, sizeof(g_bbscfg.telnet)-1);
        else if (strcmp(k, "ssh")         == 0) strncpy(g_bbscfg.ssh,         v, sizeof(g_bbscfg.ssh)-1);
        else if (strcmp(k, "website")     == 0) strncpy(g_bbscfg.website,     v, sizeof(g_bbscfg.website)-1);
        else if (strcmp(k, "server")      == 0) strncpy(g_bbscfg.server,      v, sizeof(g_bbscfg.server)-1);
        else if (strcmp(k, "port")        == 0) g_bbscfg.port      = atoi(v);
        else if (strcmp(k, "show_motd")   == 0) g_bbscfg.show_motd = (atoi(v) != 0);
    }
    fclose(fp);

    /* Validate required fields */
    if (!g_bbscfg.bbs_name[0] || !g_bbscfg.bbs_pretty[0] ||
        !g_bbscfg.sysop[0]    || !g_bbscfg.telnet[0]) {
        return 0;
    }
    /* Apply sensible defaults for optional fields */
    if (!g_bbscfg.server[0])
        strncpy(g_bbscfg.server, "na-multi.relaychat.net", sizeof(g_bbscfg.server)-1);
    if (g_bbscfg.port <= 0)
        g_bbscfg.port = 5000;
    g_bbscfg_loaded = 1;
    return 1;
}

/* Non-zero when running in single-node mode (only slot 0 is used). */
static int g_node_fixed = 0;

/* Initialise defaults for a freshly-allocated node slot. */
static void node_init_defaults(node_state_t *nd) {
    nd->handle_color = 11;
    nd->text_color   = 7;
    safe_copy(nd->enterroom_msg, sizeof(nd->enterroom_msg), "has joined");
    safe_copy(nd->leaveroom_msg, sizeof(nd->leaveroom_msg), "has left");
    safe_copy(nd->last_server,   sizeof(nd->last_server),   "na-multi.relaychat.net");
    nd->last_port      = 5000;
    nd->mrc.sock       = INVALID_SOCKET;
    nd->last_iamhere   = 0;
    nd->last_cmd_time  = GetTickCount64();
}

/* Returns 1 if any active node currently has WHOON routing enabled. */
static int any_node_whoon_active(void) {
    int i;
    for (i = 0; i < MAX_NODES; i++) {
        if (g_nodes[i].active && g_nodes[i].show_whoon_to_dos)
            return 1;
    }
    return 0;
}

/* Configure single-node mode for node number N (0/1 = default, 2-8 = suffixed). */
void helper_set_node(int n) {
    node_state_t *nd = &g_nodes[0];
    g_node_fixed = 1;
    node_init_defaults(nd);
    if (n >= 2 && n <= 8) {
        nd->node_num = n;
        snprintf(nd->bridge_in,  sizeof(nd->bridge_in),  "ANETDOS%d.IN",  n);
        snprintf(nd->bridge_out, sizeof(nd->bridge_out), "ANETDOS%d.OUT", n);
    } else {
        nd->node_num = 0;
        safe_copy(nd->bridge_in,  sizeof(nd->bridge_in),  "ANETDOS.IN");
        safe_copy(nd->bridge_out, sizeof(nd->bridge_out), "ANETDOS.OUT");
    }
    nd->active   = 1;
    g_cur_node   = nd;
}

void helper_log(const char *fmt, ...) {
    va_list ap;
    if (!g_log) { g_log = fopen("anetmrc_bridge.log", "a"); if (!g_log) return; }
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* Substitute CP437 special chars 1-6 with ASCII equivalents. */
static void subst_cp437(char *buf, size_t bufsz, const char *src) {
    size_t i = 0;
    while (*src && i + 4 < bufsz) {
        unsigned char c = (unsigned char)*src++;
        if (c >= 1 && c <= 6) {
            const char *sub[] = { ":)", ":(", "<3", "<>", "[+]", "[S]" };
            const char *r = sub[c - 1];
            while (*r && i + 1 < bufsz) buf[i++] = *r++;
        } else {
            buf[i++] = (char)c;
        }
    }
    buf[i] = 0;
}

/* Extract visible columns [from,to) from `src`, preserving pipe color codes. */
static void extract_col_range(char *out, size_t outsz, const char *src,
                              int from, int to) {
    size_t pos = 0;
    int vcol = 0;
    char cur_color[4] = "";
    int emitted_color = 0;

    if (outsz == 0) return;
    out[0] = 0;
    if (!src) return;

    while (*src && vcol < to && pos + 4 < outsz) {
        if (src[0] == '|' &&
            isdigit((unsigned char)src[1]) &&
            isdigit((unsigned char)src[2])) {
            if (vcol < from) {
                cur_color[0] = src[0];
                cur_color[1] = src[1];
                cur_color[2] = src[2];
                cur_color[3] = 0;
            } else {
                out[pos++] = src[0];
                out[pos++] = src[1];
                out[pos++] = src[2];
                emitted_color = 1;
            }
            src += 3;
            continue;
        }
        if (vcol < from) { ++vcol; ++src; continue; }
        if (!emitted_color && cur_color[0] && pos + 3 < outsz) {
            out[pos++] = cur_color[0];
            out[pos++] = cur_color[1];
            out[pos++] = cur_color[2];
            emitted_color = 1;
        }
        out[pos++] = *src++;
        ++vcol;
    }
    /* Trim trailing spaces and trailing pipe codes to avoid color leak. */
    for (;;) {
        if (pos > 0 && out[pos - 1] == ' ') { --pos; continue; }
        if (pos >= 3 && out[pos - 3] == '|' &&
            isdigit((unsigned char)out[pos - 2]) &&
            isdigit((unsigned char)out[pos - 1])) {
            pos -= 3; continue;
        }
        break;
    }
    out[pos] = 0;
}

/* Emit `src` into out[] padded/truncated to target_width visible cols. */
static size_t emit_padded(char *out, size_t outsz, size_t pos,
                          const char *src, int target_width) {
    int vcol = 0;
    while (src && *src && pos + 4 < outsz) {
        if (src[0] == '|' &&
            isdigit((unsigned char)src[1]) &&
            isdigit((unsigned char)src[2])) {
            out[pos++] = *src++;
            out[pos++] = *src++;
            out[pos++] = *src++;
            continue;
        }
        if (vcol >= target_width) break;
        out[pos++] = *src++;
        ++vcol;
    }
    while (vcol < target_width && pos + 1 < outsz) {
        out[pos++] = ' ';
        ++vcol;
    }
    return pos;
}

/* Returns 1 if the body looks like the /list "Flags: $ = Show..." legend
 * (or similar footer). These contain a literal '#' and would otherwise
 * trigger the chatters/list data-row path, which mangles the text. */
static int is_list_flags_legend(const char *body) {
    if (!body) return 0;
    return strstr(body, "Flags:") != NULL && strstr(body, "# = ") != NULL;
}

/* Returns 1 if the body is shaped like a /chatters data row (has a '#'
 * in the room column AND 'idle:' in the idle column). Used to suppress
 * the chatters formatter when stale `chatters_notme_count` leaks over
 * onto unrelated server content (join/part notices, !games tables, etc.). */
static int looks_like_chatters_row(const char *body) {
    if (!body) return 0;
    return strstr(body, "#") != NULL && strstr(body, "idle:") != NULL;
}

/* Write src to out preserving pipe codes and original spacing, truncating
 * the visible width at `maxcols`. Used for server content that isn't a
 * chatters/list data row — avoids compact_line's space-capping which
 * destroys wider table alignment (e.g., !games). */
static void passthrough_truncate(char *out, size_t outsz,
                                 const char *src, int maxcols) {
    size_t pos = 0;
    int vcol = 0;
    if (!src) { if (outsz) out[0] = 0; return; }
    while (*src && pos + 4 < outsz) {
        if (src[0] == '|' &&
            isdigit((unsigned char)src[1]) &&
            isdigit((unsigned char)src[2])) {
            out[pos++] = *src++;
            out[pos++] = *src++;
            out[pos++] = *src++;
            continue;
        }
        if (vcol >= maxcols) break;
        out[pos++] = *src++;
        ++vcol;
    }
    if (pos < outsz) out[pos] = 0;
    else out[outsz - 1] = 0;
}

/* Returns 1 if the first visible character of s (ignoring pipe codes) is '#'. */
static int starts_with_hash(const char *s) {
    if (!s) return 0;
    while (*s) {
        if (s[0] == '|' &&
            isdigit((unsigned char)s[1]) &&
            isdigit((unsigned char)s[2])) { s += 3; continue; }
        return (*s == '#');
    }
    return 0;
}

/* Split a server /list row into room/count/topic and emit a DOS-width line. */
static void format_list_line(char *out, size_t outsz, const char *body) {
    char room[96], count[48], topic[256];
    size_t pos = 0;

    /* Flags legend footer and any non-/list row pass through — avoids
     * mangling join notices, !games tables, and other content caught by
     * stale rooms_notme_count. */
    if (is_list_flags_legend(body)) {
        passthrough_truncate(out, outsz, body, 78);
        return;
    }

    extract_col_range(room,  sizeof(room),  body,  5, 28);
    /* Emit a clean header aligned to our output columns. */
    if (body && strstr(body, "Rooms") && strstr(body, "Usr") &&
        strstr(body, "Topic")) {
        snprintf(out, outsz,
                 "|08*|16|00.|16|08:  |14Rooms        |14Usr  |14Topic"
                 "                                    |15|19 /list |07");
        return;
    }
    /* Non-/list rows fall through to passthrough (keeps original spacing). */
    if (!starts_with_hash(room)) {
        passthrough_truncate(out, outsz, body, 78);
        return;
    }
    extract_col_range(count, sizeof(count), body, 29, 32);
    extract_col_range(topic, sizeof(topic), body, 34, 120);

    helper_log("LIST fmt room=[%s] count=[%s] topic=[%.60s]",
               room, count, topic);

    if (outsz == 0) return;
    if (pos + 17 < outsz) {
        const char *pfx = "|08*|16|00.|16|08:  ";
        size_t n = strlen(pfx);
        memcpy(out + pos, pfx, n); pos += n;
    }
    pos = emit_padded(out, outsz, pos, room, 12);
    if (pos + 1 < outsz) { out[pos++] = ' '; }
    pos = emit_padded(out, outsz, pos, count, 3);
    if (pos + 2 < outsz) { out[pos++] = ' '; out[pos++] = ' '; }
    pos = emit_padded(out, outsz, pos, topic, 45);
    if (pos + 3 < outsz) {
        out[pos++] = '|'; out[pos++] = '0'; out[pos++] = '7';
    }
    out[pos] = 0;
}

/* Returns 1 if s contains a visible '#' character (ignoring pipe codes). */
static int contains_hash(const char *s) {
    if (!s) return 0;
    while (*s) {
        if (s[0] == '|' &&
            isdigit((unsigned char)s[1]) &&
            isdigit((unsigned char)s[2])) { s += 3; continue; }
        if (*s == '#') return 1;
        ++s;
    }
    return 0;
}

/* Split a server /chatters row into fixed columns and emit a DOS-width line.
 * Server places "#room (network)" together in cols 35..~50 with variable
 * widths, so room and network must be extracted as one field — splitting
 * them at col 45 drops whatever character happens to sit there. */
static void format_chatters_line(char *out, size_t outsz, const char *body) {
    char marker[32], name[96], badge[32];
    char roomtype[64], idle[64];
    size_t pos = 0;

    /* Skip rows that aren't chatters data (legend footers, join/part
     * notices, !games output, etc.). Stale chatters_notme_count from an
     * interrupted /chatters would otherwise drag these through column
     * extracts and lose characters. */
    if (is_list_flags_legend(body) || !looks_like_chatters_row(body)) {
        passthrough_truncate(out, outsz, body, 78);
        return;
    }

    extract_col_range(roomtype, sizeof(roomtype), body, 35, 56);
    /* Non-chatter rows fall through to passthrough (no column mangling). */
    if (!contains_hash(roomtype)) {
        passthrough_truncate(out, outsz, body, 78);
        return;
    }

    extract_col_range(marker, sizeof(marker), body,  3,  6);
    extract_col_range(name,   sizeof(name),   body,  8, 29);
    extract_col_range(badge,  sizeof(badge),  body, 30, 33);
    extract_col_range(idle,   sizeof(idle),   body, 56, 120);

    helper_log("CHAT fmt marker=[%s] name=[%.30s] badge=[%s] roomtype=[%s]",
               marker, name, badge, roomtype);

    if (outsz == 0) return;
    if (pos + 17 < outsz) {
        const char *pfx = "|08*|16|00.|16|08:  ";
        size_t n = strlen(pfx);
        memcpy(out + pos, pfx, n); pos += n;
    }
    pos = emit_padded(out, outsz, pos, marker, 2);
    if (pos + 1 < outsz) out[pos++] = ' ';
    pos = emit_padded(out, outsz, pos, name,  14);
    if (pos + 1 < outsz) out[pos++] = ' ';
    pos = emit_padded(out, outsz, pos, badge,  5);
    if (pos + 1 < outsz) out[pos++] = ' ';
    pos = emit_padded(out, outsz, pos, roomtype, 21);
    if (pos + 1 < outsz) out[pos++] = ' ';
    pos = emit_padded(out, outsz, pos, idle,  20);
    if (pos + 3 < outsz) {
        out[pos++] = '|'; out[pos++] = '0'; out[pos++] = '7';
    }
    out[pos] = 0;
}

/* If body is the status-marker legend line, emit a spaced replacement.
 * Returns 1 if handled, 0 otherwise. */
static int write_legend_if_match(const char *body) {
    if (!body || !strstr(body, "D-Dial") || !strstr(body, "Contribute"))
        return 0;
    bridge_write_listing(
        "|08* :__ "
        "|13%% |07D-Dial  "
        "|10& |07Trust  "
        "|13* |07Fed  "
        "|12? |07Away  "
        "|14$ |07Connector  "
        "|12{|12x|12} |07!Contribute|07");
    return 1;
}

/* Case-insensitive substring search. */
static const char *ci_strstr(const char *hay, const char *needle) {
    size_t n;
    if (!hay || !needle) return NULL;
    n = strlen(needle);
    if (!n) return hay;
    for (; *hay; ++hay)
        if (_strnicmp(hay, needle, n) == 0) return hay;
    return NULL;
}

/* Returns 1 if body mentions our handle and the sender is not us. */
static int mention_check(const char *from_user, const char *body) {
    const char *me = g_mrc.nick[0] ? g_mrc.nick : g_mrc.user;
    if (!me[0] || !body || !body[0]) return 0;
    if (from_user && _stricmp(from_user, me) == 0) return 0;
    return ci_strstr(body, me) != NULL;
}

/* Append one formatted line to the DOS bridge input file. */
static void bridge_write(const char *fmt, ...) {
    FILE *out;
    va_list ap;
    char raw[HELPER_LINE_MAX];
    char buf[HELPER_LINE_MAX];

    va_start(ap, fmt);
    vsnprintf(raw, sizeof(raw), fmt, ap);
    va_end(ap);

    subst_cp437(buf, sizeof(buf), raw);

    out = fopen(g_bridge_in, "a");
    if (!out) return;
    fprintf(out, "%s\n", buf);
    fclose(out);
}

/* Like bridge_write but prefixes "LISTING " so the door skips the timestamp. */
static void bridge_write_listing(const char *fmt, ...) {
    FILE *out;
    va_list ap;
    char raw[HELPER_LINE_MAX];
    char buf[HELPER_LINE_MAX];

    va_start(ap, fmt);
    vsnprintf(raw, sizeof(raw), fmt, ap);
    va_end(ap);

    subst_cp437(buf, sizeof(buf), raw);

    out = fopen(g_bridge_in, "a");
    if (!out) return;
    fprintf(out, "LISTING %s\n", buf);
    fclose(out);
}

/* Send a STATUS line to the door, log it, and update the helper status line. */
static void write_status_and_log(const char *status) {
    bridge_write("STATUS %s", status);
    helper_log("STATUS %s", status);
    helper_status_update(status);
}

/* ============================================================================
   STRING UTILITIES
   ============================================================================ */

/* Trim trailing CR/LF from s. */
static void trim_line(char *s) {
    size_t n;
    if (!s) return;
    n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

/* NUL-terminated bounded copy. */
static void safe_copy(char *dst, size_t dstsz, const char *src) {
    size_t i;
    if (!dst || dstsz == 0) return;
    if (!src) src = "";
    for (i = 0; i + 1 < dstsz && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

/* Copy src to dst replacing spaces with underscores. */
static void underspace_copy(char *dst, size_t dstsz, const char *src) {
    size_t i;
    if (!dst || dstsz == 0) return;
    if (!src) src = "";
    for (i = 0; i + 1 < dstsz && src[i]; ++i)
        dst[i] = (src[i] == ' ') ? '_' : src[i];
    dst[i] = 0;
}

/* Replace all underscores in s with spaces. */
static void deunderscore(char *s) {
    if (!s) return;
    while (*s) { if (*s == '_') *s = ' '; ++s; }
}

/* Remove a leading '#' from s in place. */
static void strip_hash(char *s) {
    if (!s) return;
    if (s[0] == '#') memmove(s, s + 1, strlen(s));
}

/* Sanitize a string for MRC fields: keep Chr(32)-Chr(125); ~ becomes -. */
static void sanitize_field(char *dst, size_t dstsz, const char *src) {
    size_t i = 0;
    if (!dst || dstsz == 0) return;
    if (!src) src = "";
    while (*src && i + 1 < dstsz) {
        unsigned char c = (unsigned char)*src++;
        if (c == '~') { if (i + 1 < dstsz) dst[i++] = '-'; }
        else if (c >= 32 && c <= 125) { dst[i++] = (char)c; }
    }
    dst[i] = 0;
}

/* Set g_in_room when room_name matches g_mrc.room. */
static void mark_in_room_if_matches(const char *room_name) {
    char tmp[64];
    if (!room_name || !*room_name) return;
    safe_copy(tmp, sizeof(tmp), room_name);
    strip_hash(tmp);
    if (g_mrc.room[0] && stricmp(tmp, g_mrc.room) == 0)
        g_in_room = 1;
}

/* Build the formatted handle string (color + prefix/suffix) for outgoing messages. */
static void build_handle_str(char *out, size_t outsz) {
    const char *nick = g_mrc.nick[0] ? g_mrc.nick : g_mrc.user;
    if (g_handle_color >= 0 && g_handle_color <= 15) {
        snprintf(out, outsz, "%s|%02d%s%s|07",
                 g_handle_prefix, g_handle_color, nick, g_handle_suffix);
    } else {
        snprintf(out, outsz, "%s%s%s",
                 g_handle_prefix, nick, g_handle_suffix);
    }
}

/* ============================================================================
   TLS/CRYPTO FUNCTIONS
   ============================================================================ */

/* Release the current TLS context and credentials. */
static void tls_cleanup(void) {
    if (g_mrc.have_ctx)  { DeleteSecurityContext(&g_mrc.ctx);       g_mrc.have_ctx = 0; }
    if (g_mrc.have_cred) { FreeCredentialHandle(&g_mrc.cred);       g_mrc.have_cred = 0; }
    g_mrc.tls_ready = 0;
    memset(&g_mrc.sizes, 0, sizeof(g_mrc.sizes));
    g_mrc.tls_raw_used = 0;
    g_mrc.plainq_used = 0;
    g_mrc.plainq_pos = 0;
}

/* Close the MRC socket and reset all per-node connection state. */
static void mrc_reset(void) {
    if (g_mrc.sock != INVALID_SOCKET) closesocket(g_mrc.sock);
    tls_cleanup();
    memset(&g_mrc, 0, sizeof(g_mrc));
    g_mrc.sock = INVALID_SOCKET;
    g_in_room = 0;
    g_show_whoon_to_dos    = 0;
    g_whoon_lines_left     = 0;
    g_show_chatters_to_dos = 0;
    g_chatters_notme_count = 0;
    g_rooms_notme_count    = 0;
    g_show_motd_to_dos     = 0;
    g_motd_fired           = 0;
    g_identify_sent        = 0;
}

/* Send len bytes on socket s, looping until done; returns 1 on success. */
static int socket_send_all(SOCKET s, const void *vbuf, int len) {
    const char *buf = (const char *)vbuf;
    int sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

/* Acquire Schannel outbound credentials for a TLS client session. */
static int tls_acquire_credentials(void) {
    SECURITY_STATUS ss;
    TimeStamp exp;
    SCHANNEL_CRED sc;
    memset(&sc, 0, sizeof(sc));
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS |
                 SCH_CRED_IGNORE_NO_REVOCATION_CHECK | SCH_CRED_IGNORE_REVOCATION_OFFLINE;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_1_CLIENT | SP_PROT_TLS1_0_CLIENT;
    ss = AcquireCredentialsHandleA(NULL, UNISP_NAME_A, SECPKG_CRED_OUTBOUND,
                                   NULL, &sc, NULL, NULL, &g_mrc.cred, &exp);
    if (ss != SEC_E_OK) { helper_log("AcquireCredentials failed: 0x%08lx", (unsigned long)ss); return 0; }
    g_mrc.have_cred = 1;
    return 1;
}

/* Perform the Schannel client TLS handshake against server. */
static int tls_handshake(const char *server) {
    SECURITY_STATUS ss;
    TimeStamp exp;
    DWORD attrs = 0;
    DWORD req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR |
                ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    unsigned char inbuf[32768];
    int in_used = 0, first = 1;

    if (!tls_acquire_credentials()) return 0;

    for (;;) {
        SecBuffer outbufs[1];
        SecBufferDesc outdesc;
        outbufs[0].BufferType = SECBUFFER_TOKEN;
        outbufs[0].pvBuffer = NULL;
        outbufs[0].cbBuffer = 0;
        outdesc.ulVersion = SECBUFFER_VERSION;
        outdesc.cBuffers = 1;
        outdesc.pBuffers = outbufs;

        if (first) {
            ss = InitializeSecurityContextA(&g_mrc.cred, NULL, (SEC_CHAR *)server, req,
                                            0, 0, NULL, 0, &g_mrc.ctx, &outdesc, &attrs, &exp);
            g_mrc.have_ctx = 1;
            first = 0;
        } else {
            SecBuffer inbufs[2];
            SecBufferDesc indesc;
            inbufs[0].BufferType = SECBUFFER_TOKEN;
            inbufs[0].pvBuffer = inbuf;
            inbufs[0].cbBuffer = (unsigned long)in_used;
            inbufs[1].BufferType = SECBUFFER_EMPTY;
            inbufs[1].pvBuffer = NULL;
            inbufs[1].cbBuffer = 0;
            indesc.ulVersion = SECBUFFER_VERSION;
            indesc.cBuffers = 2;
            indesc.pBuffers = inbufs;
            ss = InitializeSecurityContextA(&g_mrc.cred, &g_mrc.ctx, (SEC_CHAR *)server, req,
                                            0, 0, &indesc, 0, &g_mrc.ctx, &outdesc, &attrs, &exp);
            if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED) {
                if (inbufs[1].BufferType == SECBUFFER_EXTRA && inbufs[1].cbBuffer > 0) {
                    int extra = (int)inbufs[1].cbBuffer;
                    memmove(inbuf, inbuf + (in_used - extra), (size_t)extra);
                    in_used = extra;
                } else {
                    in_used = 0;
                }
            }
        }

        if (outbufs[0].cbBuffer != 0 && outbufs[0].pvBuffer != NULL) {
            if (!socket_send_all(g_mrc.sock, outbufs[0].pvBuffer, (int)outbufs[0].cbBuffer)) {
                FreeContextBuffer(outbufs[0].pvBuffer);
                helper_log("TLS token send failed");
                return 0;
            }
            FreeContextBuffer(outbufs[0].pvBuffer);
        }

        if (ss == SEC_E_OK) break;

        if (ss == SEC_E_INCOMPLETE_MESSAGE || ss == SEC_I_CONTINUE_NEEDED) {
            int n = recv(g_mrc.sock, (char *)inbuf + in_used,
                         (int)sizeof(inbuf) - in_used, 0);
            if (n <= 0) { helper_log("TLS recv during handshake failed"); return 0; }
            in_used += n;
            continue;
        }

        helper_log("InitializeSecurityContext failed: 0x%08lx", (unsigned long)ss);
        return 0;
    }

    ss = QueryContextAttributes(&g_mrc.ctx, SECPKG_ATTR_STREAM_SIZES, &g_mrc.sizes);
    if (ss != SEC_E_OK) {
        helper_log("QueryContextAttributes failed: 0x%08lx", (unsigned long)ss);
        return 0;
    }

    g_mrc.tls_ready = 1;
    helper_log("TLS handshake complete");
    return 1;
}

/* Format a string and send it over the established TLS session. */
static int tls_send_all_fmt(const char *fmt, ...) {
    va_list ap;
    char plain[2048];
    int plain_len, max_chunk, off = 0;

    if (!g_mrc.tls_ready) return 0;

    va_start(ap, fmt);
    vsnprintf(plain, sizeof(plain), fmt, ap);
    va_end(ap);
    plain_len = (int)strlen(plain);

    max_chunk = (int)g_mrc.sizes.cbMaximumMessage;
    if (max_chunk <= 0) return 0;

    while (off < plain_len) {
        unsigned char *packet;
        SecBuffer bufs[4];
        SecBufferDesc desc;
        SECURITY_STATUS ss;
        int chunk = plain_len - off;
        int total;

        if (chunk > max_chunk) chunk = max_chunk;
        packet = (unsigned char *)malloc(g_mrc.sizes.cbHeader + chunk + g_mrc.sizes.cbTrailer);
        if (!packet) return 0;
        memcpy(packet + g_mrc.sizes.cbHeader, plain + off, (size_t)chunk);

        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].pvBuffer = packet;
        bufs[0].cbBuffer = g_mrc.sizes.cbHeader;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer = packet + g_mrc.sizes.cbHeader;
        bufs[1].cbBuffer = chunk;
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].pvBuffer = packet + g_mrc.sizes.cbHeader + chunk;
        bufs[2].cbBuffer = g_mrc.sizes.cbTrailer;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        bufs[3].pvBuffer = NULL;
        bufs[3].cbBuffer = 0;

        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers = 4;
        desc.pBuffers = bufs;

        ss = EncryptMessage(&g_mrc.ctx, 0, &desc, 0);
        if (ss != SEC_E_OK) {
            helper_log("EncryptMessage failed: 0x%08lx", (unsigned long)ss);
            free(packet);
            return 0;
        }

        total = (int)(bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
        if (!socket_send_all(g_mrc.sock, packet, total)) { free(packet); return 0; }
        free(packet);
        off += chunk;
    }

    LOG_V("SEND(TLS): %s", plain);
    return 1;
}

/* Send a raw line to the server, via TLS if enabled else plain TCP. */
static int mrc_send_raw(const char *line) {
    if (!g_mrc.connected || g_mrc.sock == INVALID_SOCKET) return 0;
    if (g_mrc.use_tls) return tls_send_all_fmt("%s", line);
    LOG_V("SEND: %s", line);
    return send(g_mrc.sock, line, (int)strlen(line), 0) == (int)strlen(line);
}

/* Queue decrypted plaintext bytes in the TLS receive buffer. */
static int tls_queue_plain(const unsigned char *src, int len) {
    if (len <= 0) return 0;
    if (g_mrc.plainq_used + len > (int)sizeof(g_mrc.plainq)) return 0;
    memcpy(g_mrc.plainq + g_mrc.plainq_used, src, (size_t)len);
    g_mrc.plainq_used += len;
    return 1;
}

/* Read decrypted plaintext from the TLS session; returns bytes, 0 on close, -1/-2 on error/would-block. */
static int tls_read_plain(unsigned char *out, int outsz) {
    if (g_mrc.plainq_pos < g_mrc.plainq_used) {
        int avail = g_mrc.plainq_used - g_mrc.plainq_pos;
        if (avail > outsz) avail = outsz;
        memcpy(out, g_mrc.plainq + g_mrc.plainq_pos, (size_t)avail);
        g_mrc.plainq_pos += avail;
        if (g_mrc.plainq_pos >= g_mrc.plainq_used) {
            g_mrc.plainq_pos = 0;
            g_mrc.plainq_used = 0;
        }
        return avail;
    }

    for (;;) {
        SecBuffer bufs[4];
        SecBufferDesc desc;
        SECURITY_STATUS ss;
        int n;

        if (g_mrc.tls_raw_used == 0) {
            n = recv(g_mrc.sock, (char *)g_mrc.tls_raw, (int)sizeof(g_mrc.tls_raw), 0);
        } else {
            n = recv(g_mrc.sock, (char *)g_mrc.tls_raw + g_mrc.tls_raw_used,
                     (int)sizeof(g_mrc.tls_raw) - g_mrc.tls_raw_used, 0);
        }

        if (n == 0) return 0;
        if (n < 0) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) return -2;
            helper_log("TLS recv error %d", e);
            return -1;
        }
        g_mrc.tls_raw_used += n;

        bufs[0].BufferType = SECBUFFER_DATA;
        bufs[0].pvBuffer = g_mrc.tls_raw;
        bufs[0].cbBuffer = (unsigned long)g_mrc.tls_raw_used;
        bufs[1].BufferType = SECBUFFER_EMPTY;
        bufs[2].BufferType = SECBUFFER_EMPTY;
        bufs[3].BufferType = SECBUFFER_EMPTY;

        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers = 4;
        desc.pBuffers = bufs;

        ss = DecryptMessage(&g_mrc.ctx, &desc, 0, NULL);
        if (ss == SEC_E_INCOMPLETE_MESSAGE) continue;
        if (ss == SEC_I_CONTEXT_EXPIRED) return 0;
        if (ss != SEC_E_OK && ss != SEC_I_RENEGOTIATE) {
            helper_log("DecryptMessage failed: 0x%08lx", (unsigned long)ss);
            return -1;
        }

        {
            int i, found = 0;
            for (i = 0; i < 4; ++i) {
                if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer > 0) {
                    if (!tls_queue_plain((const unsigned char *)bufs[i].pvBuffer,
                                        (int)bufs[i].cbBuffer)) return -1;
                    found = 1;
                }
            }
            for (i = 0; i < 4; ++i) {
                if (bufs[i].BufferType == SECBUFFER_EXTRA) {
                    memmove(g_mrc.tls_raw,
                            (unsigned char *)g_mrc.tls_raw +
                                (g_mrc.tls_raw_used - (int)bufs[i].cbBuffer),
                            (size_t)bufs[i].cbBuffer);
                    g_mrc.tls_raw_used = (int)bufs[i].cbBuffer;
                    break;
                }
            }
            if (i == 4) g_mrc.tls_raw_used = 0;
            if (found) return tls_read_plain(out, outsz);
        }
    }
}

/* ============================================================================
   MRC PROTOCOL - PACKET SENDING
   ============================================================================ */

/* Encode a 7-field MRC packet and send it to the server. */
static void mrc_send_packet_fields(const char *f1, const char *f2, const char *f3,
                                   const char *f4, const char *f5, const char *f6,
                                   const char *f7) {
    char line[1024];
    snprintf(line, sizeof(line), "%s~%s~%s~%s~%s~%s~%s~\n",
             f1 ? f1 : "", f2 ? f2 : "", f3 ? f3 : "",
             f4 ? f4 : "", f5 ? f5 : "", f6 ? f6 : "",
             f7 ? f7 : "");
    mrc_send_raw(line);
}

/* Send the initial handshake line identifying this BBS and client version. */
static void mrc_send_handshake(void) {
    char hs[192];
    snprintf(hs, sizeof(hs), "%s~%s\n",
             g_mrc.bbs_pretty[0] ? g_mrc.bbs_pretty : g_mrc.bbs_name,
             g_mrc.version_info);
    mrc_send_raw(hs);
}

/* Format a high-precision epoch timestamp into buf for use as msgext. */
static void get_epoch_str(char *buf, size_t sz) {
    LARGE_INTEGER freq, count;
    double frac = 0.0;
    if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&count) && freq.QuadPart > 0)
        frac = (double)(count.QuadPart % freq.QuadPart) / (double)freq.QuadPart;
    snprintf(buf, sz, "%.6f", (double)time(NULL) + frac);
}

/* Returns 1 if s contains any non-space, non-pipe-code visible character. */
static int has_visible_text(const char *s) {
    while (*s) {
        if (*s == '|' && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2])) {
            s += 3; continue;
        }
        if (*s != ' ' && *s != '\t') return 1;
        ++s;
    }
    return 0;
}

/* Broadcast a room join/leave notification (MRC 1.3 "Room join/part" form,
 * F6=room so the server tags it as a room event). */
static void mrc_send_room_notification(const char *room, const char *msg) {
    char handle[80], body[200];
    if (!room || !*room || !msg || !*msg) return;
    build_handle_str(handle, sizeof(handle));
    snprintf(body, sizeof(body), "%s %s", handle, msg);
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, room,
                           "NOTME", "", room, body);
}

/* Broadcast a "left chat" goodbye line to every room (F6="" form). */
static void mrc_send_leave_chat(const char *msg) {
    char handle[80], body[200];
    if (!msg || !*msg) return;
    build_handle_str(handle, sizeof(handle));
    snprintf(body, sizeof(body),
             "|07- |12%s|04 has left chat|07 (|15%s|07)", handle, msg);
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name,
                           g_mrc.room[0] ? g_mrc.room : "lobby",
                           "NOTME", "", "", body);
}

/* Switch to newroom: broadcast leave for the current room, then send NEWROOM. */
static void mrc_send_newroom(const char *newroom) {
    char roombuf[32], body[96], msgext[64];
    underspace_copy(roombuf, sizeof(roombuf), newroom);
    strip_hash(roombuf);
    snprintf(body, sizeof(body), "NEWROOM:%s:%s",
             g_mrc.room[0] ? g_mrc.room : "lobby", roombuf);
    safe_copy(g_mrc.last_room, sizeof(g_mrc.last_room),
              g_mrc.room[0] ? g_mrc.room : "lobby");

    if (g_mrc.last_room[0] && stricmp(g_mrc.last_room, roombuf) != 0) {
        const char *lmsg = (g_leaveroom_msg[0] && has_visible_text(g_leaveroom_msg))
                           ? g_leaveroom_msg : "has left";
        char handle[80];
        build_handle_str(handle, sizeof(handle));
        mrc_send_room_notification(g_mrc.last_room, lmsg);
        /* Local echo: NOTME isn't sent back to the sender. */
        bridge_write("|08* |14%s %s|07", handle, lmsg);
    }

    safe_copy(g_mrc.room, sizeof(g_mrc.room), roombuf);
    get_epoch_str(msgext, sizeof(msgext));
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.last_room,
                           "SERVER", msgext, g_mrc.last_room, body);
}

/* Send an IAMHERE keepalive with the given status extension (ACTIVE/AWAY). */
static void mrc_send_iamhere(const char *ext) {
    char body[96], msgext[64];
    get_epoch_str(msgext, sizeof(msgext));
    snprintf(body, sizeof(body), "IAMHERE:%s", ext ? ext : "");
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", msgext, g_mrc.room, body);
}

/* Reply to server PING with an IMALIVE keepalive packet. */
static void mrc_send_imalive(void) {
    char pidbuf[16], msgext[64], body[128];
    snprintf(pidbuf, sizeof(pidbuf), "%lu", (unsigned long)GetCurrentProcessId());
    get_epoch_str(msgext, sizeof(msgext));
    snprintf(body, sizeof(body), "IMALIVE:%s",
             g_mrc.bbs_pretty[0] ? g_mrc.bbs_pretty : g_mrc.bbs_name);
    mrc_send_packet_fields("CLIENT", g_mrc.bbs_name, pidbuf,
                           "SERVER", msgext, "", body);
}

/* Announce the client's advertised CAPABILITIES list to the server. */
static void mrc_send_capabilities(void) {
    char msgext[64];
    get_epoch_str(msgext, sizeof(msgext));
    mrc_send_packet_fields("CLIENT", g_mrc.bbs_name, "0",
                           "SERVER", msgext, "", "CAPABILITIES:MCI CTCP SSL MSGEXT");
}

/* Send a USERIP packet (BBS bridges send 0.0.0.0 — no end-user IP). */
static void mrc_send_userip(const char *ip) {
    char msgext[64], body[96];
    get_epoch_str(msgext, sizeof(msgext));
    snprintf(body, sizeof(body), "USERIP:%s", ip ? ip : "0.0.0.0");
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, "",
                           "SERVER", msgext, "", body);
}

/* Send the terminal size to the server. */
static void mrc_send_termsize(int w, int h) {
    char msgext[64], body[64];
    get_epoch_str(msgext, sizeof(msgext));
    snprintf(body, sizeof(body), "TERMSIZE:%d,%d", w, h);
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, "",
                           "SERVER", msgext, "", body);
}

/* Send a BBSMETA packet announcing the user's security level and SysOp. */
static void mrc_send_bbsmeta(int seclevel, const char *sysop) {
    char msgext[64], body[128];
    get_epoch_str(msgext, sizeof(msgext));
    snprintf(body, sizeof(body), "BBSMETA: SecLevel(%d) Sysop(%s)",
             seclevel, sysop ? sysop : "SysOp");
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, "",
                           "SERVER", msgext, "", body);
}

/* Send INFODSC/INFOTEL/INFOSSH/INFOWEB/INFOSYS packets from MRCBBS.DAT. */
static void mrc_send_info_fields(void) {
    char msgext[64], body[256];
    get_epoch_str(msgext, sizeof(msgext));

    if (g_bbscfg.description[0]) {
        snprintf(body, sizeof(body), "INFODSC: %s", g_bbscfg.description);
        mrc_send_packet_fields("CLIENT", g_mrc.bbs_name, "",
                               "SERVER", "ALL", msgext, body);
    }
    if (g_bbscfg.telnet[0]) {
        snprintf(body, sizeof(body), "INFOTEL: %s", g_bbscfg.telnet);
        mrc_send_packet_fields("CLIENT", g_mrc.bbs_name, "",
                               "SERVER", "ALL", msgext, body);
    }
    if (g_bbscfg.ssh[0]) {
        snprintf(body, sizeof(body), "INFOSSH: %s", g_bbscfg.ssh);
        mrc_send_packet_fields("CLIENT", g_mrc.bbs_name, "",
                               "SERVER", "ALL", msgext, body);
    }
    if (g_bbscfg.website[0]) {
        snprintf(body, sizeof(body), "INFOWEB: %s", g_bbscfg.website);
        mrc_send_packet_fields("CLIENT", g_mrc.bbs_name, "",
                               "SERVER", "ALL", msgext, body);
    }
    if (g_bbscfg.sysop[0]) {
        snprintf(body, sizeof(body), "INFOSYS: %s", g_bbscfg.sysop);
        mrc_send_packet_fields("CLIENT", g_mrc.bbs_name, "",
                               "SERVER", "ALL", msgext, body);
    }
}

/* Send a LOGOFF packet before disconnecting. */
static void mrc_send_logoff(void) {
    char msgext[64];
    get_epoch_str(msgext, sizeof(msgext));
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", msgext, g_mrc.room, "LOGOFF");
}

/* Send a SHUTDOWN packet so the server knows this client is going offline. */
static void mrc_send_shutdown(void) {
    char msgext[64];
    get_epoch_str(msgext, sizeof(msgext));
    mrc_send_packet_fields("CLIENT", g_mrc.bbs_name, "",
                           "SERVER", "ALL", msgext, "SHUTDOWN");
}

/* Send an IDENTIFY command with the given password. */
static void mrc_send_identify(const char *arg) {
    char body[128];
    if (!arg || !*arg) { bridge_write("|12ERR|07 Usage: /identify password"); return; }
    snprintf(body, sizeof(body), "IDENTIFY %s", arg);
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", "", body);
}

/* Send a REGISTER command to create an MRC account. */
static void mrc_send_register(const char *password, const char *email) {
    char body[256];
    if (!password || !*password) {
        bridge_write("|12ERR|07 Usage: !register password [email]");
        return;
    }
    if (email && *email)
        snprintf(body, sizeof(body), "REGISTER %s %s", password, email);
    else
        snprintf(body, sizeof(body), "REGISTER %s", password);
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", "", body);
}

/* Send an UPDATE command to modify an account parameter. */
static void mrc_send_update(const char *param, const char *value) {
    char body[128];
    if (!param || !*param) {
        bridge_write("|12ERR|07 Usage: !update param value");
        return;
    }
    if (value && *value)
        snprintf(body, sizeof(body), "UPDATE %s %s", param, value);
    else
        snprintf(body, sizeof(body), "UPDATE %s", param);
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", "", body);
}

/* Send a TRUST command (INFO/ADD/REM/etc). */
static void mrc_send_trust(const char *cmd) {
    char body[64];
    snprintf(body, sizeof(body), "TRUST %s", cmd ? cmd : "INFO");
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, body);
}

/* Request the WHOON listing from the server. */
static void mrc_send_whoon(void) {
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, "WHOON");
}

/* Request the CHATTERS listing from the server. */
static void mrc_send_chatters(void) {
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, "CHATTERS");
}

/* Request the USERLIST for the current room (for tab-completion state). */
static void mrc_send_userlist(void) {
    char msgext[64];
    get_epoch_str(msgext, sizeof(msgext));
    mrc_send_packet_fields("CLIENT", g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "ALL", msgext, "USERLIST");
}

/* Request the room LIST from the server. */
static void mrc_send_list(void) {
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, "LIST");
}

/* Request the USERS listing. */
static void mrc_send_users(void) {
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, "USERS");
}

/* Request the CHANNEL info. */
static void mrc_send_channel(void) {
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, "CHANNEL");
}

/* Request the list of connected BBSes. */
static void mrc_send_connected(void) {
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, "CONNECTED");
}

/* Request server INFO topic by numeric id. */
static void mrc_send_info_id(int id) {
    char body[32];
    snprintf(body, sizeof(body), "INFO %d", id);
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, body);
}

/* Request the server MOTD. */
static void mrc_send_motd(void) {
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, "MOTD");
}

/* If show_motd is enabled and not yet fired this session, request the MOTD. */
static void maybe_fire_auto_motd(void) {
    helper_log("maybe_fire_auto_motd: show_motd=%d fired=%d",
               g_bbscfg.show_motd, g_motd_fired);
    if (!g_bbscfg.show_motd) return;
    if (g_motd_fired) return;
    g_motd_fired = 1;
    bridge_write("|08[|14MOTD|08]|07 Loading message of the day...");
    g_show_motd_to_dos = 80;
    mrc_send_motd();
}

/* Request the server time. */
static void mrc_send_time_cmd(void) {
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, "TIME");
}

/* Request the server version. */
static void mrc_send_version(void) {
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, "VERSION");
}

/* Request server stats. */
static void mrc_send_stats(void) {
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, "STATS");
}

/* Request the BBS banners list. */
static void mrc_send_banners(void) {
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, "BANNERS");
}

/* Request a HELP topic (or the index if topic is NULL/empty). */
static void mrc_send_help_topic(const char *topic) {
    char body[64];
    if (topic && *topic)
        snprintf(body, sizeof(body), "HELP %s", topic);
    else
        snprintf(body, sizeof(body), "HELP");
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, body);
}

/* Send a NEWTOPIC command to change the current room's topic. */
static void mrc_send_newtopic(const char *topic) {
    char body[128];
    char safe_topic[64];
    if (!topic || !*topic) {
        bridge_write("|12ERR|07 Usage: /topic new_topic");
        return;
    }
    sanitize_field(safe_topic, sizeof(safe_topic), topic);
    snprintf(body, sizeof(body), "NEWTOPIC:%s:%s", g_mrc.room, safe_topic);
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, body);
}

/* Send a ROOMPASS command supplying the current room's password. */
static void mrc_send_roompass(const char *password) {
    char body[64];
    if (!password || !*password) {
        bridge_write("|12ERR|07 Usage: !roompass password");
        return;
    }
    snprintf(body, sizeof(body), "ROOMPASS %s", password);
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, body);
}

/* Query when a user was last seen (sent as !lastseen helper command). */
static void mrc_send_lastseen(const char *user) {
    char body[64];
    if (!user || !*user) {
        bridge_write("|12ERR|07 Usage: /last username");
        return;
    }
    snprintf(body, sizeof(body), "!lastseen %s", user);
    g_show_whoon_to_dos = 1;
    g_whoon_lines_left = 10;
    mrc_send_message_room(body);
}

/* Request the TOPICS history. */
static void mrc_send_topics_cmd(void) {
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, "TOPICS");
}

/* Send a STATUS command to set an account status flag (AFK, LASTSEEN, etc). */
static void mrc_send_status_cmd(const char *param, const char *value) {
    char body[96];
    if (!param || !*param) {
        bridge_write("|12ERR|07 Usage: !status param [value]  (e.g. AFK or LASTSEEN ON/OFF)");
        return;
    }
    if (value && *value)
        snprintf(body, sizeof(body), "STATUS %s %s", param, value);
    else
        snprintf(body, sizeof(body), "STATUS %s", param);
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, body);
}

/* Query or change a ROOMCONFIG setting for the current room. */
static void mrc_send_roomconfig(const char *param, const char *value) {
    char body[96];
    if (!param || !*param) {
        mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                               "SERVER", "", g_mrc.room, "ROOMCONFIG");
        return;
    }
    if (value && *value)
        snprintf(body, sizeof(body), "ROOMCONFIG %s %s", param, value);
    else
        snprintf(body, sizeof(body), "ROOMCONFIG %s", param);
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, body);
}

/* Request the server CHANGELOG. */
static void mrc_send_changelog(void) {
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, "CHANGELOG");
}

/* Query server message routing paths. */
static void mrc_send_routing(void) {
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "SERVER", "", g_mrc.room, "ROUTING");
}

/* Send a broadcast message visible in every room. */
static void mrc_send_broadcast(const char *text) {
    char handle[80], body[200];
    if (!g_in_room) { bridge_write("|12ERR|07 Not in room yet"); return; }
    build_handle_str(handle, sizeof(handle));
    snprintf(body, sizeof(body), "%s %s", handle, text ? text : "");
    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "", "", "", body);
    bridge_write("|14[BROADCAST]|07 %s", body);
}

/* Send a chat message to the current room. */
static void mrc_send_message_room(const char *text) {
    char handle[80], body[200];

    if (!g_in_room) { bridge_write("|12ERR|07 Not in room yet"); return; }

    build_handle_str(handle, sizeof(handle));
    if (g_text_color >= 0 && g_text_color <= 15)
        snprintf(body, sizeof(body), "%s |%02d%s", handle, g_text_color, text ? text : "");
    else
        snprintf(body, sizeof(body), "%s %s", handle, text ? text : "");

    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           "", "", g_mrc.room, body);
}

/* Send a direct (private) message to touser using the MRC DirectMsg form. */
static void mrc_send_message_user(const char *touser, const char *text) {
    char userbuf[32], handle[80], body[200];

    if (!g_in_room) { bridge_write("|12ERR|07 Not in room yet"); return; }

    underspace_copy(userbuf, sizeof(userbuf), touser);
    build_handle_str(handle, sizeof(handle));
    snprintf(body, sizeof(body), "(%s/DirectMsg) %s", handle, text ? text : "");

    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                           userbuf, "", "", body);
    bridge_write("|08(@|11%s|08/|14DirectMsg|08->|11%s|08)|07 %s",
                 g_mrc.nick[0] ? g_mrc.nick : g_mrc.user,
                 touser, text ? text : "");
}

/* ============================================================================
   MRC PROTOCOL - CONNECTION
   ============================================================================ */

/* Open a TCP (and optionally TLS) connection to an MRC server, handshake,
 * send client metadata, and join the initial room. */
static int mrc_connect_common(const char *server, int port,
                               const char *user, const char *room, int use_tls) {
    struct addrinfo hints, *res = NULL, *p = NULL;
    char portstr[16], roombuf[32];
    SOCKET s = INVALID_SOCKET;
    int rc;

    mrc_reset();
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);

    rc = getaddrinfo(server, portstr, &hints, &res);
    if (rc != 0) {
        bridge_write("STATUS DNSFAIL");
        helper_log("DNSFAIL %d", rc);
        return 0;
    }

    for (p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (s == INVALID_SOCKET) {
        bridge_write("STATUS CONNECTFAILED");
        helper_log("CONNECTFAILED");
        return 0;
    }

    g_mrc.sock = s;
    g_mrc.connected = 1;
    g_mrc.use_tls = use_tls;
    safe_copy(g_mrc.server, sizeof(g_mrc.server), server);
    g_mrc.port = port;
    /* Remember for QUICKSTATS (persists across reconnects). */
    safe_copy(g_last_server, sizeof(g_last_server), server);
    g_last_port = port;
    safe_copy(g_mrc.bbs_name,    sizeof(g_mrc.bbs_name),    g_bbscfg.bbs_name);
    safe_copy(g_mrc.bbs_pretty,  sizeof(g_mrc.bbs_pretty),  g_bbscfg.bbs_pretty);
    safe_copy(g_mrc.version_info, sizeof(g_mrc.version_info), ANETMRC_VERSION_INFO);
    /* Strip pipe color codes from the username before sending. */
    {
        char clean[64];
        size_t i = 0;
        const char *s = user;
        while (*s && i + 1 < sizeof(clean)) {
            if (*s == '|' && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2])) {
                s += 3; continue;
            }
            clean[i++] = *s++;
        }
        clean[i] = 0;
        underspace_copy(g_mrc.user, sizeof(g_mrc.user), clean);
    }
    safe_copy(g_mrc.nick, sizeof(g_mrc.nick), g_mrc.user);
    underspace_copy(roombuf, sizeof(roombuf), room);
    strip_hash(roombuf);
    safe_copy(g_mrc.room, sizeof(g_mrc.room), roombuf);

    if (use_tls) {
        if (!tls_handshake(server)) {
            bridge_write("STATUS TLSFAIL");
            helper_log("TLSFAIL");
            mrc_reset();
            return 0;
        }
    }

    {
        u_long one = 1;
        ioctlsocket(g_mrc.sock, FIONBIO, &one);
    }

    mrc_send_handshake();
    write_status_and_log("CONNECTED");
    helper_ui_log("CONNECTED %s:%d", server, port);

    mrc_send_capabilities();
    mrc_send_userip("0.0.0.0");
    mrc_send_termsize(80, 24);
    mrc_send_bbsmeta(100, g_bbscfg.sysop);
    mrc_send_info_fields();

    mrc_send_newroom(room);
    mrc_send_iamhere("ACTIVE");

    /* Pre-populate room name so DOS header shows immediately. */
    bridge_write("ROOM %s", roombuf);

    bridge_write("|14NOTICE|07 Type |15/help|07 for commands");
    bridge_write("|14NOTICE|07 Use |15/identify password|07 for MRC Trust");
    return 1;
}

/* ============================================================================
   MRC PROTOCOL - PACKET PARSING
   ============================================================================ */

/* Split a tilde-separated MRC packet line in place into its 7 fields. */
static int split_mrc_packet(char *line, char *f[7]) {
    int i;
    char *p = line;
    for (i = 0; i < 6; ++i) {
        f[i] = p;
        p = strchr(p, '~');
        if (!p) return 0;
        *p = 0;
        ++p;
    }
    /* Trailing ~ on field 7 is optional. */
    f[6] = p;
    p = strchr(p, '~');
    if (p) *p = 0;
    return 1;
}

/* Parse and dispatch a single incoming MRC packet line. */
static void handle_mrc_packet(char *line) {
    char *f[7];
    char from_user[64], from_site[128], from_room[64];
    char to_user[64], msgext[64], to_room[64], body[300];

    if (!line || !*line) return;

    if (strcmp(line, "SERVER") == 0) return;

    if (strncmp(line, "HEARTBEAT_TIMEOUT", 17) == 0 ||
        strncmp(line, "BAD_REQUEST", 11) == 0 ||
        strncmp(line, "BAD_HANDSHAKE", 13) == 0) {
        bridge_write("|12DISCONNECT|07 %s", line);
        write_status_and_log("DISCONNECTED");
        mrc_reset();
        return;
    }

    if (!strchr(line, '~')) {
        LOG_V("IGNORED NON-MRC: %s", line);
        return;
    }

    if (!split_mrc_packet(line, f)) {
        LOG_V("BAD MRC PACKET: %s", line);
        return;
    }

    safe_copy(from_user, sizeof(from_user), f[0]);
    safe_copy(from_site, sizeof(from_site), f[1]);
    safe_copy(from_room, sizeof(from_room), f[2]);
    safe_copy(to_user,   sizeof(to_user),   f[3]);
    safe_copy(msgext,    sizeof(msgext),     f[4]);
    safe_copy(to_room,   sizeof(to_room),    f[5]);
    safe_copy(body,      sizeof(body),       f[6]);

    deunderscore(from_user);
    deunderscore(from_site);
    deunderscore(from_room);
    deunderscore(to_user);
    deunderscore(msgext);
    deunderscore(to_room);

    /* MOTD content is just a stream of server-originated lines.  Let them
     * flow through the standard SERVER routing below (which tags them with
     * "|10SERVER|07") so we don't mis-label genuine server notices (room
     * joins/parts, announcements) as MOTD once the capture window is open. */

    /* ---- SERVER → CLIENT control traffic ---- */
    if (strcmp(from_user, "SERVER") == 0) {

        if (stricmp(body, "PING") == 0) {
            mrc_send_imalive();
            LOG_V("PING/IMALIVE");
            return;
        }

        /* PONG reply: server echoes our epoch msgext back for latency calc. */
        if (stricmp(body, "PONG") == 0) {
            double sent = atof(msgext);
            if (sent > 1000000.0) {
                LARGE_INTEGER freq, count;
                double now = (double)time(NULL);
                if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&count) &&
                    freq.QuadPart > 0) {
                    double frac = (double)(count.QuadPart % freq.QuadPart) /
                                  (double)freq.QuadPart;
                    now += frac;
                }
                {
                    int ms = (int)((now - sent) * 1000.0);
                    if (ms >= 0 && ms < 10000) {
                        bridge_write("LATENCY %dms", ms);
                        LOG_V("PONG latency %dms", ms);
                    }
                }
            }
            return;
        }

        /* Server confirmed room placement. */
        if (strncmp(body, "USERROOM:", 9) == 0) {
            safe_copy(g_mrc.room, sizeof(g_mrc.room), body + 9);
            strip_hash(g_mrc.room);
            g_in_room = 1;
            g_show_motd_to_dos = 0;
            bridge_write("ROOM %s", g_mrc.room);
            bridge_write("|10READY|07 You can chat now. Type /help for commands.");
            bridge_write("|14NOTICE|07 Use |15/identify password|07 for MRC Trust");
            helper_log("USERROOM: placed in room %s", g_mrc.room);
            {
                const char *jmsg = (g_enterroom_msg[0] && has_visible_text(g_enterroom_msg))
                                   ? g_enterroom_msg : "has joined";
                char handle[80];
                build_handle_str(handle, sizeof(handle));
                mrc_send_room_notification(g_mrc.room, jmsg);
                /* Local echo: NOTME isn't sent back to the sender. */
                bridge_write("|08* |14%s %s|07", handle, jmsg);
            }
            return;
        }

        /* Server confirmed nick (may have been altered to avoid collision). */
        if (strncmp(body, "USERNICK:", 9) == 0) {
            safe_copy(g_mrc.nick, sizeof(g_mrc.nick), body + 9);
            deunderscore(g_mrc.nick);
            bridge_write("|11NICK|07 Set to: %s", g_mrc.nick);
            return;
        }

        /* Room user list: update USERS count and CHATLIST for tab complete. */
        if (strncmp(body, "USERLIST:", 9) == 0) {
            const char *ul = body + 9;
            int cnt = 0;
            const char *p;
            if (to_room[0]) mark_in_room_if_matches(to_room);
            if (ul && *ul) { cnt = 1; for (p = ul; *p; ++p) if (*p == ',') ++cnt; }
            bridge_write("USERS %d", cnt);
            if (ul && *ul) bridge_write("CHATLIST %s", ul);
            if (g_show_chatters_to_dos) {
                g_show_chatters_to_dos = 0;
            }
            helper_ui_log("USERS: %s", ul);
            return;
        }

        /* Room topic update: refresh the stationary header only. */
        if (strncmp(body, "ROOMTOPIC:", 10) == 0) {
            const char *after = body + 10;
            const char *colon = strchr(after, ':');
            const char *topic_text = colon ? colon + 1 : after;
            bridge_write("TOPIC %s", topic_text);
            if (to_room[0]) mark_in_room_if_matches(to_room);
            return;
        }

        if (strncmp(body, "NOTIFY:", 7) == 0) {
            bridge_write("|14NOTICE|07 %s", body + 7);
            return;
        }

        if (strncmp(body, "TERMINATE:", 10) == 0) {
            bridge_write("|12TERMINATED|07 %s", body + 10);
            write_status_and_log("OFFLINE");
            mrc_reset();
            return;
        }

        /* CTCP request/reply via ctcp_echo_channel. */
        if (stricmp(to_room, "ctcp_echo_channel") == 0 ||
            stricmp(from_room, "ctcp_echo_channel") == 0) {
            if (strncmp(body, "[CTCP]", 6) == 0) {
                char ctcp_sender[32], ctcp_target[32], ctcp_cmd[32], ctcp_args[64];
                ctcp_sender[0] = ctcp_target[0] = ctcp_cmd[0] = ctcp_args[0] = 0;
                sscanf(body + 7, "%31s %31s %31s %63[^\n]",
                       ctcp_sender, ctcp_target, ctcp_cmd, ctcp_args);
                /* Respond only if we are the target. */
                if (to_user[0] == 0 ||
                    stricmp(to_user,      g_mrc.user) == 0 ||
                    stricmp(to_user,      g_mrc.nick) == 0 ||
                    stricmp(ctcp_target,  g_mrc.user) == 0 ||
                    stricmp(ctcp_target,  g_mrc.nick) == 0) {
                    char reply_body[220];
                    const char *mynick = g_mrc.nick[0] ? g_mrc.nick : g_mrc.user;
                    if (stricmp(ctcp_cmd, "VERSION") == 0) {
                        snprintf(reply_body, sizeof(reply_body),
                                 "[CTCP-REPLY] %s VERSION " ANETMRC_VERSION_INFO, mynick);
                    } else if (stricmp(ctcp_cmd, "TIME") == 0) {
                        time_t t = time(NULL);
                        struct tm *tm = localtime(&t);
                        char tbuf[32] = "unknown";
                        if (tm) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
                        snprintf(reply_body, sizeof(reply_body),
                                 "[CTCP-REPLY] %s TIME %s", mynick, tbuf);
                    } else if (stricmp(ctcp_cmd, "PING") == 0) {
                        snprintf(reply_body, sizeof(reply_body),
                                 "[CTCP-REPLY] %s PING %s", mynick,
                                 ctcp_args[0] ? ctcp_args : "pong");
                    } else if (stricmp(ctcp_cmd, "CLIENTINFO") == 0) {
                        snprintf(reply_body, sizeof(reply_body),
                                 "[CTCP-REPLY] %s CLIENTINFO VERSION TIME PING CLIENTINFO",
                                 mynick);
                    } else {
                        return;
                    }
                    mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name,
                                           "ctcp_echo_channel", ctcp_sender, "",
                                           "ctcp_echo_channel", reply_body);
                    bridge_write("|08[|14CTCP|08]|07 %s queried %s", ctcp_sender, ctcp_cmd);
                }
            } else if (strncmp(body, "[CTCP-REPLY]", 12) == 0) {
                bridge_write("|08[|14CTCP-REPLY|08]|07 %s", body + 13);
            }
            return;
        }

        /* Welcome banner marks post-identify room join; echo locally and
         * fire the auto-MOTD here. The genuine join banner is specifically
         * "Welcome to #room, the room is now occupied by N user(s)" — other
         * MOTD lines also contain "Welcome to" and would previously fire
         * the echo repeatedly. Gate on both phrases and skip if USERROOM
         * already delivered the echo. */
        if (strstr(body, "Welcome to") != NULL &&
            strstr(body, "the room is") != NULL) {
            int already_in_room = g_in_room;
            helper_log("WELCOME banner: %s", body);
            g_in_room = 1;
            bridge_write("|10SERVER|07 %s", body);
            if (!already_in_room) {
                const char *jmsg = (g_enterroom_msg[0] && has_visible_text(g_enterroom_msg))
                                   ? g_enterroom_msg : "has joined";
                char handle[80];
                build_handle_str(handle, sizeof(handle));
                if (g_mrc.room[0])
                    mrc_send_room_notification(g_mrc.room, jmsg);
                bridge_write("|08* |14%s %s|07", handle, jmsg);
            }
            maybe_fire_auto_motd();
            return;
        }

        /* Unsolicited WHOON-type lines pass through an active listing,
         * otherwise get logged quietly. */
        if (strstr(body, "Chatters") != NULL || strstr(body, "ssl:") != NULL ||
            strstr(body, "Federated") != NULL) {
            if (g_show_whoon_to_dos) {
                if (body[0]) bridge_write_listing("%s", body);
                if (strstr(body, "Federated") || --g_whoon_lines_left <= 0) {
                    g_show_whoon_to_dos = 0;
                    g_whoon_lines_left = 0;
                    g_rooms_notme_count = 0;
                    g_chatters_notme_count = 0;
                }
            } else {
                helper_ui_log("%s", body);
            }
            return;
        }

        /* Route /whoon, /list, /chatters server text lines to the door,
         * picking the formatter that matches the active listing. */
        if (g_show_whoon_to_dos) {
            helper_log("WHOON route: chatters_c=%d rooms_c=%d body=%.60s",
                       g_chatters_notme_count, g_rooms_notme_count, body);
            if (body[0]) {
                char packed[HELPER_LINE_MAX];
                if (!write_legend_if_match(body)) {
                    if (g_rooms_notme_count > 0) {
                        format_list_line(packed, sizeof(packed), body);
                        --g_rooms_notme_count;
                    } else if (g_chatters_notme_count > 0) {
                        format_chatters_line(packed, sizeof(packed), body);
                        --g_chatters_notme_count;
                    } else {
                        /* Generic whoon-routed server line (e.g. !games).
                         * Preserve column spacing — compact_line would cap
                         * runs at 6 spaces and destroy the table. */
                        passthrough_truncate(packed, sizeof(packed), body, 78);
                    }
                    bridge_write_listing("%s", packed);
                }
            }
            if (--g_whoon_lines_left <= 0) {
                g_show_whoon_to_dos = 0;
                g_whoon_lines_left = 0;
                g_rooms_notme_count = 0;
                g_chatters_notme_count = 0;
            }
            return;
        }

        /* SERVER→NOTME: join/part notices, and header/legend lines during
         * an active /chatters or /list listing. */
        if (stricmp(to_user, "NOTME") == 0) {
            helper_log("NOTME route: chatters_c=%d rooms_c=%d body=%.60s",
                       g_chatters_notme_count, g_rooms_notme_count, body);
            if (!g_in_room) return;
            if (g_chatters_notme_count > 0) {
                if (body[0]) {
                    char packed[HELPER_LINE_MAX];
                    if (!write_legend_if_match(body)) {
                        format_chatters_line(packed, sizeof(packed), body);
                        bridge_write_listing("%s", packed);
                    }
                }
                g_chatters_notme_count--;
            } else if (g_rooms_notme_count > 0) {
                if (body[0]) {
                    char packed[HELPER_LINE_MAX];
                    if (!write_legend_if_match(body)) {
                        format_list_line(packed, sizeof(packed), body);
                        bridge_write_listing("%s", packed);
                    }
                }
                g_rooms_notme_count--;
            } else {
                bridge_write("|08* |14%s|07", body);
            }
            return;
        }

        /* General server messages addressed to CLIENT or us. Suppress on
         * this node if another node has WHOON active (broadcast leakage). */
        if (to_user[0] == 0 ||
            stricmp(to_user, "CLIENT") == 0 ||
            stricmp(to_user, g_mrc.user) == 0 ||
            stricmp(to_user, g_mrc.nick) == 0) {
            if (!g_show_whoon_to_dos && any_node_whoon_active()) {
                return;
            }
            helper_log("SRV route to_user=%s chatters_c=%d rooms_c=%d body=%.60s",
                       to_user, g_chatters_notme_count, g_rooms_notme_count, body);
            if (strncmp(body, "BANNER:", 7) == 0) {
                bridge_write("|14[BANNER]|07 %s", body + 7);
            } else {
                bridge_write("|10SERVER|07 %s", body);
            }
            return;
        }
        return;
    }

    /* ---- CTCP: user-to-user via ctcp_echo_channel ---- */
    if (stricmp(to_room, "ctcp_echo_channel") == 0 ||
        stricmp(from_room, "ctcp_echo_channel") == 0) {
        if (strncmp(body, "[CTCP]", 6) == 0) {
            char ctcp_sender[32], ctcp_target[32], ctcp_cmd[32], ctcp_args[64];
            ctcp_sender[0] = ctcp_target[0] = ctcp_cmd[0] = ctcp_args[0] = 0;
            sscanf(body + 7, "%31s %31s %31s %63[^\n]",
                   ctcp_sender, ctcp_target, ctcp_cmd, ctcp_args);
            if (!ctcp_target[0] ||
                stricmp(ctcp_target, g_mrc.user) == 0 ||
                stricmp(ctcp_target, g_mrc.nick) == 0) {
                char reply_body[220];
                if (stricmp(ctcp_cmd, "VERSION") == 0) {
                    snprintf(reply_body, sizeof(reply_body),
                             "[CTCP-REPLY] %s VERSION " ANETMRC_VERSION_INFO,
                             g_mrc.nick[0] ? g_mrc.nick : g_mrc.user);
                } else if (stricmp(ctcp_cmd, "TIME") == 0) {
                    time_t t = time(NULL);
                    struct tm *tm = localtime(&t);
                    char tbuf[32] = "unknown";
                    if (tm) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
                    snprintf(reply_body, sizeof(reply_body),
                             "[CTCP-REPLY] %s TIME %s",
                             g_mrc.nick[0] ? g_mrc.nick : g_mrc.user, tbuf);
                } else if (stricmp(ctcp_cmd, "PING") == 0) {
                    snprintf(reply_body, sizeof(reply_body),
                             "[CTCP-REPLY] %s PING %s",
                             g_mrc.nick[0] ? g_mrc.nick : g_mrc.user,
                             ctcp_args[0] ? ctcp_args : "pong");
                } else if (stricmp(ctcp_cmd, "CLIENTINFO") == 0) {
                    snprintf(reply_body, sizeof(reply_body),
                             "[CTCP-REPLY] %s CLIENTINFO VERSION TIME PING CLIENTINFO",
                             g_mrc.nick[0] ? g_mrc.nick : g_mrc.user);
                } else {
                    return;
                }
                mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name,
                                       "ctcp_echo_channel", ctcp_sender, "",
                                       "ctcp_echo_channel", reply_body);
                bridge_write("|08[|14CTCP|08]|07 %s queried %s", ctcp_sender, ctcp_cmd);
            }
        } else if (strncmp(body, "[CTCP-REPLY]", 12) == 0) {
            bridge_write("|08[|14CTCP-REPLY|08]|07 %s", body + 13);
        }
        return;
    }

    /* ---- Join/Part notifications (NOTME = room broadcast) ---- */
    if (stricmp(to_user, "NOTME") == 0) {
        if (!g_in_room) return;
        /* NOTME join/part lines do not count as mentions. */
        bridge_write("|08* |14%s|07", body);
        return;
    }

    /* ---- Direct messages addressed to our user ---- */
    if (to_user[0] &&
        stricmp(to_user, "SERVER") != 0 &&
        stricmp(to_user, "CLIENT") != 0) {
        /* Body arrives wrapped as "(senderHandle/DirectMsg) text" — unwrap
         * to get the sender handle and plain text. */
        const char *shown = body;
        char dm_sender[64];
        safe_copy(dm_sender, sizeof(dm_sender), from_user);

        if (shown[0] == '*' && shown[1] == ' ') shown += 2;

        if (shown[0] == '(') {
            const char *slash = strchr(shown, '/');
            const char *close = strstr(shown, ") ");
            if (slash && close && slash < close) {
                int slen = (int)(slash - shown - 1);
                if (slen > 0 && slen < (int)sizeof(dm_sender)) {
                    memcpy(dm_sender, shown + 1, (size_t)slen);
                    dm_sender[slen] = 0;
                }
                shown = close + 2;
            } else {
                const char *close2 = strstr(shown, ") ");
                if (close2) shown = close2 + 2;
            }
        } else {
            /* Strip bare from_user prefix when no wrapper is present. */
            size_t ulen = strlen(from_user);
            if (ulen > 0 && _strnicmp(body, from_user, ulen) == 0 &&
                (body[ulen] == ' ' || body[ulen] == 0))
                shown = (body[ulen] == ' ') ? body + ulen + 1 : body + ulen;
        }
        /* CTCP request routed as DirectMsg. */
        if (strncmp(shown, "[CTCP]", 6) == 0) {
            const char *p = shown + 6;
            char ctcp_target[32], ctcp_cmd[32], ctcp_args[64];
            ctcp_target[0] = ctcp_cmd[0] = ctcp_args[0] = 0;
            if (*p == ' ') ++p;
            {
                char skip_sender[32];
                skip_sender[0] = 0;
                sscanf(p, "%31s %31s %31s %63[^\n]",
                       skip_sender, ctcp_target, ctcp_cmd, ctcp_args);
            }
            if (ctcp_target[0] == 0 ||
                stricmp(ctcp_target, g_mrc.user) == 0 ||
                stricmp(ctcp_target, g_mrc.nick) == 0) {
                const char *mynick = g_mrc.nick[0] ? g_mrc.nick : g_mrc.user;
                char reply_body[220];
                if (stricmp(ctcp_cmd, "VERSION") == 0) {
                    snprintf(reply_body, sizeof(reply_body),
                             "[CTCP-REPLY] %s VERSION " ANETMRC_VERSION_INFO, mynick);
                } else if (stricmp(ctcp_cmd, "TIME") == 0) {
                    time_t t = time(NULL);
                    struct tm *tmv = localtime(&t);
                    char tbuf[32] = "unknown";
                    if (tmv) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tmv);
                    snprintf(reply_body, sizeof(reply_body),
                             "[CTCP-REPLY] %s TIME %s", mynick, tbuf);
                } else if (stricmp(ctcp_cmd, "PING") == 0) {
                    snprintf(reply_body, sizeof(reply_body),
                             "[CTCP-REPLY] %s PING %s", mynick,
                             ctcp_args[0] ? ctcp_args : "pong");
                } else if (stricmp(ctcp_cmd, "CLIENTINFO") == 0) {
                    snprintf(reply_body, sizeof(reply_body),
                             "[CTCP-REPLY] %s CLIENTINFO VERSION TIME PING CLIENTINFO",
                             mynick);
                } else {
                    return;
                }
                mrc_send_message_user(dm_sender, reply_body);
                bridge_write("|08[|14CTCP|08]|07 %s queried %s", dm_sender, ctcp_cmd);
            }
            return;
        }

        if (strncmp(shown, "[CTCP-REPLY]", 12) == 0) {
            bridge_write("|08[|14CTCP-REPLY|08]|07 %s", shown + 13);
            return;
        }

        bridge_write("|08(@|11%s|08/|14DirectMsg|08)|07 %s", dm_sender, shown);
        return;
    }

    /* ---- General room chat ---- */
    {
        const char *shown = body;
        size_t ulen = strlen(from_user);

        if (!g_in_room) return;

        /* Action/emote form: body begins with "* ". */
        if (body[0] == '*' && body[1] == ' ') {
            bridge_write("|13%s|07", body);
            if (mention_check(from_user, body))
                bridge_write("MENTION %s\t%s", from_user, body);
            return;
        }

        if (ulen > 0 && _strnicmp(body, from_user, ulen) == 0 &&
            (body[ulen] == ' ' || body[ulen] == 0)) {
            shown = (body[ulen] == ' ') ? body + ulen + 1 : body + ulen;
            bridge_write("|08[|11%s|08]|07 %s", from_user, shown);
        } else {
            bridge_write("%s", body);
        }

        /* Emit MENTION after the display line so the door counts it once. */
        if (mention_check(from_user, body)) {
            helper_log("MENTION fired from=%s body=%.80s", from_user, shown);
            bridge_write("MENTION %s\t%s", from_user, shown);
        }
    }
}

/* ============================================================================
   HELPER SEND - ! COMMANDS
   ============================================================================ */

/* Dispatch a !command (msg is the text after the '!'). */
static void handle_helper_command(const char *msg) {

    if (strncmp(msg, "register", 8) == 0) {
        const char *rest = msg + 8;
        char pass[32], email[128];
        pass[0] = email[0] = 0;
        if (*rest == ' ') ++rest;
        if (sscanf(rest, "%31s %127s", pass, email) >= 1)
            mrc_send_register(pass, email[0] ? email : NULL);
        else
            mrc_send_register(NULL, NULL);
        return;
    }

    if (strncmp(msg, "identify", 8) == 0) {
        const char *rest = msg + 8;
        if (*rest == ' ') ++rest;
        mrc_send_identify(rest);
        return;
    }

    if (strncmp(msg, "update", 6) == 0) {
        const char *rest = msg + 6;
        char param[32], value[64];
        param[0] = value[0] = 0;
        if (*rest == ' ') ++rest;
        sscanf(rest, "%31s %63s", param, value);
        mrc_send_update(param, value[0] ? value : NULL);
        return;
    }

    if (strncmp(msg, "trust", 5) == 0) {
        const char *rest = msg + 5;
        if (*rest == ' ') ++rest;
        mrc_send_trust(*rest ? rest : "INFO");
        return;
    }

    if (strncmp(msg, "roompass", 8) == 0) {
        const char *rest = msg + 8;
        if (*rest == ' ') ++rest;
        mrc_send_roompass(*rest ? rest : NULL);
        return;
    }

    if (strncmp(msg, "lastseen", 8) == 0) {
        const char *rest = msg + 8;
        if (*rest == ' ') ++rest;
        mrc_send_lastseen(*rest ? rest : NULL);
        return;
    }

    if (strcmp(msg, "topics") == 0) {
        mrc_send_topics_cmd();
        return;
    }

    if (strncmp(msg, "status", 6) == 0) {
        const char *rest = msg + 6;
        char param[32], value[64];
        param[0] = value[0] = 0;
        if (*rest == ' ') ++rest;
        sscanf(rest, "%31s %63[^\n]", param, value);
        mrc_send_status_cmd(param, value[0] ? value : NULL);
        return;
    }

    if (strncmp(msg, "afk", 3) == 0) {
        const char *rest = msg + 3;
        if (*rest == ' ') ++rest;
        mrc_send_status_cmd("AFK", *rest ? rest : NULL);
        return;
    }

    if (strncmp(msg, "roomconfig", 10) == 0) {
        const char *rest = msg + 10;
        char param[32], value[64];
        param[0] = value[0] = 0;
        if (*rest == ' ') ++rest;
        sscanf(rest, "%31s %63[^\n]", param, value);
        mrc_send_roomconfig(param, value[0] ? value : NULL);
        return;
    }

    if (strcmp(msg, "info changelog") == 0 || strcmp(msg, "changelog") == 0) {
        mrc_send_changelog();
        return;
    }

    if (strcmp(msg, "info routing") == 0 || strcmp(msg, "routing") == 0) {
        mrc_send_routing();
        return;
    }

    if (strcmp(msg, "list") == 0) {
        g_show_whoon_to_dos = 1;
        g_whoon_lines_left = 512;
        g_rooms_notme_count = 64;
        g_chatters_notme_count = 0;
        mrc_send_message_room("!list");
        return;
    }

    if (strcmp(msg, "chatters") == 0) {
        g_show_whoon_to_dos = 1;
        g_whoon_lines_left  = 512;
        g_show_chatters_to_dos = 1;
        g_chatters_notme_count = 64;
        g_rooms_notme_count = 0;
        mrc_send_chatters();
        return;
    }

    if (strcmp(msg, "whoon") == 0) {
        g_show_whoon_to_dos = 1;
        g_whoon_lines_left = 512;
        g_rooms_notme_count = 0;
        g_chatters_notme_count = 0;
        mrc_send_whoon();
        return;
    }

    if (strcmp(msg, "users") == 0) {
        mrc_send_users();
        return;
    }

    if (strcmp(msg, "motd") == 0) {
        bridge_write("|08[|14MOTD|08]|07 Loading message of the day...");
        g_show_motd_to_dos = 80;
        mrc_send_motd();
        return;
    }

    if (strcmp(msg, "time") == 0) {
        mrc_send_time_cmd();
        return;
    }

    if (strcmp(msg, "version") == 0) {
        mrc_send_version();
        return;
    }

    if (strcmp(msg, "stats") == 0) {
        mrc_send_stats();
        return;
    }

    if (strcmp(msg, "banners") == 0) {
        mrc_send_banners();
        return;
    }

    if (strcmp(msg, "bbses") == 0 || strcmp(msg, "connected") == 0) {
        g_show_whoon_to_dos = 1;
        g_whoon_lines_left = 512;
        mrc_send_connected();
        return;
    }

    /* Unknown !command: forward to the server as a room message and
     * capture its response via WHOON routing. */
    {
        char raw[256];
        if (!g_in_room) { bridge_write("|12ERR|07 Not in room"); return; }
        snprintf(raw, sizeof(raw), "!%s", msg);
        g_show_whoon_to_dos = 1;
        g_whoon_lines_left = 32;
        mrc_send_message_room(raw);
    }
}

/* ============================================================================
   QUICKSTATS - lightweight bare-TCP stats query
   ============================================================================ */

/* Fetch lightweight stats via a bare-TCP "~\n" query, or via STATS if already
 * connected. Protocol: server replies with "d1 d2 d3 d4\n" then closes. */
static void do_quickstats(void) {
    SOCKET s = INVALID_SOCKET;
    struct addrinfo hints, *res = NULL, *p = NULL;
    char portstr[16];
    char buf[128];
    int n;
    DWORD timeout_ms = 3000;

    if (g_mrc.connected) {
        mrc_send_stats();
        return;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", g_last_port);

    helper_ui_log("QUICKSTATS %s:%d", g_last_server, g_last_port);

    if (getaddrinfo(g_last_server, portstr, &hints, &res) != 0) {
        bridge_write("STATS_RESULT 0 0 0 0");
        helper_ui_log("QUICKSTATS DNS failed");
        return;
    }

    for (p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (s == INVALID_SOCKET) {
        bridge_write("STATS_RESULT 0 0 0 0");
        helper_ui_log("QUICKSTATS connect failed");
        return;
    }

    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout_ms, sizeof(timeout_ms));

    send(s, "~\n", 2, 0);

    memset(buf, 0, sizeof(buf));
    n = recv(s, buf, sizeof(buf) - 1, 0);
    closesocket(s);

    if (n > 0) {
        while (n > 0 && (buf[n-1] == '\r' || buf[n-1] == '\n' || buf[n-1] == ' '))
            buf[--n] = 0;
        bridge_write("STATS_RESULT %s", buf);
        helper_ui_log("QUICKSTATS: %s", buf);
    } else {
        bridge_write("STATS_RESULT 0 0 0 0");
        helper_ui_log("QUICKSTATS no response");
    }
}

/* ============================================================================
   MAIN LOOP - BRIDGE COMMAND HANDLER
   ============================================================================ */

/* Parse one command line from the DOS bridge file and dispatch it. */
static void handle_bridge_command(const char *line) {
    char cmd[64];
    helper_log("CMD: %s", line);
    if (sscanf(line, "%63s", cmd) != 1) return;

    /* ---- CONNECT ---- */
    if (strcmp(cmd, "CONNECT") == 0) {
        char server[128], user[64], room[64];
        int port = 0, tls = 0;
        server[0] = user[0] = room[0] = 0;
        if (sscanf(line, "CONNECT %127s %d %d %63s %63s",
                   server, &port, &tls, user, room) == 5) {
            /* MRCBBS.DAT overrides the door's default server/port. */
            if (g_bbscfg.server[0])
                safe_copy(server, sizeof(server), g_bbscfg.server);
            if (g_bbscfg.port > 0)
                port = g_bbscfg.port;
            tls = 0;
            mrc_connect_common(server, port, user, room, tls);
        } else {
            bridge_write("STATUS BADCMD");
        }

    /* ---- QUIT / DISCONNECT ---- */
    } else if (strcmp(cmd, "QUIT") == 0) {
        if (g_mrc.connected) {
            const char *lmsg = (g_leaveroom_msg[0] && has_visible_text(g_leaveroom_msg))
                               ? g_leaveroom_msg : "disconnected";
            mrc_send_leave_chat(lmsg);
            /* Clear AFK before disconnecting so we don't return AFK next login. */
            mrc_send_status_cmd("AFK", "OFF");
            mrc_send_iamhere("ACTIVE");
            mrc_send_logoff();
            mrc_send_shutdown();
        }
        bridge_write("STATUS OFFLINE");
        mrc_reset();

    /* ---- SEND (room chat; also intercepts !commands and /list/chatters) ---- */
    } else if (strcmp(cmd, "SEND") == 0) {
        const char *msg = (strlen(line) > 5) ? line + 5 : "";

        if (msg[0] == '!') {
            handle_helper_command(msg + 1);
            return;
        }

        if (strcmp(msg, "/list") == 0 || strcmp(msg, "!list") == 0) {
            g_show_whoon_to_dos = 1;
            g_whoon_lines_left = 512;
            g_rooms_notme_count = 64;
            g_chatters_notme_count = 0;
            mrc_send_list(); return;
        }
        if (strcmp(msg, "/chatters") == 0 || strcmp(msg, "!chatters") == 0) {
            g_show_whoon_to_dos = 1;
            g_whoon_lines_left  = 512;
            g_show_chatters_to_dos = 1;
            g_chatters_notme_count = 64;
            g_rooms_notme_count = 0;
            mrc_send_chatters(); return;
        }

        g_last_cmd_time = GetTickCount64();
        mrc_send_message_room(msg);

    /* ---- JOIN room ---- */
    } else if (strcmp(cmd, "JOIN") == 0) {
        char roombuf[32];
        const char *arg = (strlen(line) > 5) ? line + 5 : "lobby";
        underspace_copy(roombuf, sizeof(roombuf), arg);
        strip_hash(roombuf);
        if (!roombuf[0]) safe_copy(roombuf, sizeof(roombuf), "lobby");
        g_in_room = 0;
        mrc_send_newroom(roombuf);
        bridge_write("|11JOIN|07 Joining %s...", roombuf);

    /* ---- MSG user text (private message) ---- */
    } else if (strcmp(cmd, "MSG") == 0) {
        const char *args = (strlen(line) > 4) ? line + 4 : "";
        const char *sp;
        char target[64];
        while (*args == ' ') ++args;
        sp = strchr(args, ' ');
        if (sp && sp > args) {
            int tlen = (int)(sp - args);
            if (tlen > (int)sizeof(target) - 1) tlen = (int)sizeof(target) - 1;
            memcpy(target, args, (size_t)tlen);
            target[tlen] = 0;
            g_last_cmd_time = GetTickCount64();
            mrc_send_message_user(target, sp + 1);
        } else {
            bridge_write("|12ERR|07 Usage: /msg user text");
        }

    /* ---- BROADCAST text ---- */
    } else if (strcmp(cmd, "BROADCAST") == 0) {
        const char *text = (strlen(line) > 10) ? line + 10 : "";
        mrc_send_broadcast(text);

    /* ---- ME: action/emote sent as room message ---- */
    } else if (strcmp(cmd, "ME") == 0) {
        const char *text = (strlen(line) > 3) ? line + 3 : "";
        if (*text == ' ') ++text;
        if (*text && g_in_room) {
            char body[220];
            const char *nick = g_mrc.nick[0] ? g_mrc.nick : g_mrc.user;
            snprintf(body, sizeof(body), "* %s %s", nick, text);
            g_last_cmd_time = GetTickCount64();
            mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                                   "", "", g_mrc.room, body);
        } else if (!g_in_room) {
            bridge_write("|12ERR|07 Not in room yet");
        }

    /* ---- CTCP: send a CTCP query via ctcp_echo_channel ---- */
    } else if (strcmp(cmd, "CTCP") == 0) {
        /* format: CTCP target COMMAND [args] */
        const char *rest = (strlen(line) > 5) ? line + 5 : "";
        if (*rest == ' ') ++rest;
        if (*rest && g_in_room) {
            char tgt[32], ctcpcmd[32], body[220];
            tgt[0] = ctcpcmd[0] = 0;
            sscanf(rest, "%31s %31s", tgt, ctcpcmd);
            if (tgt[0] && ctcpcmd[0]) {
                const char *nick = g_mrc.nick[0] ? g_mrc.nick : g_mrc.user;
                snprintf(body, sizeof(body), "[CTCP] %s %s %s", nick, tgt, ctcpcmd);
                mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name,
                                       "ctcp_echo_channel", tgt, "",
                                       "ctcp_echo_channel", body);
                bridge_write("|08[|14CTCP|08]|07 Sent %s query to %s", ctcpcmd, tgt);
            } else {
                bridge_write("|12ERR|07 Usage: /ctcp target COMMAND (VERSION/TIME/PING/CLIENTINFO)");
            }
        } else if (!g_in_room) {
            bridge_write("|12ERR|07 Not in room yet");
        }

    /* ---- IDENTIFY password ---- */
    } else if (strcmp(cmd, "IDENTIFY") == 0) {
        const char *arg = (strlen(line) > 9) ? line + 9 : "";
        mrc_send_identify(arg);
        g_identify_sent = 1;

    /* ---- REGISTER password [email] ---- */
    } else if (strcmp(cmd, "REGISTER") == 0) {
        const char *rest = (strlen(line) > 9) ? line + 9 : "";
        char pass[32], email[128];
        pass[0] = email[0] = 0;
        sscanf(rest, "%31s %127s", pass, email);
        mrc_send_register(pass, email[0] ? email : NULL);

    /* ---- UPDATE param value ---- */
    } else if (strcmp(cmd, "UPDATE") == 0) {
        const char *rest = (strlen(line) > 7) ? line + 7 : "";
        char param[32], value[64];
        param[0] = value[0] = 0;
        sscanf(rest, "%31s %63s", param, value);
        mrc_send_update(param, value[0] ? value : NULL);

    /* ---- TRUST [cmd] ---- */
    } else if (strcmp(cmd, "TRUST") == 0) {
        const char *arg = (strlen(line) > 6) ? line + 6 : "INFO";
        mrc_send_trust(*arg ? arg : "INFO");

    /* ---- WHOON ---- */
    } else if (strcmp(cmd, "WHOON") == 0) {
        g_show_whoon_to_dos = 1;
        g_whoon_lines_left = 512;
        mrc_send_whoon();

    /* ---- CHATTERS ---- */
    } else if (strcmp(cmd, "CHATTERS") == 0) {
        g_show_whoon_to_dos = 1;
        g_whoon_lines_left  = 512;
        g_show_chatters_to_dos = 1;
        g_chatters_notme_count = 64;
        g_rooms_notme_count = 0;
        mrc_send_chatters();

    /* ---- USERLIST (raw - result goes to UI log) ---- */
    } else if (strcmp(cmd, "USERLIST") == 0) {
        mrc_send_userlist();

    /* ---- LIST (room list) ---- */
    } else if (strcmp(cmd, "ROOMS") == 0) {
        g_show_whoon_to_dos = 1;
        g_whoon_lines_left = 512;
        g_rooms_notme_count = 64;
        g_chatters_notme_count = 0;
        mrc_send_list();

    /* ---- USERS ---- */
    } else if (strcmp(cmd, "USERS") == 0) {
        g_show_whoon_to_dos = 1;
        g_whoon_lines_left = 512;
        mrc_send_users();

    /* ---- CHANNEL ---- */
    } else if (strcmp(cmd, "CHANNEL") == 0) {
        g_show_whoon_to_dos = 1;
        g_whoon_lines_left = 512;
        mrc_send_channel();

    /* ---- CONNECTED (list of BBSes) ---- */
    } else if (strcmp(cmd, "BBSES") == 0) {
        g_show_whoon_to_dos = 1;
        g_whoon_lines_left = 512;
        mrc_send_connected();

    /* ---- INFO N ---- */
    } else if (strcmp(cmd, "INFO") == 0) {
        int id = 0;
        sscanf(line, "INFO %d", &id);
        mrc_send_info_id(id);

    /* ---- TIME ---- */
    } else if (strcmp(cmd, "TIME") == 0) {
        mrc_send_time_cmd();

    /* ---- VERSION ---- */
    } else if (strcmp(cmd, "VERSION") == 0) {
        mrc_send_version();

    /* ---- STATS ---- */
    } else if (strcmp(cmd, "STATS") == 0) {
        mrc_send_stats();

    /* ---- MOTD ---- */
    } else if (strcmp(cmd, "MOTD") == 0) {
        bridge_write("|08[|14MOTD|08]|07 Loading message of the day...");
        g_show_motd_to_dos = 80;
        mrc_send_motd();

    /* ---- BANNERS ---- */
    } else if (strcmp(cmd, "BANNERS") == 0) {
        mrc_send_banners();

    /* ---- HELPSERVER [topic] ---- */
    } else if (strcmp(cmd, "HELPSERVER") == 0) {
        const char *arg = (strlen(line) > 11) ? line + 11 : "";
        g_show_whoon_to_dos = 1;
        g_whoon_lines_left = 512;
        mrc_send_help_topic(*arg ? arg : NULL);

    /* ---- TOPIC new_topic ---- */
    } else if (strcmp(cmd, "TOPIC") == 0) {
        const char *arg = (strlen(line) > 6) ? line + 6 : "";
        mrc_send_newtopic(arg);

    /* ---- ROOMPASS password ---- */
    } else if (strcmp(cmd, "ROOMPASS") == 0) {
        const char *arg = (strlen(line) > 9) ? line + 9 : "";
        mrc_send_roompass(*arg ? arg : NULL);

    /* ---- LAST — replay recent chat history ---- */
    } else if (strcmp(cmd, "LAST") == 0) {
        g_show_whoon_to_dos = 1;
        g_whoon_lines_left = 1100;
        bridge_write("|14LAST|07 Loading recent chat history...");
        mrc_send_packet_fields(g_mrc.user, g_mrc.bbs_name, g_mrc.room,
                               "SERVER", "", g_mrc.room, "LAST");

    /* ---- LASTSEEN user ---- */
    } else if (strcmp(cmd, "LASTSEEN") == 0) {
        const char *arg = (strlen(line) > 9) ? line + 9 : "";
        mrc_send_lastseen(*arg ? arg : NULL);

    /* ---- TOPICS (history) ---- */
    } else if (strcmp(cmd, "TOPICS") == 0) {
        mrc_send_topics_cmd();

    /* ---- SETSTATUS param [value] ---- */
    } else if (strcmp(cmd, "SETSTATUS") == 0) {
        const char *rest = (strlen(line) > 10) ? line + 10 : "";
        char param[32], value[64];
        param[0] = value[0] = 0;
        sscanf(rest, "%31s %63[^\n]", param, value);
        /* /back clears AFK and forces an IAMHERE ACTIVE on the next keepalive. */
        if (stricmp(param, "BACK") == 0) {
            mrc_send_status_cmd("AFK", "OFF");
            g_last_cmd_time = GetTickCount64();
            bridge_write("|10BACK|07 AFK cleared.");
        } else {
            mrc_send_status_cmd(param, value[0] ? value : NULL);
        }

    /* ---- ROOMCONFIG param [value] ---- */
    } else if (strcmp(cmd, "ROOMCONFIG") == 0) {
        const char *rest = (strlen(line) > 11) ? line + 11 : "";
        char param[32], value[64];
        param[0] = value[0] = 0;
        sscanf(rest, "%31s %63[^\n]", param, value);
        mrc_send_roomconfig(param, value[0] ? value : NULL);

    /* ---- CHANGELOG ---- */
    } else if (strcmp(cmd, "CHANGELOG") == 0) {
        mrc_send_changelog();

    /* ---- ROUTING ---- */
    } else if (strcmp(cmd, "ROUTING") == 0) {
        mrc_send_routing();

    /* ---- SET key value (client-side config) ---- */
    } else if (strcmp(cmd, "SET") == 0) {
        char key[32], value[64];
        key[0] = value[0] = 0;
        sscanf(line, "SET %31s %63[^\n]", key, value);

        if (stricmp(key, "HANDLECOLOR") == 0) {
            int c = atoi(value);
            if (c < 0 || c > 15) c = 11;
            g_handle_color = c;
        } else if (stricmp(key, "PREFIX") == 0) {
            safe_copy(g_handle_prefix, sizeof(g_handle_prefix),
                      strcmp(value, "NONE") == 0 ? "" : value);
        } else if (stricmp(key, "SUFFIX") == 0) {
            safe_copy(g_handle_suffix, sizeof(g_handle_suffix),
                      strcmp(value, "NONE") == 0 ? "" : value);
        } else if (stricmp(key, "BBSNAME") == 0) {
            underspace_copy(g_mrc.bbs_name, sizeof(g_mrc.bbs_name), value);
        } else if (stricmp(key, "BBSPRETTY") == 0) {
            underspace_copy(g_mrc.bbs_pretty, sizeof(g_mrc.bbs_pretty), value);
        } else if (stricmp(key, "ENTERROOM") == 0) {
            safe_copy(g_enterroom_msg, sizeof(g_enterroom_msg), value);
        } else if (stricmp(key, "LEAVEROOM") == 0) {
            safe_copy(g_leaveroom_msg, sizeof(g_leaveroom_msg), value);
        } else if (stricmp(key, "TEXTCOLOR") == 0) {
            int c = atoi(value);
            if (c < 0 || c > 15) c = 7;
            g_text_color = c;
        } else {
            bridge_write("|12ERR|07 Unknown SET key: %s", key);
        }

    /* ---- QUICKSTATS (lightweight stats, works when not connected) ---- */
    } else if (strcmp(cmd, "QUICKSTATS") == 0) {
        do_quickstats();

    } else {
        bridge_write("|12ERR|07 Unknown helper command: %s", cmd);
    }
}

/* ============================================================================
   MAIN LOOP - POLL
   ============================================================================ */

/* Drain the MRC socket for the current node and dispatch packets. */
static void mrc_poll(void) {
    unsigned char buf[2048];
    int n;
    char *start, *eol;

    if (!g_mrc.connected || g_mrc.sock == INVALID_SOCKET) return;

    for (;;) {
        if (g_mrc.use_tls) {
            n = tls_read_plain(buf, sizeof(buf));
            if (n == -2) break;
        } else {
            n = recv(g_mrc.sock, (char *)buf, sizeof(buf), 0);
            if (n < 0) {
                int e = WSAGetLastError();
                if (e == WSAEWOULDBLOCK) break;
                write_status_and_log("SOCKETERROR");
                helper_log("recv error %d", e);
                mrc_reset();
                return;
            }
        }

        if (n == 0) {
            bridge_write("STATUS DISCONNECTED");
            mrc_reset();
            return;
        }
        if (n < 0) {
            write_status_and_log("SOCKETERROR");
            mrc_reset();
            return;
        }

        if (g_mrc.recv_used + n >= (int)sizeof(g_mrc.recvbuf) - 1)
            g_mrc.recv_used = 0;
        memcpy(g_mrc.recvbuf + g_mrc.recv_used, buf, (size_t)n);
        g_mrc.recv_used += n;
        g_mrc.recvbuf[g_mrc.recv_used] = 0;

        start = g_mrc.recvbuf;
        for (;;) {
            eol = strchr(start, '\n');
            if (!eol) break;
            *eol = 0;
            trim_line(start);
            if (*start) LOG_V("RECV: %s", start);
            if (*start) handle_mrc_packet(start);
            start = eol + 1;
        }

        if (start != g_mrc.recvbuf) {
            size_t remain = strlen(start);
            memmove(g_mrc.recvbuf, start, remain + 1);
            g_mrc.recv_used = (int)remain;
        }
    }
}

/* ============================================================================
   ENTRY POINT
   ============================================================================ */

/* Bridge entry point: load config, init node slots, run the main event loop. */
void helper_run(void) {
    int i;

    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);

    if (!load_mrcbbs_config()) {
        DWORD written;
        const char *msg =
            "\r\n"
            "  ERROR: MRCBBS.DAT not found or incomplete.\r\n"
            "\r\n"
            "  Please run config.exe first to set up your BBS identity.\r\n"
            "  Required fields: bbs_name, bbs_pretty, sysop, telnet\r\n"
            "\r\n"
            "  Press Enter to exit...\r\n";
        WriteConsoleA(g_hout, msg, (DWORD)strlen(msg), &written, NULL);
        {
            char dummy[8];
            fgets(dummy, sizeof(dummy), stdin);
        }
        return;
    }

    /* Initialise node slots: slot 0/1 → ANETDOS.OUT/IN, slot N → ANETDOSN.OUT/IN. */
    for (i = 0; i < MAX_NODES; i++) {
        if (g_node_fixed && i == 0) continue;
        memset(&g_nodes[i], 0, sizeof(g_nodes[i]));
        node_init_defaults(&g_nodes[i]);
        g_nodes[i].node_num = (i < 2) ? 0 : i;
        if (i == 0 || i == 1) {
            safe_copy(g_nodes[i].bridge_out, sizeof(g_nodes[i].bridge_out), "ANETDOS.OUT");
            safe_copy(g_nodes[i].bridge_in,  sizeof(g_nodes[i].bridge_in),  "ANETDOS.IN");
        } else {
            snprintf(g_nodes[i].bridge_out, sizeof(g_nodes[i].bridge_out), "ANETDOS%d.OUT", i);
            snprintf(g_nodes[i].bridge_in,  sizeof(g_nodes[i].bridge_in),  "ANETDOS%d.IN",  i);
        }
    }
    /* Slot 1 duplicates slot 0's files. */
    g_nodes[1].active = 0;

    if (!g_node_fixed) {
        g_cur_node = &g_nodes[0];
    }

    helper_show_ans_banner();
    helper_status_update("Waiting for connection...");
    if (g_node_fixed) {
        helper_ui_log("ANETMRC Bridge started (single-node %d, files: %s / %s)",
                      g_cur_node->node_num, g_cur_node->bridge_out, g_cur_node->bridge_in);
    } else {
        helper_ui_log("ANETMRC Bridge started (multi-node mode, polling ANETDOS*.OUT)");
    }
    helper_ui_log("BBS: %s (%s)  SysOp: %s",
                  g_bbscfg.bbs_name, g_bbscfg.bbs_pretty, g_bbscfg.sysop);
    helper_ui_log("Server: %s:%d  show_motd=%s",
                  g_bbscfg.server, g_bbscfg.port,
                  g_bbscfg.show_motd ? "yes" : "no");

    g_cur_node = &g_nodes[0];
    mrc_reset();

    /* Main event loop: poll each active node's bridge file and socket.
     * Single-node mode processes only slot 0; multi-node iterates MAX_NODES. */
    for (;;) {
        int max_slot = g_node_fixed ? 1 : MAX_NODES;
        int any_connected = 0;
        ULONGLONG now;

        /* select() on plain-TCP sockets for up to 50 ms so we wake on arrival;
         * TLS sockets may buffer internally and are serviced via the timeout. */
        {
            fd_set rfds;
            struct timeval tv;
            int nfds = 0;
            FD_ZERO(&rfds);
            for (i = 0; i < max_slot; i++) {
                node_state_t *nd = &g_nodes[i];
                if (i == 1 && !g_node_fixed) continue;
                if (nd->active && nd->mrc.sock != INVALID_SOCKET
                        && !nd->mrc.use_tls) {
                    FD_SET(nd->mrc.sock, &rfds);
                    if ((int)nd->mrc.sock + 1 > nfds)
                        nfds = (int)nd->mrc.sock + 1;
                    any_connected = 1;
                }
                if (nd->active && nd->mrc.connected) any_connected = 1;
            }
            if (nfds > 0) {
                tv.tv_sec  = 0;
                tv.tv_usec = 50000;
                select(nfds, &rfds, NULL, NULL, &tv);
            } else if (!any_connected) {
                Sleep(200);
            } else {
                Sleep(50);
            }
        }

        now = GetTickCount64();
        any_connected = 0;

        for (i = 0; i < max_slot; i++) {
            node_state_t *nd = &g_nodes[i];
            FILE *in;
            char line[HELPER_LINE_MAX];

            if (i == 1 && !g_node_fixed) continue;

            g_cur_node = nd;

            /* Rate-limit .OUT file probes to every 500 ms on inactive slots. */
            if (!nd->active) {
                if ((now - nd->last_iamhere) < 500ULL)
                    continue;
                nd->last_iamhere = now;
                if (GetFileAttributesA(nd->bridge_out) == INVALID_FILE_ATTRIBUTES)
                    continue;
                nd->last_iamhere = 0;
                node_init_defaults(nd);
                nd->active = 1;
                helper_ui_log("Node %d activated (%s)", nd->node_num, nd->bridge_out);
                helper_status_update("Waiting for connection...");
            }

            in = fopen(nd->bridge_out, "r");
            if (in) {
                LOG_V("opened %s", nd->bridge_out);
                while (fgets(line, sizeof(line), in)) {
                    trim_line(line);
                    if (*line) handle_bridge_command(line);
                }
                fclose(in);
                DeleteFileA(nd->bridge_out);
            }

            if (nd->mrc.connected || nd->mrc.sock != INVALID_SOCKET) {
                mrc_poll();
                any_connected = 1;
            }

            /* IAMHERE every 60s; AWAY after 10 minutes with no user input. */
            if (nd->mrc.connected &&
                (GetTickCount64() - nd->last_iamhere) >= 60000ULL) {
                ULONGLONG idle_ms = GetTickCount64() - nd->last_cmd_time;
                mrc_send_iamhere(idle_ms >= 600000ULL ? "AWAY" : "ACTIVE");
                nd->last_iamhere = GetTickCount64();
            }

            /* Retire slot once disconnected and no .OUT file is pending. */
            if (!nd->mrc.connected &&
                nd->mrc.sock == INVALID_SOCKET &&
                GetFileAttributesA(nd->bridge_out) == INVALID_FILE_ATTRIBUTES) {
                if (nd->active) {
                    helper_ui_log("Node %d deactivated", nd->node_num);
                    nd->active = 0;
                }
            }
        }
    }
}
