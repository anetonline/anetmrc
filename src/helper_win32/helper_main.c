/* helper_main.c — entry point for the Win32 MRC bridge helper. */
#include "helper_common.h"
#include "helper_protocol.h"
#include <string.h>
#include <stdio.h>

int helper_verbose = 0;

/* Read the COM port digit from DOOR.SYS line 1; returns 1-8 or 0. */
static int node_from_doorsys(void) {
    FILE *fp;
    char line[128];
    int n = 0;

    fp = fopen("DOOR.SYS", "r");
    if (!fp) return 0;
    if (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "COM%d", &n) == 1 && n >= 1 && n <= 8) {
            fclose(fp);
            return n;
        }
    }
    fclose(fp);
    return 0;
}

/* Parse args, pick the node, start Winsock, and enter the helper loop. */
int main(int argc, char *argv[]) {
    WSADATA wsa;
    int i, node = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-d") == 0) {
            helper_verbose = 1;
        } else if ((strcmp(argv[i], "-node") == 0 || strcmp(argv[i], "-port") == 0) &&
                    i + 1 < argc) {
            node = atoi(argv[++i]);
        } else if (strncmp(argv[i], "-node", 5) == 0) {
            node = atoi(argv[i] + 5);
        } else if (strncmp(argv[i], "-port", 5) == 0) {
            node = atoi(argv[i] + 5);
        }
    }

    /* Fall back to DOOR.SYS when no -node argument is given. */
    if (node == 0)
        node = node_from_doorsys();

    if (node > 0) helper_set_node(node);

    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return 1;
    helper_run();
    WSACleanup();
    return 0;
}
