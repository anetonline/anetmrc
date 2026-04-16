/* common.h — shared includes and small string helpers for the DOS door. */
#ifndef ANET_DOS_COMMON_H
#define ANET_DOS_COMMON_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#define IRC_LINE_MAX 512
#define PROFILE_MAX 8
static void safe_copy(char *dst, size_t dstsz, const char *src){size_t i;if(!dst||dstsz==0)return;if(!src)src="";for(i=0;i+1<dstsz&&src[i];++i)dst[i]=src[i];dst[i]=0;}
static void trim_crlf(char *s){size_t n;if(!s)return;n=strlen(s);while(n&&(s[n-1]=='\r'||s[n-1]=='\n'))s[--n]=0;}
#endif
