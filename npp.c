/*
 * neatpp - a small and simple C preprocessor
 *
 * Copyright (C) 2010-2012 Ali Gholami Rudi
 *
 * This file is released under GNU GPL version 2.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "tok.h"

#define OBUFSZ		(1 << 19)

static int rmcomments(char *d, char *s, int l)
{
	char *e = s + l;
	char *d1 = d;
	char *r = s;
	while (r < e) {
		if (r + 3 < e && r[0] == '/' && r[1] == '*') {
			memcpy(d, s, r - s);
			d += r - s;
			while (r + 1 < e) {
				r++;
				if (r[0] == '*' && r[1] == '/') {
					r++;
					break;
				}
			}
			s = r + 1;
		}
		if (r + 1 < e && (r[0] == '\'' || r[0] == '"')) {
			char c = r[0];
			r++;
			while (r < e && *r != c) {
				if (*r == '\\')
					r++;
				r++;
			}
		}
		r++;
	}
	memcpy(d, s, e - s);
	d += e - s;
	return d - d1;
}

static int xwrite(int fd, char *buf, int len)
{
	int nw = 0;
	while (nw < len) {
		int ret = write(fd, buf + nw, len - nw);
		if (ret == -1 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (ret < 0)
			break;
		nw += ret;
	}
	return nw;
}

void err(char *fmt, ...)
{
	va_list ap;
	char msg[512];
	va_start(ap, fmt);
	vsprintf(msg, fmt, ap);
	va_end(ap);
	die(msg);
}

int main(int argc, char *argv[])
{
	int ofd = 1;
	int i = 1;
	char *s1, *s2;
	int nr;
	int len = 0;
	while (i < argc && argv[i][0] == '-') {
		if (argv[i][1] == 'I')
			cpp_addpath(argv[i][2] ? argv[i] + 2 : argv[++i]);
		if (argv[i][1] == 'D') {
			char *name = argv[i] + 2;
			char *def = "";
			char *eq = strchr(name, '=');
			if (eq) {
				*eq = '\0';
				def = eq + 1;
			}
			cpp_define(name, def);
		}
		i++;
	}
	if (i + 1 >= argc) {
		printf("usage: npp [-I idir] [-D define] input output\n");
		return 0;
	}
	if (cpp_init(argv[i++]))
		die("npp: cannot open <%s>\n", argv[i - 1]);
	ofd = open(argv[i++], O_WRONLY | O_TRUNC | O_CREAT, 0600);
	if (ofd < 0)
		die("npp: cannot open <%s>\n", argv[i - 1]);
	s1 = malloc(OBUFSZ);
	s2 = malloc(OBUFSZ);
	if (!s1 || !s2)
		die("npp: cannot allocate enough memory\n");
	while ((nr = cpp_read(s1 + len)) >= 0)
		len += nr;
	len = rmcomments(s2, s1, len);
	xwrite(ofd, s2, len);
	close(ofd);
	return 0;
}
