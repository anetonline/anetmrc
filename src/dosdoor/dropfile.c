/* dropfile.c — parse DOOR.SYS to extract COM port and user identity. */
#include "common.h"
#include "dropfile.h"

/* Overwrite dst with src when src looks more informative. */
static void copy_if_better(char *dst, size_t dstsz, const char *src) {
    if (!src || !*src) return;
    if (!dst[0] || strlen(src) > 1) safe_copy(dst, dstsz, src);
}

/* Parse "COMn" or "COMn:"; returns 0-based port index or -1. */
static int parse_com_from_line(const char *s) {
    int n = 0;
    if (!s) return -1;

    if (sscanf(s, "COM%d:", &n) == 1 || sscanf(s, "COM%d", &n) == 1) {
        if (n >= 1 && n <= 8) return n - 1;
    }
    return -1;
}

/* Extract the dropfile path from command-line args, defaulting to DOOR.SYS. */
const char *dropfile_get_path_from_args(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--dropfile=", 11) == 0) return argv[i] + 11;
        if (strcmp(argv[i], "--dropfile") == 0 && i + 1 < argc) return argv[i + 1];
    }
    return "DOOR.SYS";
}

/* Load a DOOR.SYS-style dropfile and populate out with user and port info. */
int dropfile_load(dropfile_info_t *out, const char *path) {
    FILE *fp;
    char line[256];
    char lines[64][256];
    int count = 0;
    int i;

    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    safe_copy(out->dropfile_path, sizeof(out->dropfile_path), path ? path : "DOOR.SYS");

    fp = fopen(out->dropfile_path, "r");
    if (!fp) return 0;

    while (count < 64 && fgets(line, sizeof(line), fp)) {
        trim_crlf(line);
        safe_copy(lines[count], sizeof(lines[count]), line);
        ++count;
    }
    fclose(fp);

    out->active = 1;

    /* Line 1 is usually "COMn:"; accept a bare digit as a fallback. */
    if (count > 0) {
        int p = parse_com_from_line(lines[0]);
        if (p >= 0) {
            out->comm_port = p;
        } else {
            out->comm_type = atoi(lines[0]);
            if (out->comm_type >= 1 && out->comm_type <= 8)
                out->comm_port = out->comm_type - 1;
        }
    }

    /* Last-resort scan for COMn anywhere in the file. */
    if (out->comm_port == 0) {
        for (i = 0; i < count; ++i) {
            int p = parse_com_from_line(lines[i]);
            if (p >= 0) { out->comm_port = p; break; }
        }
    }

    if (count > 5) copy_if_better(out->user_name, sizeof(out->user_name), lines[5]);
    if (count > 6) copy_if_better(out->location, sizeof(out->location), lines[6]);
    if (count > 7) copy_if_better(out->alias, sizeof(out->alias), lines[7]);

    if (count > 9)  copy_if_better(out->alias, sizeof(out->alias), lines[9]);
    if (count > 10) copy_if_better(out->location, sizeof(out->location), lines[10]);

    if (!out->user_name[0] && out->alias[0])
        safe_copy(out->user_name, sizeof(out->user_name), out->alias);
    if (!out->alias[0] && out->user_name[0])
        safe_copy(out->alias, sizeof(out->alias), out->user_name);
    if (!out->location[0])
        safe_copy(out->location, sizeof(out->location), "Unknown");

    if (strlen(out->alias) <= 1 && count > 9 && strlen(lines[9]) > 1)
        safe_copy(out->alias, sizeof(out->alias), lines[9]);
    if (strlen(out->user_name) <= 1 && count > 9 && strlen(lines[9]) > 1)
        safe_copy(out->user_name, sizeof(out->user_name), lines[9]);
    if (strlen(out->location) <= 1 && count > 10 && strlen(lines[10]) > 1)
        safe_copy(out->location, sizeof(out->location), lines[10]);

    return 1;
}
