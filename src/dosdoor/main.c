/* main.c — ANETMRC DOS door: handles handle entry, chat UI, slash commands,
 * FOSSIL serial I/O, and the bridge-file protocol. */

#include "common.h"
#include "fossil.h"
#include "dropfile.h"
#include "bridge.h"
#include <time.h>
#include <dos.h>

/* Forward declarations */
static void fossil_put_pipe(const char *s);
static void draw_color_bar(void);
static void put_input_colored(void);
static void redraw_chat_input(void);

/* ============================================================================
   CONSTANTS
   ============================================================================ */

#define MSG_MAX        100   /* ring buffer for pre-wrapped lines */
#define MSG_LEN        200
#define INPUT_RULE_ROW  23
#define INPUT_PROMPT_ROW 24
#define SCREEN_W        80
#define CHAT_W          78
#define CHAT_MSG_TOP     4   /* first message row (1-based) */
#define CHAT_MSG_BOT    22   /* last message row */
#define CHAT_MSG_ROWS   19

/* Default MRC server */
#define MRC_SERVER "na-multi.relaychat.net"
#define MRC_PORT    5000

/* ============================================================================
   DATA TYPES
   ============================================================================ */

#define TWIT_MAX 10
typedef struct {
    char handle[24];          /* MRC chat handle (may differ from BBS login name) */
    int  handle_color;        /* pipe color 0-15 */
    char handle_prefix[32];   /* text before handle, e.g. "|12@" */
    char handle_suffix[32];   /* text after handle, e.g. "|08[|12ANET|08]" */
    int  text_color;          /* general chat text color 0-15 */
    char enterroom[64];       /* join message, e.g. "has joined" */
    char leaveroom[64];       /* part message, e.g. "has left" */
    int  theme;               /* border theme 1-6 (1=dark gray default) */
    /* Twit (ignore) list: up to TWIT_MAX handles filtered from display */
    char twit_list[TWIT_MAX][24];
    int  twit_count;
} user_settings_t;

typedef enum {
    MODE_HANDLE   = 0,    /* First run: enter chat handle */
    MODE_MENU     = 1,    /* Main menu */
    MODE_SETTINGS = 2,    /* Settings editor */
    MODE_STATS    = 3,    /* Server stats display */
    MODE_CHAT     = 4,    /* Live chat */
    MODE_HELP     = 5     /* Help / info browser */
} app_mode_t;

typedef enum {
    EDIT_NONE       = 0,
    EDIT_HANDLE     = 1,
    EDIT_HCOLOR     = 2,
    EDIT_PREFIX     = 3,
    EDIT_SUFFIX     = 4,
    EDIT_ENTERROOM  = 5,
    EDIT_LEAVEROOM  = 6,
    EDIT_TEXTCOLOR  = 7
} edit_field_t;

/* ============================================================================
   GLOBALS
   ============================================================================ */

/* Message ring buffer for chat display */
static char g_msgs[MSG_MAX][MSG_LEN];
static int  g_msg_count = 0;
/* Mention flag bitset: 1 bit per g_msgs entry. */
#define MENTION_BITS ((MSG_MAX + 7) / 8)
static unsigned char g_msg_mention[MENTION_BITS];
#define mention_get(i) ((g_msg_mention[(i) >> 3] >> ((i) & 7)) & 1)
#define mention_set(i) (g_msg_mention[(i) >> 3] |=  (unsigned char)(1 << ((i) & 7)))
#define mention_clr(i) (g_msg_mention[(i) >> 3] &= (unsigned char)~(1 << ((i) & 7)))

/* Sent-message history for up-arrow recall */
#define SENT_HIST_MAX 10
static char g_sent_hist[SENT_HIST_MAX][128];
static int  g_sent_hist_count = 0;
static int  g_hist_browse = -1;     /* -1 = not browsing */
static char g_hist_saved[128];

/* Application state */
static app_mode_t  g_mode     = MODE_HANDLE;
static edit_field_t g_edit    = EDIT_NONE;
static int         g_sent_quit = 0;

/* Text input buffer (shared across modes) */
static char g_input[256];
static int  g_input_len = 0;
static int  g_input_max = 140;   /* MRC 140-char protocol limit */

/* Current user settings */
static user_settings_t g_settings;

/* BBS username (settings DB key) */
static char g_bbs_user[64];

/* Chat screen header state */
static char g_chat_room[32];
static char g_chat_topic[80];
static char g_chat_users[16];
static char g_chat_status[32];
static char g_chat_latency[16];

/* Scrollback: 0 = live view, positive = scrolled back N lines */
static int g_scroll_off = 0;

static int g_new_while_scrolled = 0;

/* Visible input width after the prompt */
static int g_chat_view_w = 60;

/* Mention tracking */
static int  g_mention_count = 0;
static char g_last_mentioner[32];
static int  g_last_line_start = -1;

/* Last DM partner for /r */
static char g_last_dm_target[32];

/* Slash-command to main-loop signal for /quit */
static int  g_return_to_menu = 0;

/* Tab autocomplete: comma-separated username list */
static char g_tab_users[512];
/* Tab highlight range in g_input (-1 = none) */
static int g_tab_hl_start = -1;
static int g_tab_hl_end   = -1;

/* Local (no-BBS) mode: bypass FOSSIL, use stdin/stdout */
static int g_local_mode = 0;

/* Help browser: 0 = menu, 1-4 = subpage */
static int g_help_page = 0;

/* ============================================================================
   SETTINGS DATABASE  (MRCUSER.DAT)
   ============================================================================ */

/* Build a settings section key from a BBS username (alphanumeric + underscore). */
static void make_section_key(char *out, size_t outsz, const char *name) {
    size_t i = 0;
    while (*name && i + 1 < outsz) {
        unsigned char c = (unsigned char)*name++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9')) {
            out[i++] = (char)c;
        } else if (c == ' ' || c == '_') {
            out[i++] = '_';
        }
    }
    out[i] = 0;
    if (!out[0]) { out[0] = 'U'; out[1] = 0; }
}

/* Populate a user_settings_t with default values. */
static void settings_defaults(user_settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->handle[0]   = 0;
    s->handle_color = 11;
    s->text_color   = 7;
    safe_copy(s->enterroom, sizeof(s->enterroom), "has joined");
    safe_copy(s->leaveroom, sizeof(s->leaveroom), "has left");
    s->theme = 1;
    s->twit_count = 0;
}

/* Load a user's settings from MRCUSER.DAT; returns 1 if a handle was found. */
static int load_settings(const char *bbs_username, user_settings_t *out) {
    FILE *fp;
    char line[128];
    char section[64];
    char key_section[64];
    int in_section = 0;

    settings_defaults(out);
    make_section_key(key_section, sizeof(key_section), bbs_username);

    fp = fopen("MRCUSER.DAT", "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp)) {
        trim_crlf(line);
        if (!line[0] || line[0] == '#') continue;

        if (line[0] == '[') {
            size_t i;
            for (i = 0; i + 1 < sizeof(section) && line[i + 1] && line[i + 1] != ']'; ++i)
                section[i] = line[i + 1];
            section[i] = 0;
            in_section = (stricmp(section, key_section) == 0);
            continue;
        }

        if (!in_section) continue;

        {
            char *eq = strchr(line, '=');
            const char *k, *v;
            if (!eq) continue;
            *eq = 0;
            k = line;
            v = eq + 1;

            if (stricmp(k, "handle") == 0)
                safe_copy(out->handle, sizeof(out->handle), v);
            else if (stricmp(k, "handle_color") == 0)
                out->handle_color = atoi(v);
            else if (stricmp(k, "handle_prefix") == 0)
                safe_copy(out->handle_prefix, sizeof(out->handle_prefix), v);
            else if (stricmp(k, "handle_suffix") == 0)
                safe_copy(out->handle_suffix, sizeof(out->handle_suffix), v);
            else if (stricmp(k, "text_color") == 0)
                out->text_color = atoi(v);
            else if (stricmp(k, "enterroom") == 0)
                safe_copy(out->enterroom, sizeof(out->enterroom), v);
            else if (stricmp(k, "leaveroom") == 0)
                safe_copy(out->leaveroom, sizeof(out->leaveroom), v);
            else if (stricmp(k, "theme") == 0)
                out->theme = atoi(v);
            else if (stricmp(k, "twit") == 0) {
                char tmp[256], *tok;
                safe_copy(tmp, sizeof(tmp), v);
                tok = strtok(tmp, ",");
                while (tok && out->twit_count < TWIT_MAX) {
                    while (*tok == ' ') tok++;
                    if (*tok) {
                        safe_copy(out->twit_list[out->twit_count],
                                  sizeof(out->twit_list[0]), tok);
                        out->twit_count++;
                    }
                    tok = strtok(NULL, ",");
                }
            }
        }
    }

    fclose(fp);
    return out->handle[0] ? 1 : 0;
}

/* Write one user's settings section to an open file. */
static void write_settings_section(FILE *fp, const char *key_section,
                                   const user_settings_t *s) {
    fprintf(fp, "[%s]\n", key_section);
    fprintf(fp, "handle=%s\n",         s->handle);
    fprintf(fp, "handle_color=%d\n",   s->handle_color);
    fprintf(fp, "handle_prefix=%s\n",  s->handle_prefix);
    fprintf(fp, "handle_suffix=%s\n",  s->handle_suffix);
    fprintf(fp, "text_color=%d\n",     s->text_color);
    fprintf(fp, "enterroom=%s\n",      s->enterroom);
    fprintf(fp, "leaveroom=%s\n",      s->leaveroom);
    fprintf(fp, "theme=%d\n",          s->theme);
    if (s->twit_count > 0) {
        int ti;
        fprintf(fp, "twit=");
        for (ti = 0; ti < s->twit_count; ti++) {
            if (ti > 0) fprintf(fp, ",");
            fprintf(fp, "%s", s->twit_list[ti]);
        }
        fprintf(fp, "\n");
    }
    fprintf(fp, "\n");
}

/* Rewrite the user's section in MRCUSER.DAT via a temp file. */
static void save_settings(const char *bbs_username, const user_settings_t *s) {
    char key_section[64];
    char line[128];
    FILE *fin, *fout;
    int in_target = 0, wrote = 0;

    make_section_key(key_section, sizeof(key_section), bbs_username);

    fin  = fopen("MRCUSER.DAT", "r");
    fout = fopen("MRCUSER.TMP", "w");
    if (!fout) { if (fin) fclose(fin); return; }

    if (fin) {
        while (fgets(line, sizeof(line), fin)) {
            trim_crlf(line);
            if (line[0] == '[') {
                char sec[64];
                size_t j = 0;
                while (j + 1 < sizeof(sec) && line[j+1] && line[j+1] != ']')
                    { sec[j] = line[j+1]; j++; }
                sec[j] = 0;
                if (stricmp(sec, key_section) == 0) {
                    write_settings_section(fout, key_section, s);
                    wrote = 1;
                    in_target = 1;
                    continue;
                } else {
                    in_target = 0;
                }
            }
            if (!in_target)
                fprintf(fout, "%s\n", line);
        }
        fclose(fin);
    }

    if (!wrote)
        write_settings_section(fout, key_section, s);

    fclose(fout);
    remove("MRCUSER.DAT");
    rename("MRCUSER.TMP", "MRCUSER.DAT");
}

/* ============================================================================
   DISPLAY PRIMITIVES
   ============================================================================ */

/* Move cursor to row,col via ANSI escape. */
static void ansi_goto(int row, int col) {
    fossil_printf("\033[%d;%dH", row, col);
}

/* Emit ANSI SGR for pipe color code. 0-15 = foreground (dark 0-7,
 * bright 8-15). 16-18 = background (Mystic convention):
 *   |16 = black bg   |17 = dark blue bg   |18 = dark green bg
 *
 * Each fg emit also sets intensity explicitly (22 = normal, 1 = bold).
 * Without this, terminals like SyncTERM keep bold ON after a prior
 * bright code, so colors 1-7 render as bright when they should be dark. */
