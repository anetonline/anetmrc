/* helper_protocol.h — public API for the MRC helper core loop. */
#ifndef ANET_HELPER_PROTOCOL_H
#define ANET_HELPER_PROTOCOL_H
extern int helper_verbose;   /* set to 1 by --verbose / -d flag */
void helper_log(const char *fmt, ...);
void helper_set_node(int n); /* call before helper_run(), n = COM port 1-8 */
void helper_run(void);
#endif
