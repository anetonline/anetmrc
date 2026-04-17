/* fossil.h — FOSSIL/local-console I/O API for the DOS door. */
#ifndef ANET_DOS_FOSSIL_H
#define ANET_DOS_FOSSIL_H
void fossil_set_port(unsigned short port);
void fossil_set_local_mode(void);
int fossil_init(void);
void fossil_deinit(void);
int fossil_getch_nonblock(void);
int fossil_getch_block(void);
void fossil_putch(int ch);
void fossil_puts(const char *s);
void fossil_printf(const char *fmt, ...);
void fossil_cls(void);
#endif