static void ansi_color_code(int n) {
    static const int fgbase[8] = { 30, 34, 32, 36, 31, 35, 33, 37 };
    static const int bgmap[3]  = { 40, 44, 42 };
    if (n < 0) n = 0;
    if (n <= 7) {
        fossil_printf("\033[22;%dm", fgbase[n]);
    } else if (n <= 15) {
        fossil_printf("\033[1;%dm", fgbase[n - 8]);
    } else if (n <= 18) {
        fossil_printf("\033[%dm", bgmap[n - 16]);
    } else {
        fossil_printf("\033[1;%dm", fgbase[7]);
    }
}

/* Emit a string to the FOSSIL, translating |NN pipe codes to ANSI colors. */
static void fossil_put_pipe(const char *s) {
    while (s && *s) {
        if (*s == '|' &&
            isdigit((unsigned char)s[1]) &&
            isdigit((unsigned char)s[2])) {
            ansi_color_code((s[1] - '0') * 10 + (s[2] - '0'));
            s += 3;
            continue;
        }
        fossil_putch((unsigned char)*s++);
    }
    fossil_puts("\033[0m");
}

/* Draw a themed 80-column horizontal rule using CP437 block chars. */
static void draw_hr(void) {
    int i, t;
    static const char *theme_ansi[7] = {
        "\033[90m",
        "\033[90m",
        "\033[34m",
        "\033[32m",
        "\033[36m",
        "\033[31m",
        "\033[35m",
    };
    t = g_settings.theme;
    if (t < 1 || t > 6) t = 1;
    fossil_puts(theme_ansi[t]);
    for (i = 0; i < SCREEN_W; ++i)
        fossil_putch(0xDC);
    fossil_puts("\033[0m\r\n");
}

/* Draw a two-column "label: value" line. */
static void draw_info_line(const char *label, const char *value) {
    char buf[256];
    snprintf(buf, sizeof(buf), "|15%-14s|08: |07%s", label ? label : "", value ? value : "");
    fossil_put_pipe(buf);
    fossil_puts("\r\n");
}

/* Draw a color sampler bar showing colors 1-15. */
static void draw_color_bar(void) {
    char bar[300];
    int i, pos = 0;
    const char *header = "|08Colors: ";
    size_t hlen = strlen(header);
    memcpy(bar, header, hlen);
    pos = (int)hlen;
    for (i = 1; i <= 15 && pos + 10 < (int)sizeof(bar); ++i) {
        int n = snprintf(bar + pos, sizeof(bar) - pos, "|%02d%-3d", i, i);
        if (n > 0) pos += n;
    }
    bar[pos] = 0;
    fossil_put_pipe(bar);
    fossil_puts("\r\n");
}

/* Draw a preview line showing the handle with current prefix/color/suffix. */
static void draw_handle_preview(void) {
    char preview[128];
    snprintf(preview, sizeof(preview), "|07Preview: %s|%02d%s%s|07",
             g_settings.handle_prefix,
             g_settings.handle_color,
             g_settings.handle[0] ? g_settings.handle : "Handle",
             g_settings.handle_suffix);
    fossil_put_pipe(preview);
    fossil_puts("\r\n");
}

/* ============================================================================
   TWIT (IGNORE) LIST HELPERS
   ============================================================================ */

/* Return 1 if handle is on the twit (ignore) list. */
static int twit_has(const char *handle) {
    int i;
    for (i = 0; i < g_settings.twit_count; i++)
        if (stricmp(g_settings.twit_list[i], handle) == 0) return 1;
    return 0;
}

/* Add a handle to the twit list and save settings; returns 1 on add, 0 if duplicate, -1 if full. */
static int twit_add(const char *handle) {
    if (twit_has(handle)) return 0;
    if (g_settings.twit_count >= TWIT_MAX) return -1;
    safe_copy(g_settings.twit_list[g_settings.twit_count],
              sizeof(g_settings.twit_list[0]), handle);
    g_settings.twit_count++;
    save_settings(g_bbs_user, &g_settings);
    return 1;
}

/* Remove a handle from the twit list and save settings; returns 1 if removed. */
static int twit_remove(const char *handle) {
    int i;
    for (i = 0; i < g_settings.twit_count; i++) {
        if (stricmp(g_settings.twit_list[i], handle) == 0) {
            int j;
            for (j = i; j < g_settings.twit_count - 1; j++)
                memcpy(g_settings.twit_list[j], g_settings.twit_list[j+1],
                       sizeof(g_settings.twit_list[0]));
            g_settings.twit_count--;
            save_settings(g_bbs_user, &g_settings);
            return 1;
        }
    }
    return 0;
}

/* ============================================================================
   MESSAGE BUFFER
   ============================================================================ */

/* Copy up to max_vis visible chars from src to dst, preserving pipe codes and
 * breaking at the last space to avoid mid-word cuts. */
static int copy_vis_pipe(char *dst, int dstsz, const char *src, int max_vis,
                         int *active_color_io) {
    int di = 0, vis = 0, si = 0;
    int last_space_dst = -1, last_space_src = -1;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = 0;

    while (src[si] && di + 1 < dstsz) {
        if (src[si] == '|' &&
            isdigit((unsigned char)src[si + 1]) &&
            isdigit((unsigned char)src[si + 2])) {
            int c = (src[si + 1] - '0') * 10 + (src[si + 2] - '0');
            if (di + 3 >= dstsz) break;
            dst[di++] = src[si++];
            dst[di++] = src[si++];
            dst[di++] = src[si++];
            if (active_color_io) *active_color_io = c;
            continue;
        }
        if (vis >= max_vis) break;
        if (src[si] == ' ') { last_space_dst = di; last_space_src = si; }
        dst[di++] = src[si++];
        ++vis;
    }
    dst[di] = 0;

    if (src[si] && last_space_dst > 0) {
        dst[last_space_dst] = 0;
        return last_space_src + 1;
    }
    return si;
}

/* Append a message to the scrollback ring; shifts out oldest when full. */
static void add_msg(const char *s) {
    int i;
    if (!s || !*s) return;
    if (g_msg_count >= MSG_MAX) {
        for (i = 1; i < MSG_MAX; ++i) {
            safe_copy(g_msgs[i - 1], sizeof(g_msgs[i - 1]), g_msgs[i]);
            if (mention_get(i)) mention_set(i - 1); else mention_clr(i - 1);
        }
        g_msg_count = MSG_MAX - 1;
    }
    safe_copy(g_msgs[g_msg_count], sizeof(g_msgs[g_msg_count]), s);
    mention_clr(g_msg_count);
    g_msg_count++;
}

/* Append a message to the scrollback ring, wrapping long lines at width. */
static void add_msg_wrapped(const char *s, int width) {
    char line[MSG_LEN];
    int off = 0, slen;
    int active_color = -1;

    if (!s || !*s) { add_msg(""); return; }
    slen = (int)strlen(s);

    while (off < slen) {
        int consumed, dst_start = 0;

        while (s[off] == ' ') off++;
        if (off >= slen) break;

        if (active_color >= 0 && off > 0) {
            line[0] = '|';
            line[1] = '0' + active_color / 10;
            line[2] = '0' + active_color % 10;
            dst_start = 3;
        }

        consumed = copy_vis_pipe(line + dst_start,
                                 (int)sizeof(line) - dst_start,
                                 s + off, width, &active_color);
        if (consumed <= 0) break;
        add_msg(line);
        off += consumed;
    }
}

/* ============================================================================
   SCREEN LAYOUTS
   ============================================================================ */

/* Clear screen and draw the ANETMRC title and optional subtitle. */
static void draw_header(const char *subtitle) {
    fossil_cls();
    fossil_put_pipe("|10A|11NET|15MRC|07  |08MRC chat for DOS BBSes|07\r\n");
    draw_hr();
    if (subtitle && *subtitle) {
        fossil_put_pipe(subtitle);
        fossil_puts("\r\n");
        draw_hr();
    }
}

/* Render the initial handle-entry screen. */
static void draw_handle_entry(const dropfile_info_t *drop) {
    draw_header("|15Welcome to MRC Chat|07");
    draw_info_line("BBS Login", drop->alias[0] ? drop->alias : drop->user_name);
    fossil_put_pipe("|07\r\n");
    fossil_put_pipe("|07Your |15chat handle|07 is how others see you in MRC chat.\r\n");
    fossil_put_pipe("|07It may differ from your BBS login name.\r\n");
    fossil_put_pipe("|07  - Max 20 characters\r\n");
    fossil_put_pipe("|07  - Spaces become underscores\r\n");
    fossil_put_pipe("|07  - No ~ character\r\n");
    fossil_puts("\r\n");
    draw_hr();
    fossil_put_pipe("|15Enter your chat handle:|07\r\n");
    fossil_put_pipe("|08> |07");
    fossil_put_pipe(g_input);
    fossil_putch('_');
}

/* Render the help browser page indicated by g_help_page. */
static void draw_help(void) {
    draw_header("|14Help|07");
    draw_hr();

    if (g_help_page == 0) {
        fossil_put_pipe("|14  ANET MRC Help\r\n\r\n|07");
        fossil_put_pipe("|08 |151|07) About MRC & ANETMRC\r\n");
        fossil_put_pipe("|08 |152|07) Chat, Navigation & Appearance\r\n");
        fossil_put_pipe("|08 |153|07) MRC Server Commands\r\n");
        fossil_put_pipe("|08 |154|07) Profile, Ignore & CTCP\r\n");
        fossil_put_pipe("\r\n|08Press |151-4|07 or |15B|07 to return: ");

    } else if (g_help_page == 1) {
        fossil_put_pipe("|14  About MRC & ANETMRC\r\n\r\n|07");
        fossil_put_pipe("|15MRC|07 (Multi-Relay Chat) links BBSes for real-time chat.\r\n");
        fossil_put_pipe("|07Users on any MRC-connected BBS share the same rooms.\r\n\r\n");
        fossil_put_pipe("|11Network: |07MRC relay (NA/EU/AU, set in MRCBBS.DAT)\r\n");
        fossil_put_pipe("|11Protocol:|07 MRC 1.3  |11Client:|07 ANETMRC 1.3.8\r\n\r\n");
        fossil_put_pipe("|15Tips:\r\n|07");
        fossil_put_pipe(" /identify <pw>  Register/log in to MRC Trust\r\n");
        fossil_put_pipe(" /join <room>    Switch rooms\r\n");
        fossil_put_pipe(" /chatters       See who is in your room\r\n");
        fossil_put_pipe(" /syshelp        Full command list in chat\r\n");
        draw_hr();
        fossil_put_pipe("|08Press any key... ");

    } else if (g_help_page == 2) {
        fossil_put_pipe("|14  Chat & Navigation\r\n\r\n|07");
        fossil_put_pipe("|15/join <room>  /list  /chatters  /whoon\r\n|07");
        fossil_put_pipe("|15/msg <u> txt  /t <u> txt  (private msg)\r\n|07");
        fossil_put_pipe("|15/me <text>    /b <text>   (action/broadcast)\r\n|07");
        fossil_put_pipe("|15/afk [msg]    /back       (AFK on/off)\r\n|07");
        fossil_put_pipe("|15/mentions     /quit\r\n\r\n|07");
        fossil_put_pipe("|11Navigation:\r\n|07");
        fossil_put_pipe(" PgUp/PgDn  Scroll  |15ESC|07 menu  |15Up|07 recall  |15Tab|07 complete\r\n\r\n");
        fossil_put_pipe("|11Appearance:\r\n|07");
        fossil_put_pipe("|15/color 1-15  /prefix <txt>  /suffix <txt>\r\n|07");
        fossil_put_pipe("|15/theme 1-6   |07(1=gray 2=blue 3=green 4=cyan 5=red 6=magenta)\r\n");
        draw_hr();
        fossil_put_pipe("|08Press any key... ");

    } else if (g_help_page == 3) {
        fossil_put_pipe("|14  MRC Server Commands\r\n\r\n|07");
        fossil_put_pipe("|15/motd  /time  /version  /stats  /banners\r\n");
        fossil_put_pipe("|15/users  /channel  /bbses  /info <N>\r\n");
        fossil_put_pipe("|15/topic <txt>  /last  /lastseen <u>\r\n");
        fossil_put_pipe("|15/topics  /routing  /changelog\r\n");
        fossil_put_pipe("|15/roompass <pw>  /roomconfig [param val]\r\n");
        fossil_put_pipe("|15/helpserver [topic]  |07 MRC server help\r\n");
        fossil_put_pipe("|15!<cmd>|07  Send any server helper command\r\n");
        fossil_put_pipe("|07    (e.g. !weather, !fortune, !time)\r\n");
        draw_hr();
        fossil_put_pipe("|08Press any key... ");

    } else if (g_help_page == 4) {
        fossil_put_pipe("|14  Profile, Ignore & CTCP\r\n\r\n|07");
        fossil_put_pipe("|11MRC Trust:\r\n|07");
        fossil_put_pipe("|15/register <pw> [email]|07  Register handle\r\n");
        fossil_put_pipe("|15/identify <pw>|07         Log in\r\n");
        fossil_put_pipe("|15/update <param> <val>|07  Update profile\r\n");
        fossil_put_pipe("|15/trust [INFO|USERS|ROOMS|BBSES]\r\n\r\n|07");
        fossil_put_pipe("|11Ignore list:\r\n|07");
        fossil_put_pipe("|15/twit <handle>|07   Add to ignore list\r\n");
        fossil_put_pipe("|15/twit|07          Show ignore list\r\n");
        fossil_put_pipe("|15/untwit <h>|07   Remove from ignore list\r\n\r\n");
        fossil_put_pipe("|11CTCP:\r\n|07");
        fossil_put_pipe("|15/ctcp <u> VERSION|TIME|PING|CLIENTINFO\r\n");
        draw_hr();
        fossil_put_pipe("|08Press any key... ");
    }
}

