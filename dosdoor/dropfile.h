/* dropfile.h — DOOR.SYS parser types and API for the DOS door. */
#ifndef ANET_DOS_DROPFILE_H
#define ANET_DOS_DROPFILE_H

typedef struct {
    int active;
    int comm_type;
    int comm_port;
    char alias[64];
    char user_name[64];
    char location[64];
    char dropfile_path[260];
} dropfile_info_t;

int dropfile_load(dropfile_info_t *out, const char *path);
const char *dropfile_get_path_from_args(int argc, char **argv);

#endif
