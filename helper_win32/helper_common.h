/* helper_common.h — shared includes and version string for the Win32 helper. */
#ifndef ANET_HELPER_COMMON_H
#define ANET_HELPER_COMMON_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define HELPER_LINE_MAX 1024

/* MRC protocol VERSION_INFO sent in the handshake and CTCP VERSION replies.
 * Format: CLIENT/Platform.Arch/ProtocolVersion  (see MRC-DEV-DOCS.TXT). */
#define ANETMRC_VERSION_INFO "ANETMRC/Windows.i386/1.3.6"

#endif