/* Render the main menu. */
static void draw_main_menu(const dropfile_info_t *drop) {
    char colinfo[64];

    draw_header(NULL);
    draw_info_line("BBS User", drop->alias[0] ? drop->alias : drop->user_name);
    draw_info_line("Server",   "MRC relay (from MRCBBS.DAT)");

    snprintf(colinfo, sizeof(colinfo), "|%02d%s%s%s|07",
             g_settings.handle_color,
             g_settings.handle_prefix,
             g_settings.handle,
             g_settings.handle_suffix);
    draw_info_line("Chat Handle", colinfo);
    draw_hr();

    draw_color_bar();
    fossil_puts("\r\n");

    fossil_put_pipe("|08  |151|07) Connect to MRC\r\n");
    fossil_put_pipe("|08  |152|07) Edit Settings\r\n");
    fossil_put_pipe("|08  |153|07) Server Stats\r\n");
    fossil_put_pipe("|08  |154|07) Help\r\n");
    fossil_put_pipe("|08  |155|07) Quit\r\n");
    fossil_puts("\r\n");
    draw_hr();
    fossil_put_pipe("|08Press |151-5|07: ");
}

/* Render the settings editor screen. */
static void draw_settings_menu(void) {
    char buf[128];

    draw_header("|15Edit Settings|07");
    draw_handle_preview();
    fossil_puts("\r\n");

    snprintf(buf, sizeof(buf), "%s|%02d%s%s|07",
             g_settings.handle_prefix,
             g_settings.handle_color,
             g_settings.handle,
             g_settings.handle_suffix);
    draw_info_line("|151|07) Chat Handle", buf);

    snprintf(buf, sizeof(buf), "|%02d%d|07  (use /color N in chat too)",
             g_settings.handle_color, g_settings.handle_color);
    draw_info_line("|152|07) Handle Color", buf);

    snprintf(buf, sizeof(buf), "%s", g_settings.handle_prefix[0] ? g_settings.handle_prefix : "(none)");
    draw_info_line("|153|07) Handle Prefix", buf);

    snprintf(buf, sizeof(buf), "%s", g_settings.handle_suffix[0] ? g_settings.handle_suffix : "(none)");
    draw_info_line("|154|07) Handle Suffix", buf);

    snprintf(buf, sizeof(buf), "|%02d%d|07  (chat text color)",
             g_settings.text_color, g_settings.text_color);
    draw_info_line("|155|07) Text Color", buf);

    snprintf(buf, sizeof(buf), "%s", g_settings.enterroom);
    draw_info_line("|156|07) Enter Room Msg", buf);

    snprintf(buf, sizeof(buf), "%s", g_settings.leaveroom);
    draw_info_line("|157|07) Leave Room Msg", buf);

    draw_hr();

    if (g_edit != EDIT_NONE) {
        const char *field_names[] = {
            "", "Chat Handle", "Handle Color (1-15)", "Handle Prefix",
            "Handle Suffix", "Text Color (1-15)", "Enter Room Message",
            "Leave Room Message"
        };
        fossil_put_pipe("|11Editing: |15");
        fossil_puts(field_names[g_edit]);
        fossil_puts("|07\r\n");

        if (g_edit == EDIT_HCOLOR || g_edit == EDIT_TEXTCOLOR)
            draw_color_bar();

        fossil_put_pipe("|08New value (Enter=save, Esc=cancel): |07");
        fossil_put_pipe(g_input);
        fossil_putch('_');
    } else {
        fossil_put_pipe("|08[1-7] Edit field    [B] Back to menu|07\r\n");
        fossil_put_pipe("|08> ");
    }
}

/* Render the server stats screen from a "bbses rooms users activity" string. */
static void draw_stats(const char *raw) {
    int bbses = 0, rooms = 0, users = 0, activity = 0;
    const char *acts[] = { "None", "Low", "Medium", "High" };

    draw_header("|15MRC Server Stats|07");

    sscanf(raw ? raw : "0 0 0 0", "%d %d %d %d", &bbses, &rooms, &users, &activity);
    if (activity < 0) activity = 0;
    if (activity > 3) activity = 3;

    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", bbses);
        draw_info_line("BBSes Connected", buf);
        snprintf(buf, sizeof(buf), "%d", rooms);
        draw_info_line("Active Rooms   ", buf);
        snprintf(buf, sizeof(buf), "%d", users);
        draw_info_line("Users Online   ", buf);
        draw_info_line("Activity       ", acts[activity]);
    }

    draw_hr();
    fossil_put_pipe("|08Server: |07MRC relay (from MRCBBS.DAT)\r\n");
    fossil_puts("\r\n");
    fossil_put_pipe("|15Press any key to return...|07");
}

/* Render the "waiting for stats" placeholder screen. */
static void draw_stats_waiting(void) {
    draw_header("|15MRC Server Stats|07");
    fossil_put_pipe("|14Connecting to server for stats...|07\r\n");
    fossil_puts("Please wait.\r\n");
}

/* Case-insensitive strstr. */
static const char *ci_strstr(const char *hay, const char *needle) {
    int nlen = (int)strlen(needle);
    if (!nlen) return hay;
    for (; *hay; ++hay) {
        if (strnicmp(hay, needle, (size_t)nlen) == 0)
            return hay;
    }
    return NULL;
}

/* Format current local time as "HH:MM" into buf. */
static void local_time_str(char *buf, int bufsz) {
    time_t t = time(NULL);
    struct tm *tm;
    if (t == (time_t)-1) { safe_copy(buf, (size_t)bufsz, "??:??"); return; }
    tm = localtime(&t);
    if (!tm) { safe_copy(buf, (size_t)bufsz, "??:??"); return; }
    snprintf(buf, (size_t)bufsz, "%02d:%02d", tm->tm_hour, tm->tm_min);
}

/* Redraw rows 1-2 of the chat header (room/topic and status bar). */
static void draw_chat_info(void) {
    char buf[SCREEN_W + 64];

    ansi_goto(1, 1);
    fossil_puts("\033[K");
    snprintf(buf, sizeof(buf), "|11Room:|07 %-10s  |13Topic:|07 %s",
             g_chat_room[0]  ? g_chat_room  : "---",
             g_chat_topic[0] ? g_chat_topic : "(no topic)");
    fossil_put_pipe(buf);

    {
        char cnt_part[24];
        if (g_input_len >= 140)
            snprintf(cnt_part, sizeof(cnt_part), "|12%d|07/140", g_input_len);
        else if (g_input_len >= 130)
            snprintf(cnt_part, sizeof(cnt_part), "|14%d|07/140", g_input_len);
        else
            snprintf(cnt_part, sizeof(cnt_part), "|07%d|07/140", g_input_len);

        ansi_goto(2, 1);
        fossil_puts("\033[K");
        snprintf(buf, sizeof(buf),
                 "|10Status:|07 %-9s |14Users:|07 %-3s |09Ping:|07 %-6s |13Mntn:|07 %-2d |08Char:|07 %s",
                 g_chat_status[0]  ? g_chat_status  : "IDLE",
                 g_chat_users[0]   ? g_chat_users   : "?",
                 g_chat_latency[0] ? g_chat_latency : "---",
                 g_mention_count, cnt_part);
        fossil_put_pipe(buf);
    }
}

/* Rewrite only the character-count field on row 2 of the chat header. */
static void draw_char_count_only(void) {
    char cnt_buf[20];
    if (g_input_len >= 140)
        snprintf(cnt_buf, sizeof(cnt_buf), "|12%d|07/140", g_input_len);
    else if (g_input_len >= 130)
        snprintf(cnt_buf, sizeof(cnt_buf), "|14%d|07/140", g_input_len);
    else
        snprintf(cnt_buf, sizeof(cnt_buf), "|07%d|07/140", g_input_len);
    ansi_goto(2, 58);
    fossil_puts("\033[K");
    fossil_put_pipe(cnt_buf);
}

/* Clear screen and redraw the entire chat header (rows 1-3). */
static void draw_chat_header(void) {
    fossil_cls();
    draw_chat_info();
    ansi_goto(3, 1);
    draw_hr();
}

/* Redraw the chat message rows, rule row, and input prompt. */
static void draw_chat_msgs(void) {
    int row, j;
    int end, first;
    char prompt[64];

    end = g_msg_count - g_scroll_off;
    if (end < 0) end = 0;
    first = (end > CHAT_MSG_ROWS) ? end - CHAT_MSG_ROWS : 0;

    fossil_puts("\033[?25l");   /* hide cursor while painting */

    for (row = CHAT_MSG_TOP; row <= CHAT_MSG_BOT; ++row) {
        int mi = first + (row - CHAT_MSG_TOP);
        ansi_goto(row, 1);
        fossil_puts("\033[K");
        if (mi < end && mi < g_msg_count) {
            if (mention_get(mi)) fossil_put_pipe("|14*|07");
            fossil_put_pipe(g_msgs[mi]);
        }
    }

    {
        int t2; static const char *ta[7] = {
            "\033[90m","\033[90m","\033[34m","\033[32m",
            "\033[36m","\033[31m","\033[35m"
        };
        t2 = g_settings.theme; if (t2 < 1 || t2 > 6) t2 = 1;
        ansi_goto(INPUT_RULE_ROW, 1);
        fossil_puts("\033[K");
        fossil_puts(ta[t2]);
        for (j = 0; j < SCREEN_W; ++j)
            fossil_putch(0xDC);
        fossil_puts("\033[0m");
    }
    if (g_scroll_off > 0) {
        char scr_buf[64];
        ansi_goto(INPUT_RULE_ROW, 1);
        if (g_new_while_scrolled > 0)
            snprintf(scr_buf, sizeof(scr_buf),
                     "|14[ SCROLL: UP/DN - %d new - ESC to resume ]|07", g_new_while_scrolled);
        else
            safe_copy(scr_buf, sizeof(scr_buf), "|14[ SCROLL: UP/DN - ESC to resume ]|07");
        fossil_put_pipe(scr_buf);
    }

    ansi_goto(INPUT_PROMPT_ROW, 1);
    fossil_puts("\033[K");
    snprintf(prompt, sizeof(prompt), "%s|%02d%s%s|07 ",
             g_settings.handle_prefix,
             g_settings.handle_color,
             g_settings.handle,
             g_settings.handle_suffix);
    fossil_put_pipe(prompt);
    put_input_colored();

    fossil_puts("\033[?25h");   /* show cursor */
}

