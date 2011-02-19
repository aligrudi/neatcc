/*
 * neatpp - a small and simple C preprocessor
 *
 * Copyright (C) 2010-2011 Ali Gholami Rudi
 *
 * This file is released under GNU GPL version 2.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "tok.h"

int main(int argc, char *argv[])
{
	int ofd = 1;
	int i = 1;
	char buf[BUFSIZE];
	int nr;
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
		die("neatcc: cannot open input file\n");
	ofd = open(argv[i++], O_WRONLY | O_TRUNC | O_CREAT, 0600);
	if (ofd < 0)
		die("%s: cannot open output file\n");
	while ((nr = cpp_read(buf)) >= 0)
		write(ofd, buf, nr);
	close(ofd);
	return 0;
}