/* Redraw the full chat screen (header + messages). */
static void draw_chat(void) {
    draw_chat_header();
    draw_chat_msgs();
}

/* Show the paginated /mentions log viewer; blocks until ENTER/ESC. */
static void show_mentions_pane(void) {
    int count = 0, pages, page = 0;
    char buf[256];
    FILE *mf;

    mf = fopen("MRCMENT.LOG", "r");
    if (mf) {
        while (fgets(buf, sizeof(buf), mf)) {
            size_t n = strlen(buf);
            while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
            if (buf[0]) ++count;
        }
        fclose(mf);
    }
    if (count == 0) {
        add_msg("|07No mentions since last check.");
        draw_chat_info();
        draw_chat_msgs();
        redraw_chat_input();
        return;
    }
    pages = (count + CHAT_MSG_ROWS - 1) / CHAT_MSG_ROWS;

    for (;;) {
        int start = page * CHAT_MSG_ROWS;
        int row   = CHAT_MSG_TOP;
        int idx   = 0;
        int done  = 0;

        fossil_cls();
        ansi_goto(1, 1);
        fossil_put_pipe("|14* * *  MENTIONS  * * *|07");
        ansi_goto(2, 1);
        {
            char hdr[80];
            snprintf(hdr, sizeof(hdr),
                     "|08Page |15%d|08/|15%d  |08total |15%d|07",
                     page + 1, pages, count);
            fossil_put_pipe(hdr);
        }
        ansi_goto(3, 1);
        draw_hr();

        mf = fopen("MRCMENT.LOG", "r");
        if (mf) {
            while (fgets(buf, sizeof(buf), mf) && row <= CHAT_MSG_BOT) {
                size_t n = strlen(buf);
                char *t1, *t2;
                while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
                if (!buf[0]) continue;
                if (idx++ < start) continue;
                t1 = strchr(buf, '\t'); if (!t1) continue; *t1 = 0;
                t2 = strchr(t1 + 1, '\t'); if (!t2) continue; *t2 = 0;
                {
                    char out[260];
                    snprintf(out, sizeof(out),
                             "|08[%s]|07 |11%s|08:|07 %s",
                             buf, t1 + 1, t2 + 1);
                    ansi_goto(row++, 1);
                    fossil_puts("\033[K");
                    fossil_put_pipe(out);
                }
            }
            fclose(mf);
        }

        ansi_goto(INPUT_RULE_ROW, 1);
        fossil_puts("\033[K");
        fossil_put_pipe(
            "|14[ PgUp/PgDn/Home/End - ENTER or ESC to return to chat ]|07");
        ansi_goto(INPUT_PROMPT_ROW, 1);
        fossil_puts("\033[K");

        while (!done) {
            int ch = fossil_getch_nonblock();
            if (ch < 0) { delay(20); continue; }

            if (ch == 13 || ch == 10) { done = 2; break; }

            if (ch == 27) {
                int ch2 = -1, ch3 = -1, r;
                for (r = 0; r < 25; ++r) {
                    ch2 = fossil_getch_nonblock();
                    if (ch2 >= 0) break;
                    delay(2);
                }
                if (ch2 != '[' && ch2 != 'O') { done = 2; break; }
                for (r = 0; r < 25; ++r) {
                    ch3 = fossil_getch_nonblock();
                    if (ch3 >= 0) break;
                    delay(2);
                }
                if (ch3 == '5' || ch3 == 'I') {
                    if (ch3 == '5') (void)fossil_getch_nonblock();
                    if (page > 0) { --page; done = 1; }
                } else if (ch3 == '6' || ch3 == 'G') {
                    if (ch3 == '6') (void)fossil_getch_nonblock();
                    if (page < pages - 1) { ++page; done = 1; }
                } else if (ch3 == 'H' || ch3 == '1') {
                    if (ch3 == '1') (void)fossil_getch_nonblock();
                    if (page != 0) { page = 0; done = 1; }
                } else if (ch3 == 'F' || ch3 == '4') {
                    if (ch3 == '4') (void)fossil_getch_nonblock();
                    if (page != pages - 1) { page = pages - 1; done = 1; }
                }
                continue;
            }

            if (ch == 0) {
                int scan = fossil_getch_nonblock();
                if (scan < 0) continue;
                if (scan == 73 && page > 0) { --page; done = 1; }
                else if (scan == 81 && page < pages - 1) { ++page; done = 1; }
                else if (scan == 71 && page != 0) { page = 0; done = 1; }
                else if (scan == 79 && page != pages - 1) {
                    page = pages - 1; done = 1;
                }
            }
        }
        if (done == 2) break;
    }

    remove("MRCMENT.LOG");
    {
        int i;
        for (i = 0; i < MENTION_BITS; ++i) g_msg_mention[i] = 0;
    }
    g_mention_count = 0;
    g_last_mentioner[0] = 0;

    draw_chat();
}

/* Count visible characters in a pipe-coded string. */
static int pipe_vis_len(const char *s) {
    int n = 0;
    while (*s) {
        if (*s == '|' && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2])) {
            s += 3; continue;
        }
        ++n; ++s;
    }
    return n;
}

/* Return the 1-based column where the input cursor should sit. */
static int chat_cursor_col(void) {
    char prompt[64];
    int vis;
    snprintf(prompt, sizeof(prompt), "%s|%02d%s%s|07 ",
             g_settings.handle_prefix, g_settings.handle_color,
             g_settings.handle, g_settings.handle_suffix);
    vis = g_input_len < g_chat_view_w ? g_input_len : g_chat_view_w;
    return pipe_vis_len(prompt) + vis + 1;
}

/* Compute the [ms, me) byte range in g_input that should render as '*'
 * (passwords in /identify, /roompass, /register, /update password). */
static int masked_range(int *ms, int *me) {
    int i;
    *ms = -1; *me = -1;
    if (strncmp(g_input, "/identify ", 10) == 0 ||
        strncmp(g_input, "/roompass ", 10) == 0) {
        *ms = 10; *me = g_input_len;
        return 1;
    }
    if (strncmp(g_input, "/register ", 10) == 0) {
        *ms = 10; *me = g_input_len;
        for (i = 10; i < g_input_len; ++i)
            if (g_input[i] == ' ') { *me = i; break; }
        return 1;
    }
    if (strncmp(g_input, "/update password ", 17) == 0) {
        *ms = 17; *me = g_input_len;
        return 1;
    }
    return 0;
}

/* Emit g_input to the screen with tab-completion highlight and password masking. */
static void put_input_colored(void) {
    int i, view_start, ms, me;
    view_start = (g_input_len > g_chat_view_w) ? (g_input_len - g_chat_view_w) : 0;
    masked_range(&ms, &me);

    ansi_color_code(g_settings.text_color);
    if (g_tab_hl_start >= 0 && g_tab_hl_end > g_tab_hl_start
            && g_tab_hl_end <= g_input_len) {
        int hl_s = g_tab_hl_start > view_start ? g_tab_hl_start : view_start;
        for (i = view_start; i < hl_s; ++i)
            fossil_putch((i >= ms && i < me) ? '*' : (unsigned char)g_input[i]);
        fossil_puts("\033[95m");
        for (i = hl_s; i < g_tab_hl_end; ++i)
            fossil_putch((i >= ms && i < me) ? '*' : (unsigned char)g_input[i]);
        ansi_color_code(g_settings.text_color);
        for (i = g_tab_hl_end; i < g_input_len; ++i)
            fossil_putch((i >= ms && i < me) ? '*' : (unsigned char)g_input[i]);
    } else {
        for (i = view_start; i < g_input_len; ++i)
            fossil_putch((i >= ms && i < me) ? '*' : (unsigned char)g_input[i]);
    }
}

/* Redraw the input prompt row with the current handle and g_input buffer. */
static void redraw_chat_input(void) {
    char prompt[64];
    ansi_goto(INPUT_PROMPT_ROW, 1);
    fossil_puts("\033[K");
    snprintf(prompt, sizeof(prompt), "%s|%02d%s%s|07 ",
             g_settings.handle_prefix,
             g_settings.handle_color,
             g_settings.handle,
             g_settings.handle_suffix);
    fossil_put_pipe(prompt);
    put_input_colored();
}

/* ============================================================================
   HELPER SYNC
   ============================================================================ */

/* Send all style settings to the bridge helper. */
static void sync_settings_to_helper(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "SET HANDLECOLOR %d", g_settings.handle_color);
    bridge_send_line(cmd);
    snprintf(cmd, sizeof(cmd), "SET PREFIX %s",
             g_settings.handle_prefix[0] ? g_settings.handle_prefix : "NONE");
    bridge_send_line(cmd);
    snprintf(cmd, sizeof(cmd), "SET SUFFIX %s",
             g_settings.handle_suffix[0] ? g_settings.handle_suffix : "NONE");
    bridge_send_line(cmd);
    snprintf(cmd, sizeof(cmd), "SET ENTERROOM %s", g_settings.enterroom);
    bridge_send_line(cmd);
    snprintf(cmd, sizeof(cmd), "SET LEAVEROOM %s", g_settings.leaveroom);
    bridge_send_line(cmd);
    snprintf(cmd, sizeof(cmd), "SET TEXTCOLOR %d", g_settings.text_color);
    bridge_send_line(cmd);
}

/* Tell the helper to open the MRC connection and sync settings. */
static void send_connect(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "CONNECT %s %d 0 %s lobby",
             MRC_SERVER, MRC_PORT, g_settings.handle);
    bridge_send_line(cmd);
    sync_settings_to_helper();
}

/* Send a single QUIT to the helper, guarded so it only fires once. */
static void send_quit_once(void) {
    if (!g_sent_quit) {
        bridge_send_line("QUIT");
        g_sent_quit = 1;
    }
}

/* ============================================================================
   INPUT VALIDATION HELPERS
   ============================================================================ */

/* Sanitize a handle: strip pipe codes, tilde, control chars; spaces to underscore; max 20 chars. */
static void sanitize_handle(char *out, size_t outsz, const char *src) {
    size_t i = 0;
    while (*src && i + 1 < outsz && i < 20) {
        unsigned char c = (unsigned char)*src;
        if (c == '|' && isdigit((unsigned char)src[1]) && isdigit((unsigned char)src[2])) {
            src += 3; continue;
        }
        if (c == '~' || c < 32) { src++; continue; }
        out[i++] = (c == ' ') ? '_' : (char)c;
        src++;
    }
    out[i] = 0;
}

/* Sanitize a short text field: strip tilde, truncate to maxlen chars. */
static void sanitize_field(char *out, size_t outsz, const char *src, size_t maxlen) {
    size_t i = 0;
    if (maxlen > outsz - 1) maxlen = outsz - 1;
    while (*src && i < maxlen) {
        unsigned char c = (unsigned char)*src++;
        if (c == '~') continue;
        out[i++] = (char)c;
    }
    out[i] = 0;
}

/* ============================================================================
   CHAT SLASH COMMAND HANDLER
   ============================================================================ */

/* Dispatch a /command entered in chat mode. */
static void handle_slash_command(const char *input_orig) {
    char slash_buf[512];
    const char *input;
    char cmd[512];
    const char *args, *sp;
    {
        int i = 0;
        /* Lowercase the command verb; preserve argument case. */
        while (i < (int)sizeof(slash_buf) - 1 && input_orig[i] && input_orig[i] != ' ')
            slash_buf[i] = (char)tolower((unsigned char)input_orig[i]), i++;
        while (i < (int)sizeof(slash_buf) - 1 && input_orig[i])
            slash_buf[i] = input_orig[i], i++;
        slash_buf[i] = 0;
        input = slash_buf;
    }

    if (strcmp(input, "/help") == 0 || strcmp(input, "/syshelp") == 0) {
        add_msg("|14=== ANETMRC Client Commands ===");
        add_msg("|11Chat:");
        add_msg("|15/join <room>   |07Switch room");
        add_msg("|15/list          |07Room list");
        add_msg("|15/chatters      |07Users in your room");
        add_msg("|15/whoon         |07Users across the whole network");
        add_msg("|15/msg <u> <txt> |07Private message  (|15/t|07 shortcut)");
        add_msg("|15/r <text>      |07Reply to last DM partner");
        add_msg("|15/me <text>     |07Action / emote");
        add_msg("|15/b <text>      |07Broadcast to all rooms");
        add_msg("|15/afk [msg]     |07Set AFK");
        add_msg("|15/back          |07Clear AFK");
        add_msg("|15/mentions      |07Show / reset mention counter");
        add_msg("|11Ignore:");
        add_msg("|15/twit <handle> |07Add user to ignore list");
        add_msg("|15/twit          |07Show ignore list");
        add_msg("|15/untwit <h>    |07Remove from ignore list");
        add_msg("|11Appearance:");
        add_msg("|15/color 1-15    |07Handle color");
        add_msg("|15/prefix /suffix|07 Handle decorators");
        add_msg("|15/theme 1-6     |07Border color theme");
        add_msg("|11Navigation:");
        add_msg("|15/quit  ESC     |07Back to main menu");
        add_msg("|15Tab            |07Autocomplete handle from users in room");
        add_msg("|11MRC Server:");
        add_msg("|15/motd /time /version /stats /banners");
        add_msg("|15/users /channel /bbses /info N");
        add_msg("|15/topic /last /lastseen /topics");
        add_msg("|15/routing /changelog /roompass");
        add_msg("|15/identify /register /update /trust");
        add_msg("|14=== /helpserver for full MRC server help ===");
        draw_chat_msgs();
        return;
    }

    if (strncmp(input, "/quit", 5) == 0) { g_return_to_menu = 1; return; }

    if (strcmp(input, "/mentions") == 0) {
        show_mentions_pane();
        return;
    }

    if (strncmp(input, "/join ", 6) == 0) {
        snprintf(cmd, sizeof(cmd),"JOIN %s", input + 6);
        bridge_send_line(cmd); return;
    }

    if (strncmp(input, "/identify ", 10) == 0) {
        snprintf(cmd, sizeof(cmd),"IDENTIFY %s", input + 10);
        bridge_send_line(cmd); return;
    }

    if (strncmp(input, "/register ", 10) == 0) {
        snprintf(cmd, sizeof(cmd),"REGISTER %s", input + 10);
        bridge_send_line(cmd); return;
    }

    if (strncmp(input, "/update ", 8) == 0) {
        snprintf(cmd, sizeof(cmd),"UPDATE %s", input + 8);
        bridge_send_line(cmd); return;
    }

    if (strncmp(input, "/trust", 6) == 0) {
        const char *a = input + 6;
        if (*a == ' ') ++a;
        snprintf(cmd, sizeof(cmd),"TRUST %s", *a ? a : "INFO");
        bridge_send_line(cmd); return;
    }

    if (strncmp(input, "/msg ", 5) == 0 || strncmp(input, "/t ", 3) == 0) {
        int off = (input[2] == ' ') ? 3 : 5;
        args = input + off;
        sp = strchr(args, ' ');
        if (sp && sp > args) {
            int ulen = (int)(sp - args);
            if (ulen > 0 && ulen < (int)sizeof(g_last_dm_target)) {
                memcpy(g_last_dm_target, args, (size_t)ulen);
                g_last_dm_target[ulen] = 0;
            }
            snprintf(cmd, sizeof(cmd),"MSG %.*s %s", ulen, args, sp + 1);
            bridge_send_line(cmd);
        } else { add_msg("|12ERR|07 Usage: /msg user text  (or /t user text)"); }
        return;
    }

    if (strncmp(input, "/r ", 3) == 0) {
        if (!g_last_dm_target[0]) {
            add_msg("|12ERR|07 No previous DM. Use /msg <user> <text> first.");
            return;
        }
        snprintf(cmd, sizeof(cmd), "MSG %s %s", g_last_dm_target, input + 3);
        bridge_send_line(cmd);
        return;
    }

    if (strncmp(input, "/me ", 4) == 0) {
        snprintf(cmd, sizeof(cmd),"ME %s", input + 4);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/ctcp ", 6) == 0) {
        snprintf(cmd, sizeof(cmd),"CTCP %s", input + 6);
        bridge_send_line(cmd); return;
    }

    if (strncmp(input, "/b ", 3) == 0) {
        snprintf(cmd, sizeof(cmd),"BROADCAST %s", input + 3);
        bridge_send_line(cmd); return;
    }

    if (strcmp(input, "/chatters") == 0) { bridge_send_line("CHATTERS"); return; }
    if (strcmp(input, "/whoon")    == 0) { bridge_send_line("WHOON");    return; }
    if (strcmp(input, "/userlist") == 0) { bridge_send_line("USERLIST"); return; }
    if (strcmp(input, "/users")    == 0) { bridge_send_line("USERS");    return; }
    if (strcmp(input, "/list")     == 0) { bridge_send_line("ROOMS");    return; }
    if (strcmp(input, "/channel")  == 0) { bridge_send_line("CHANNEL");  return; }
    if (strcmp(input, "/bbses")    == 0) { bridge_send_line("BBSES");    return; }
    if (strcmp(input, "/motd")     == 0) { bridge_send_line("MOTD");     return; }
    if (strcmp(input, "/time")     == 0) { bridge_send_line("TIME");     return; }
    if (strcmp(input, "/version")  == 0) { bridge_send_line("VERSION");  return; }
    if (strcmp(input, "/stats")    == 0) { bridge_send_line("STATS");    return; }
    if (strcmp(input, "/banners")  == 0) { bridge_send_line("BANNERS");  return; }
    if (strcmp(input, "/changelog")== 0) { bridge_send_line("CHANGELOG");return; }
    if (strcmp(input, "/routing")  == 0) { bridge_send_line("ROUTING");  return; }
    if (strcmp(input, "/topics")   == 0) { bridge_send_line("TOPICS");   return; }

    if (strncmp(input, "/info ", 6) == 0) {
        snprintf(cmd, sizeof(cmd),"INFO %s", input + 6);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/topic ", 7) == 0) {
        snprintf(cmd, sizeof(cmd),"TOPIC %s", input + 7);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/roompass ", 10) == 0) {
        snprintf(cmd, sizeof(cmd),"ROOMPASS %s", input + 10);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/lastseen ", 10) == 0) {
        snprintf(cmd, sizeof(cmd),"LASTSEEN %s", input + 10);
        bridge_send_line(cmd); return;
    }
    if (strcmp(input, "/last") == 0) {
        bridge_send_line("LAST"); return;
    }
    if (strncmp(input, "/roomconfig", 11) == 0) {
        const char *a = input + 11;
        if (*a == ' ') ++a;
        snprintf(cmd, sizeof(cmd),"ROOMCONFIG %s", a);
        bridge_send_line(cmd); return;
    }
    if (strncmp(input, "/helpserver", 11) == 0) {
        const char *a = input + 11;
        if (*a == ' ') ++a;
        snprintf(cmd, sizeof(cmd),"HELPSERVER %s", a);
        bridge_send_line(cmd); return;
    }

    /* /scroll (and /scroll up) scroll up one line — same behavior as the
     * Up arrow. /pgup still moves a full page. */
    if (strcmp(input, "/scroll") == 0 ||
        strncmp(input, "/scroll up", 10) == 0) {
        int max_scroll = g_msg_count > CHAT_MSG_ROWS ? g_msg_count - CHAT_MSG_ROWS : 0;
        if (max_scroll > 0) {
            g_scroll_off++;
            if (g_scroll_off > max_scroll) g_scroll_off = max_scroll;
            draw_chat_msgs();
        }
        return;
    }
    if (strcmp(input, "/pgup") == 0) {
        int max_scroll = g_msg_count > CHAT_MSG_ROWS ? g_msg_count - CHAT_MSG_ROWS : 0;
        g_scroll_off += CHAT_MSG_ROWS;
        if (g_scroll_off > max_scroll) g_scroll_off = max_scroll;
        draw_chat_msgs();
        return;
    }
    /* /scroll down = one line down (Down arrow). /pgdn = full page. */
    if (strncmp(input, "/scroll down", 12) == 0 ||
        strcmp(input, "/scroll live") == 0) {
        if (g_scroll_off > 0) {
            g_scroll_off--;
            if (g_scroll_off == 0) g_new_while_scrolled = 0;
            draw_chat_msgs();
        }
        return;
    }
    if (strcmp(input, "/pgdn") == 0) {
        g_scroll_off -= CHAT_MSG_ROWS;
        if (g_scroll_off < 0) g_scroll_off = 0;
        if (g_scroll_off == 0) g_new_while_scrolled = 0;
        draw_chat_msgs();
        return;
    }

    if (strncmp(input, "/afk", 4) == 0) {
        const char *a = input + 4;
        if (*a == ' ') ++a;
        if (*a) snprintf(cmd, sizeof(cmd),"SETSTATUS AFK %s", a);
        else    snprintf(cmd, sizeof(cmd),"SETSTATUS AFK");
        bridge_send_line(cmd); return;
    }

    if (strcmp(input, "/back") == 0) {
        bridge_send_line("SETSTATUS BACK");
        add_msg("|10BACK|07 AFK cleared. Welcome back!");
        return;
    }

    /* Twit (ignore) list */
    if (strncmp(input, "/twit", 5) == 0) {
        const char *a = input + 5;
        if (*a == ' ') ++a;
        if (*a) {
            int r = twit_add(a);
            if (r == 1) {
                char m[80];
                snprintf(m, sizeof(m), "|11TWIT|07 |12%s|07 added to ignore list.", a);
                add_msg(m);
            } else if (r == 0) {
                add_msg("|11TWIT|07 Already on ignore list.");
            } else {
                add_msg("|12ERR|07 Ignore list full (max 10).");
            }
        } else {
            if (g_settings.twit_count == 0) {
                add_msg("|11TWIT|07 Ignore list is empty.");
            } else {
                char m[200];
                int ti;
                add_msg("|11TWIT|07 Ignored handles:");
                for (ti = 0; ti < g_settings.twit_count; ti++) {
                    snprintf(m, sizeof(m), "|11  %d|07) |12%s|07",
                             ti + 1, g_settings.twit_list[ti]);
                    add_msg(m);
                }
            }
        }
        draw_chat_msgs();
        return;
    }

    if (strncmp(input, "/untwit ", 8) == 0) {
        const char *a = input + 8;
        if (twit_remove(a)) {
            char m[80];
            snprintf(m, sizeof(m), "|11TWIT|07 |14%s|07 removed from ignore list.", a);
            add_msg(m);
        } else {
            add_msg("|12ERR|07 Handle not in ignore list.");
        }
        draw_chat_msgs();
        return;
    }

    if (strncmp(input, "/color ", 7) == 0) {
        int c = atoi(input + 7);
        if (c < 1 || c > 15) { add_msg("|12ERR|07 Color 1-15"); return; }
        g_settings.handle_color = c;
        save_settings(g_bbs_user, &g_settings);
        snprintf(cmd, sizeof(cmd),"SET HANDLECOLOR %d", c);
        bridge_send_line(cmd);
        { char m[64]; snprintf(m, sizeof(m), "|11COLOR|07 Handle color set: |%02d%d|07", c, c); add_msg(m); }
        return;
    }

    if (strncmp(input, "/prefix", 7) == 0) {
        const char *val = (input[7] == ' ') ? input + 8 : "";
        sanitize_field(g_settings.handle_prefix, sizeof(g_settings.handle_prefix), val, 30);
        save_settings(g_bbs_user, &g_settings);
        snprintf(cmd, sizeof(cmd),"SET PREFIX %s",
                g_settings.handle_prefix[0] ? g_settings.handle_prefix : "NONE");
        bridge_send_line(cmd);
        add_msg("|11PREFIX|07 Updated");
        return;
    }

    if (strncmp(input, "/suffix", 7) == 0) {
        const char *val = (input[7] == ' ') ? input + 8 : "";
        sanitize_field(g_settings.handle_suffix, sizeof(g_settings.handle_suffix), val, 30);
        save_settings(g_bbs_user, &g_settings);
        snprintf(cmd, sizeof(cmd),"SET SUFFIX %s",
                g_settings.handle_suffix[0] ? g_settings.handle_suffix : "NONE");
        bridge_send_line(cmd);
        add_msg("|11SUFFIX|07 Updated");
        return;
    }

    if (strncmp(input, "/theme ", 7) == 0) {
        int t = atoi(input + 7);
        if (t < 1 || t > 6) {
            add_msg("|12ERR|07 Theme 1-6:  1=gray 2=blue 3=green 4=cyan 5=red 6=magenta");
            return;
        }
        g_settings.theme = t;
        save_settings(g_bbs_user, &g_settings);
        draw_chat_header();
        { char m[64]; snprintf(m, sizeof(m), "|11THEME|07 Changed to %d", t); add_msg(m); }
        draw_chat_msgs();
        return;
    }

    /* Unknown /command: forward to server as !command. Server replies with
     * its own error if the command is invalid; lets new server commands work
     * without a client update. */
    {
        char raw[512];
        snprintf(raw, sizeof(raw), "SEND !%s", input + 1);
        bridge_send_line(raw);
    }
}

/* ============================================================================
   MENTION DETECTION
   ============================================================================ */

/* Return 1 if the bridge line is a chat message eligible for mention checks. */
static int is_mentionable_chat_line(const char *line) {
    if (!line || !line[0]) return 0;
    if (strncmp(line, "|08*",       4)  == 0) return 0;
    if (strncmp(line, "|08(",       4)  == 0) return 0;
    if (strncmp(line, "|08[|14CTCP",10) == 0) return 0;
    if (strncmp(line, "|08[|14CTCP-REPLY",16) == 0) return 0;
    if (strncmp(line, "|10SERVER",  9)  == 0) return 0;
    if (strncmp(line, "|10READY",   8)  == 0) return 0;
    if (strncmp(line, "|14NOTICE",  9)  == 0) return 0;
    if (strncmp(line, "|14[BROADCAST]",14) == 0) return 0;
    if (strncmp(line, "|14[BANNER]",11) == 0) return 0;
    if (strncmp(line, "|11JOIN",    7)  == 0) return 0;
    if (strncmp(line, "|11NICK",    7)  == 0) return 0;
    if (strncmp(line, "|11CHATTERS",11) == 0) return 0;
    if (strncmp(line, "|11Users",   8)  == 0) return 0;
    if (strncmp(line, "|11MOTD",    7)  == 0) return 0;
    if (strncmp(line, "|12",        3)  == 0) return 0;
    if (strncmp(line, "|13(",       4)  == 0) return 0;
    if (strncmp(line, "|13* ",      5)  == 0) return 0;
    if (strncmp(line, "Connecting", 10) == 0) return 0;
    {
        char own[128];
        snprintf(own, sizeof(own), "%s|%02d%s%s|07",
                 g_settings.handle_prefix,
                 g_settings.handle_color,
                 g_settings.handle,
                 g_settings.handle_suffix);
        if (strncmp(line, own, strlen(own)) == 0) return 0;
    }
    return 1;
}

/* Copy src to dst with all |NN pipe color codes stripped. */
static void strip_pipe_for_mention(char *dst, int dstsz, const char *src) {
    int i = 0;
    while (*src && i < dstsz - 1) {
        if (src[0] == '|' &&
            (unsigned char)src[1] >= '0' && (unsigned char)src[1] <= '9' &&
            (unsigned char)src[2] >= '0' && (unsigned char)src[2] <= '9') {
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = 0;
}

/* Return 1 if handle appears as a whole word in hay, case-insensitive. */
static int handle_mentioned(const char *hay, const char *handle) {
    char stripped[MSG_LEN + 16];
    int hlen = (int)strlen(handle);
    const char *p;
    if (!hlen) return 0;
    strip_pipe_for_mention(stripped, sizeof(stripped), hay);
    p = stripped;
    while ((p = ci_strstr(p, handle)) != NULL) {
        int before = (p == stripped) || (!isalnum((unsigned char)p[-1]) && p[-1] != '_');
        int after  = !isalnum((unsigned char)p[hlen]) && p[hlen] != '_';
        if (before && after) return 1;
        p++;
    }
    return 0;
}

/* Extract the sender handle from a formatted chat line. */
static const char *extract_sender(const char *line) {
    static char sbuf[32];
    char stripped[MSG_LEN + 16];
    const char *p;
    int i;
    strip_pipe_for_mention(stripped, sizeof(stripped), line);
    p = stripped;
    while (*p && !isalnum((unsigned char)*p) && *p != '_') p++;
    if (!*p) return NULL;
    for (i = 0; *p && i < (int)sizeof(sbuf) - 1; ++p) {
        if (*p == '>' || *p == ']' || *p == ')' || *p == '/' || *p == ' ' || *p == '<')
            break;
        sbuf[i++] = *p;
    }
    sbuf[i] = 0;
    return i > 0 ? sbuf : NULL;
}

/* ============================================================================
   TAB AUTOCOMPLETE
   ============================================================================ */

/* Tab-complete the partial word in g_input from the cached user list. */
static int tab_complete(void) {
    int word_start, i;
    char partial[32];
    int plen;
    const char *p, *tok;
    char users_copy[512];
    char *name, *rest;

    if (!g_tab_users[0] || g_input_len <= 0) return 0;

    word_start = g_input_len;
    while (word_start > 0 && g_input[word_start - 1] != ' ')
        --word_start;

    plen = g_input_len - word_start;
    if (plen <= 0) return 0;
    if (plen >= (int)sizeof(partial)) return 0;

    for (i = 0; i < plen; ++i)
        partial[i] = g_input[word_start + i];
    partial[plen] = 0;

    safe_copy(users_copy, sizeof(users_copy), g_tab_users);
    name = users_copy;
    while (name && *name) {
        rest = strchr(name, ',');
        if (rest) *rest = 0;
        tok = name;
        while (*tok == ' ') ++tok;
        {
            int tlen = (int)strlen(tok);
            while (tlen > 0 && tok[tlen-1] == ' ') tlen--;
            if (tlen >= plen && strnicmp(tok, partial, (size_t)plen) == 0) {
                int new_len = word_start + tlen;
                if (new_len < (int)sizeof(g_input) - 2) {
                    int k;
                    for (k = 0; k < tlen; ++k)
                        g_input[word_start + k] = tok[k];
                    g_input[new_len] = 0;
                    g_input_len = new_len;
                    if (word_start == 0 && new_len + 1 < (int)sizeof(g_input) - 2) {
                        g_input[new_len] = ':';
                        g_input[new_len + 1] = ' ';
                        g_input[new_len + 2] = 0;
                        g_input_len = new_len + 2;
                    }
                    g_tab_hl_start = word_start;
                    g_tab_hl_end   = word_start + tlen;
                    return 1;
                }
            }
        }
        name = rest ? rest + 1 : NULL;
    }
    (void)p;
    return 0;
}

/* ============================================================================
   SETTINGS FIELD COMMIT
   ============================================================================ */

/* Apply the current input buffer to the field selected by g_edit and save. */
static void commit_settings_edit(void) {
    char sanitized[128];

    switch (g_edit) {
    case EDIT_HANDLE:
        sanitize_handle(sanitized, sizeof(sanitized), g_input);
        if (!sanitized[0]) break;
        safe_copy(g_settings.handle, sizeof(g_settings.handle), sanitized);
        break;

    case EDIT_HCOLOR: {
        int c = atoi(g_input);
        if (c < 1 || c > 15) c = 11;
        g_settings.handle_color = c;
        break;
    }

    case EDIT_PREFIX:
        sanitize_field(g_settings.handle_prefix, sizeof(g_settings.handle_prefix),
                       g_input, 30);
        break;

    case EDIT_SUFFIX:
        sanitize_field(g_settings.handle_suffix, sizeof(g_settings.handle_suffix),
                       g_input, 30);
        break;

    case EDIT_TEXTCOLOR: {
        char cmd[32];
        int c = atoi(g_input);
        if (c < 1 || c > 15) c = 7;
        g_settings.text_color = c;
        snprintf(cmd, sizeof(cmd), "SET TEXTCOLOR %d", c);
        bridge_send_line(cmd);
        break;
    }

    case EDIT_ENTERROOM:
        sanitize_field(g_settings.enterroom, sizeof(g_settings.enterroom),
                       g_input, 60);
        break;

    case EDIT_LEAVEROOM:
        sanitize_field(g_settings.leaveroom, sizeof(g_settings.leaveroom),
                       g_input, 60);
        break;

    default: break;
    }

    save_settings(g_bbs_user, &g_settings);
    g_edit = EDIT_NONE;
    g_input[0] = 0;
    g_input_len = 0;
}

/* ============================================================================
   MAIN LOOP
   ============================================================================ */

/* Door entry point: sets up the environment and runs the main event loop. */
int main(int argc, char **argv) {
    dropfile_info_t drop;
    char status[64];
    char line[512];
    int ch;
    int stats_wait = 0;
    int i;
    int idle_count = 0;

    memset(&drop,  0, sizeof(drop));
    memset(g_input, 0, sizeof(g_input));
    safe_copy(status, sizeof(status), "READY");

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--local") == 0) {
            g_local_mode = 1;
            break;
        }
    }

    if (g_local_mode) {
        safe_copy(drop.user_name, sizeof(drop.user_name), "SysOp");
        safe_copy(drop.alias,     sizeof(drop.alias),     "SysOp");
        drop.comm_port = 0;
        fossil_set_local_mode();
    } else {
        const char *drop_path = dropfile_get_path_from_args(argc, argv);
        if (!dropfile_load(&drop, drop_path)) return 1;
    }

    safe_copy(g_bbs_user, sizeof(g_bbs_user),
              drop.alias[0] ? drop.alias : drop.user_name);

    fossil_set_port((unsigned short)drop.comm_port);
    if (!fossil_init()) return 1;

    /* Per-node bridge files: COM1 -> node1, COM2 -> node2, etc.
     * --local uses ANETDOS0.OUT/IN to avoid colliding with a live node 1. */
    if (g_local_mode)
        bridge_set_node_local();
    else if (drop.comm_port >= 0)
        bridge_set_node(drop.comm_port + 1);

    if (!bridge_connect_local()) {
        fossil_puts("Bridge connect failed.\r\n");
        fossil_deinit();
        return 1;
    }

    if (load_settings(g_bbs_user, &g_settings) && g_settings.handle[0]) {
        g_mode = MODE_MENU;
    } else {
        g_mode = MODE_HANDLE;
    }

    if (g_settings.handle_color < 0 || g_settings.handle_color > 15)
        g_settings.handle_color = 11;
    if (g_settings.text_color < 0 || g_settings.text_color > 15)
        g_settings.text_color = 7;
    if (g_settings.theme < 1 || g_settings.theme > 6)
        g_settings.theme = 1;

    if (g_mode == MODE_HANDLE)
        draw_handle_entry(&drop);
    else
        draw_main_menu(&drop);

    for (;;) {
        int got_bridge = 0;

        while (bridge_poll_line(line, sizeof(line))) {
            got_bridge = 1;

            if (strncmp(line, "STATUS ", 7) == 0) {
                safe_copy(status, sizeof(status), line + 7);
                safe_copy(g_chat_status, sizeof(g_chat_status), line + 7);
                if (g_mode == MODE_CHAT) { draw_chat_info(); ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col()); }
                continue;
            }
            if (strncmp(line, "ROOM ", 5) == 0) {
                safe_copy(g_chat_room, sizeof(g_chat_room), line + 5);
                if (g_mode == MODE_CHAT) { draw_chat_info(); ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col()); }
                continue;
            }
            if (strncmp(line, "TOPIC ", 6) == 0) {
                safe_copy(g_chat_topic, sizeof(g_chat_topic), line + 6);
                if (g_mode == MODE_CHAT) { draw_chat_info(); ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col()); }
                continue;
            }
            if (strncmp(line, "USERS ", 6) == 0) {
                safe_copy(g_chat_users, sizeof(g_chat_users), line + 6);
                if (g_mode == MODE_CHAT) { draw_chat_info(); ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col()); }
                continue;
            }
            if (strncmp(line, "LATENCY ", 8) == 0) {
                safe_copy(g_chat_latency, sizeof(g_chat_latency), line + 8);
                if (g_mode == MODE_CHAT) { draw_chat_info(); ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col()); }
                continue;
            }

            if (strncmp(line, "|11CONFIG", 9) == 0) continue;

            if (strncmp(line, "CHATLIST ", 9) == 0) {
                safe_copy(g_tab_users, sizeof(g_tab_users), line + 9);
                continue;
            }

            if (strncmp(line, "STATS_RESULT ", 13) == 0) {
                stats_wait = 0;
                draw_stats(line + 13);
                g_mode = MODE_STATS;
                continue;
            }

            if (g_mode == MODE_CHAT) {
                if (strcmp(status, "DISCONNECTED") == 0 ||
                    strcmp(status, "OFFLINE")      == 0 ||
                    strcmp(status, "SOCKETERROR")  == 0) {
                    g_sent_quit = 1;
                    add_msg_wrapped("|12DISCONNECTED|07 Press ESC to return to menu.", CHAT_W);
                }
                if (line[0]) {
                    /* Track incoming DM sender for /r reply. */
                    if (strncmp(line, "|08(@|11", 9) == 0) {
                        const char *p = line + 9;
                        const char *end = p;
                        while (*end && *end != '|') end++;
                        if (end > p) {
                            int n = (int)(end - p);
                            char nm[32];
                            if (n >= (int)sizeof(nm)) n = sizeof(nm) - 1;
                            memcpy(nm, p, (size_t)n);
                            nm[n] = 0;
                            if (nm[0] && stricmp(nm, g_settings.handle) != 0)
                                safe_copy(g_last_dm_target, sizeof(g_last_dm_target), nm);
                        }
                    }

                    /* LISTING lines: /whoon, /list, /chatters output (no timestamp). */
                    if (strncmp(line, "LISTING ", 8) == 0) {
                        add_msg_wrapped(line + 8, CHAT_W);
                        g_last_line_start = -1;
                        if (g_scroll_off > 0) g_new_while_scrolled++;
                        draw_chat_msgs();
                        continue;
                    }

                    /* MENTION sidecar: flag the last chat line and log to MRCMENT.LOG. */
                    if (strncmp(line, "MENTION ", 8) == 0) {
                        const char *p = line + 8;
                        const char *tab = strchr(p, '\t');
                        char who[32];
                        int n = tab ? (int)(tab - p) : (int)strlen(p);
                        if (n >= (int)sizeof(who)) n = sizeof(who) - 1;
                        if (n > 0) memcpy(who, p, (size_t)n);
                        who[n > 0 ? n : 0] = 0;
                        g_mention_count++;
                        safe_copy(g_last_mentioner, sizeof(g_last_mentioner), who);
                        if (g_last_line_start >= 0 &&
                            g_last_line_start < g_msg_count) {
                            int k;
                            for (k = g_last_line_start; k < g_msg_count; ++k)
                                mention_set(k);
                        }
                        g_last_line_start = -1;
                        {
                            FILE *mf = fopen("MRCMENT.LOG", "a");
                            if (mf) {
                                char ts[8];
                                local_time_str(ts, sizeof(ts));
                                fprintf(mf, "%s\t%s\t%s\n",
                                        ts, who, tab ? tab + 1 : "");
                                fclose(mf);
                            }
                        }
                        draw_chat_info();
                        draw_chat_msgs();
                        redraw_chat_input();
                        continue;
                    }

                    /* Twit filter: suppress messages from ignored handles. */
                    if (g_settings.twit_count > 0 && is_mentionable_chat_line(line)) {
                        const char *twit_who = extract_sender(line);
                        if (twit_who && twit_has(twit_who)) {
                            continue;
                        }
                    }

                    /* Snapshot g_msg_count so a MENTION sidecar can flag wrap sub-lines. */
                    g_last_line_start = g_msg_count;
                    {
                        char ts[8], tline[MSG_LEN + 16];
                        local_time_str(ts, sizeof(ts));
                        snprintf(tline, sizeof(tline), "|08[%s]|07 %s", ts, line);
                        add_msg_wrapped(tline, CHAT_W);
                    }
                }

                if (g_scroll_off > 0)
                    g_new_while_scrolled++;

                draw_chat_msgs();
            }
        }

        if (stats_wait > 0) {
            --stats_wait;
            if (stats_wait == 0) {
                draw_stats("0 0 0 0");
                add_msg("|12TIMEOUT|07 No response from stats query.");
                g_mode = MODE_STATS;
            }
        }

        ch = fossil_getch_nonblock();
        if (ch < 0) {
            /* Yield to DOS via INT 28h and adaptively back off when idle. */
            if (!got_bridge) {
                _asm { int 28h }
                idle_count++;
                delay(idle_count > 10 ? 50 : 20);
            } else {
                idle_count = 0;
            }
            continue;
        }
        idle_count = 0;

        switch (g_mode) {

        case MODE_HANDLE:
            g_input_max = 20;
            if (ch == '\r' || ch == '\n') {
                char sanitized[32];
                sanitize_handle(sanitized, sizeof(sanitized), g_input);
                if (sanitized[0]) {
                    safe_copy(g_settings.handle, sizeof(g_settings.handle), sanitized);
                    save_settings(g_bbs_user, &g_settings);
                    g_input[0] = 0; g_input_len = 0;
                    g_mode = MODE_MENU;
                    draw_main_menu(&drop);
                }
            } else if ((ch == 8 || ch == 127) && g_input_len > 0) {
                g_input[--g_input_len] = 0;
                draw_handle_entry(&drop);
            } else if (isprint((unsigned char)ch) && g_input_len < g_input_max) {
                if ((unsigned char)ch != '~') {
                    g_input[g_input_len++] = (char)ch;
                    g_input[g_input_len] = 0;
                    draw_handle_entry(&drop);
                }
            } else if (ch == 27) {
                goto exit_loop;
            }
            break;

        case MODE_MENU:
            if (ch == '1') {
                g_msg_count = 0;
                g_chat_room[0] = 0; g_chat_topic[0] = 0;
                g_chat_users[0] = 0;
                safe_copy(g_chat_status, sizeof(g_chat_status), "CONNECTING");
                add_msg_wrapped("|14Connecting to MRC...|07", CHAT_W);
                add_msg_wrapped("|07Type |15/help|07 for commands, |15/chatters|07 for users.", CHAT_W);
                g_sent_quit = 0;
                safe_copy(status, sizeof(status), "CONNECTING");
                send_connect();
                g_mode = MODE_CHAT;
                draw_chat();
            } else if (ch == '2') {
                g_edit = EDIT_NONE;
                g_input[0] = 0; g_input_len = 0;
                g_mode = MODE_SETTINGS;
                draw_settings_menu();
            } else if (ch == '3') {
                draw_stats_waiting();
                bridge_send_line("QUICKSTATS");
                stats_wait = 200;
                g_mode = MODE_STATS;
            } else if (ch == '4') {
                g_help_page = 0;
                g_mode = MODE_HELP;
                draw_help();
            } else if (ch == '5' || ch == 'q' || ch == 'Q' || ch == 27) {
                goto exit_loop;
            }
            break;

        case MODE_SETTINGS:
            if (g_edit == EDIT_NONE) {
                g_input[0] = 0; g_input_len = 0;
                if (ch == '1') { g_edit = EDIT_HANDLE;    g_input_max = 20; }
                else if (ch == '2') { g_edit = EDIT_HCOLOR;    g_input_max = 3;  }
                else if (ch == '3') { g_edit = EDIT_PREFIX;    g_input_max = 30; }
                else if (ch == '4') { g_edit = EDIT_SUFFIX;    g_input_max = 30; }
                else if (ch == '5') { g_edit = EDIT_TEXTCOLOR; g_input_max = 3;  }
                else if (ch == '6') { g_edit = EDIT_ENTERROOM; g_input_max = 60; }
                else if (ch == '7') { g_edit = EDIT_LEAVEROOM; g_input_max = 60; }
                else if (ch == 'b' || ch == 'B' || ch == '0' || ch == 27) {
                    g_mode = MODE_MENU;
                    draw_main_menu(&drop);
                    break;
                }
                if (g_edit != EDIT_NONE)
                    draw_settings_menu();
            } else {
                if (ch == '\r' || ch == '\n') {
                    commit_settings_edit();
                    draw_settings_menu();
                } else if (ch == 27) {
                    g_edit = EDIT_NONE;
                    g_input[0] = 0; g_input_len = 0;
                    draw_settings_menu();
                } else if ((ch == 8 || ch == 127) && g_input_len > 0) {
                    g_input[--g_input_len] = 0;
                    draw_settings_menu();
                } else if (isprint((unsigned char)ch) &&
                           g_input_len < g_input_max &&
                           (unsigned char)ch != '~') {
                    g_input[g_input_len++] = (char)ch;
                    g_input[g_input_len] = 0;
                    draw_settings_menu();
                }
            }
            break;

        case MODE_STATS:
            stats_wait = 0;
            g_mode = MODE_MENU;
            draw_main_menu(&drop);
            break;

        case MODE_HELP:
            if (g_help_page == 0) {
                if (ch >= '1' && ch <= '4') {
                    g_help_page = ch - '0';
                    draw_help();
                } else if (ch == 'b' || ch == 'B' || ch == 27 || ch == '0') {
                    g_mode = MODE_MENU;
                    draw_main_menu(&drop);
                }
            } else {
                g_help_page = 0;
                draw_help();
            }
            break;

        case MODE_CHAT:
            {
                {
                    char _pr[64]; int _pv;
                    snprintf(_pr, sizeof(_pr), "%s|%02d%s%s|07 ",
                             g_settings.handle_prefix, g_settings.handle_color,
                             g_settings.handle, g_settings.handle_suffix);
                    _pv = pipe_vis_len(_pr);
                    g_chat_view_w = SCREEN_W - _pv - 1;
                    if (g_chat_view_w < 10) g_chat_view_w = 10;
                }
                g_input_max = 140;  /* MRC protocol limit */
                if (g_input_max >= (int)sizeof(g_input) - 1)
                    g_input_max = (int)sizeof(g_input) - 2;
            }

            /* Extended key prefix (0x00 scan code) */
            if (ch == 0) {
                int scan = fossil_getch_nonblock();
                if (scan < 0) break;
                if (scan == 73) { /* PgUp */
                    int max_scroll = g_msg_count > CHAT_MSG_ROWS ? g_msg_count - CHAT_MSG_ROWS : 0;
                    g_scroll_off += CHAT_MSG_ROWS;
                    if (g_scroll_off > max_scroll) g_scroll_off = max_scroll;
                    draw_chat_msgs();
                } else if (scan == 81) { /* PgDn */
                    g_scroll_off -= CHAT_MSG_ROWS;
                    if (g_scroll_off < 0) g_scroll_off = 0;
                    if (g_scroll_off == 0) g_new_while_scrolled = 0;
                    draw_chat_msgs();
                } else if (scan == 72) { /* Up arrow */
                    int max_scroll = g_msg_count > CHAT_MSG_ROWS ? g_msg_count - CHAT_MSG_ROWS : 0;
                    if (max_scroll > 0) {
                        g_scroll_off++;
                        if (g_scroll_off > max_scroll) g_scroll_off = max_scroll;
                        draw_chat_msgs();
                    }
                } else if (scan == 80) { /* Down arrow */
                    if (g_scroll_off > 0) {
                        g_scroll_off--;
                        if (g_scroll_off == 0) g_new_while_scrolled = 0;
                        draw_chat_msgs();
                    }
                } else if (scan == 71) { /* Home */
                    int max_scroll = g_msg_count > CHAT_MSG_ROWS ? g_msg_count - CHAT_MSG_ROWS : 0;
                    if (max_scroll > 0) {
                        g_scroll_off = max_scroll;
                        draw_chat_msgs();
                    }
                } else if (scan == 79) { /* End */
                    if (g_scroll_off > 0) {
                        g_scroll_off = 0;
                        g_new_while_scrolled = 0;
                        draw_chat_msgs();
                    }
                }
                break;
            }

            /* ESC or start of an ANSI escape sequence; bytes may arrive separately. */
            if (ch == 27) {
                int ch2, retries;
                ch2 = -1;
                for (retries = 0; retries < 25; ++retries) {
                    ch2 = fossil_getch_nonblock();
                    if (ch2 >= 0) break;
                    delay(2);
                }
                if (ch2 == '[' || ch2 == 'O') {
                    int ch3 = -1, ch4 = -1;
                    for (retries = 0; retries < 25; ++retries) {
                        ch3 = fossil_getch_nonblock();
                        if (ch3 >= 0) break;
                        delay(2);
                    }
                    if (ch3 == '5' || ch3 == 'I') { /* PgUp */
                        int max_scroll = g_msg_count > CHAT_MSG_ROWS ? g_msg_count - CHAT_MSG_ROWS : 0;
                        for (retries = 0; retries < 25; ++retries) {
                            ch4 = fossil_getch_nonblock(); (void)ch4;
                            if (ch4 >= 0) break;
                            delay(2);
                        }
                        g_scroll_off += CHAT_MSG_ROWS;
                        if (g_scroll_off > max_scroll) g_scroll_off = max_scroll;
                        draw_chat_msgs();
                    } else if (ch3 == '6' || ch3 == 'G') { /* PgDn */
                        for (retries = 0; retries < 25; ++retries) {
                            ch4 = fossil_getch_nonblock(); (void)ch4;
                            if (ch4 >= 0) break;
                            delay(2);
                        }
                        g_scroll_off -= CHAT_MSG_ROWS;
                        if (g_scroll_off < 0) g_scroll_off = 0;
                        if (g_scroll_off == 0) g_new_while_scrolled = 0;
                        draw_chat_msgs();
                    } else if (ch3 == 'A') { /* Up arrow */
                        if (g_scroll_off > 0 ||
                            (g_msg_count > CHAT_MSG_ROWS && g_input_len == 0)) {
                            int max_scroll = g_msg_count > CHAT_MSG_ROWS ?
                                             g_msg_count - CHAT_MSG_ROWS : 0;
                            g_scroll_off++;
                            if (g_scroll_off > max_scroll) g_scroll_off = max_scroll;
                            draw_chat_msgs();
                        } else if (g_sent_hist_count > 0) {
                            if (g_hist_browse < 0) {
                                safe_copy(g_hist_saved, sizeof(g_hist_saved), g_input);
                                g_hist_browse = 0;
                            } else if (g_hist_browse < g_sent_hist_count - 1) {
                                g_hist_browse++;
                            }
                            safe_copy(g_input, sizeof(g_input), g_sent_hist[g_hist_browse]);
                            g_input_len = (int)strlen(g_input);
                            g_tab_hl_start = -1; g_tab_hl_end = -1;
                            redraw_chat_input();
                        }
                    } else if (ch3 == 'B') { /* Down arrow */
                        if (g_scroll_off > 0) {
                            g_scroll_off--;
                            if (g_scroll_off == 0) g_new_while_scrolled = 0;
                            draw_chat_msgs();
                        } else if (g_hist_browse >= 0) {
                            g_hist_browse--;
                            if (g_hist_browse < 0) {
                                safe_copy(g_input, sizeof(g_input), g_hist_saved);
                            } else {
                                safe_copy(g_input, sizeof(g_input), g_sent_hist[g_hist_browse]);
                            }
                            g_input_len = (int)strlen(g_input);
                            g_tab_hl_start = -1; g_tab_hl_end = -1;
                            redraw_chat_input();
                        }
                    } else if (ch3 == 'C') { /* Right arrow: cycle text color forward */
                        {
                            char cmd[32];
                            g_settings.text_color = (g_settings.text_color + 1) % 16;
                            save_settings(g_bbs_user, &g_settings);
                            snprintf(cmd, sizeof(cmd),"SET TEXTCOLOR %d", g_settings.text_color);
                            bridge_send_line(cmd);
                            redraw_chat_input();
                        }
                    } else if (ch3 == 'D') { /* Left arrow: cycle text color backward */
                        {
                            char cmd[32];
                            g_settings.text_color = (g_settings.text_color + 15) % 16;
                            save_settings(g_bbs_user, &g_settings);
                            snprintf(cmd, sizeof(cmd),"SET TEXTCOLOR %d", g_settings.text_color);
                            bridge_send_line(cmd);
                            redraw_chat_input();
                        }
                    } else if (ch3 == '1' || ch3 == 'H') { /* Home */
                        for (retries = 0; retries < 15; ++retries) {
                            ch4 = fossil_getch_nonblock(); (void)ch4;
                            if (ch4 >= 0) break;
                            delay(2);
                        }
                        {
                            int max_scroll = g_msg_count > CHAT_MSG_ROWS ? g_msg_count - CHAT_MSG_ROWS : 0;
                            if (max_scroll > 0) {
                                g_scroll_off = max_scroll;
                                draw_chat_msgs();
                            }
                        }
                    } else if (ch3 == '4' || ch3 == 'F' || ch3 == 'K') { /* End */
                        for (retries = 0; retries < 15; ++retries) {
                            ch4 = fossil_getch_nonblock(); (void)ch4;
                            if (ch4 >= 0) break;
                            delay(2);
                        }
                        if (g_scroll_off > 0) {
                            g_scroll_off = 0;
                            g_new_while_scrolled = 0;
                            draw_chat_msgs();
                            redraw_chat_input();
                        }
                    }
                    ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col());
                    break;
                }
                if (ch2 >= 0) {
                    ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col());
                    break;
                }
                /* Plain ESC: return to live view if scrolled, else disconnect. */
                if (g_scroll_off > 0) {
                    g_scroll_off = 0;
                    g_new_while_scrolled = 0;
                    draw_chat_msgs();
                    redraw_chat_input();
                    break;
                }
                g_scroll_off = 0;
                g_new_while_scrolled = 0;
                g_mention_count = 0;
                g_last_mentioner[0] = 0;
                remove("MRCMENT.LOG");
                g_hist_browse = -1;
                send_quit_once();
                g_mode = MODE_MENU;
                draw_main_menu(&drop);
                break;
            }

            if (ch == '\t') {
                if (tab_complete())
                    redraw_chat_input();
                break;
            }

            if (ch == '\r' || ch == '\n') {
                g_tab_hl_start = -1; g_tab_hl_end = -1;
                g_hist_browse = -1;   /* stop browsing on send */
                g_input[g_input_len] = 0;
                if (g_input_len > 0) {
                    /* Skip history for password-bearing commands. */
                    int is_secret =
                        strncmp(g_input, "/identify ", 10) == 0 ||
                        strncmp(g_input, "/register ", 10) == 0 ||
                        strncmp(g_input, "/roompass ", 10) == 0 ||
                        strncmp(g_input, "/update password ", 17) == 0;
                    if (!is_secret) {
                        int hi;
                        for (hi = SENT_HIST_MAX - 1; hi > 0; --hi)
                            safe_copy(g_sent_hist[hi], sizeof(g_sent_hist[hi]), g_sent_hist[hi-1]);
                        safe_copy(g_sent_hist[0], sizeof(g_sent_hist[0]), g_input);
                        if (g_sent_hist_count < SENT_HIST_MAX) g_sent_hist_count++;
                    }

                    if (g_input[0] == '/') {
                        handle_slash_command(g_input);
                        if (g_return_to_menu) {
                            g_return_to_menu = 0;
                            g_scroll_off = 0;
                            g_new_while_scrolled = 0;
                            g_mention_count = 0;
                            g_last_mentioner[0] = 0;
                            g_hist_browse = -1;
                            send_quit_once();
                            g_mode = MODE_MENU;
                            g_input_len = 0;
                            g_input[0] = 0;
                            draw_main_menu(&drop);
                            break;
                        }
                        draw_chat_msgs();
                    } else {
                        char cmd[512];
                        g_scroll_off = 0;
                        g_new_while_scrolled = 0;
                        snprintf(cmd, sizeof(cmd),"SEND %s", g_input);
                        bridge_send_line(cmd);
                    }
                    g_input_len = 0;
                    g_input[0] = 0;
                    draw_char_count_only();
                }
                redraw_chat_input();
            } else if ((ch == 8 || ch == 127) && g_input_len > 0) {
                g_tab_hl_start = -1; g_tab_hl_end = -1;
                g_input[--g_input_len] = 0;
                ansi_color_code(g_settings.text_color);
                if (g_input_len + 1 > g_chat_view_w) {
                    redraw_chat_input();
                } else {
                    fossil_puts("\b \b");
                }
                draw_char_count_only();
                ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col());
            } else if (isprint((unsigned char)ch) &&
                       g_input_len < g_input_max) {
                int ms, me, vis;
                g_tab_hl_start = -1; g_tab_hl_end = -1;
                g_input[g_input_len++] = (char)ch;
                g_input[g_input_len]   = 0;
                masked_range(&ms, &me);
                vis = ((g_input_len - 1) >= ms && (g_input_len - 1) < me)
                      ? '*' : (unsigned char)ch;
                if (g_input_len > g_chat_view_w) {
                    redraw_chat_input();
                } else {
                    ansi_color_code(g_settings.text_color);
                    fossil_putch((unsigned char)vis);
                }
                draw_char_count_only();
                ansi_goto(INPUT_PROMPT_ROW, (int)chat_cursor_col());
            }
            break;
        }
    }

exit_loop:
    send_quit_once();
    bridge_close();
    fossil_deinit();
    return 0;
}
