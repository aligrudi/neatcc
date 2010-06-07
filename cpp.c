#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tok.h"

static char *buf;
static int len;
static int cur;

#define MAXDEFS			(1 << 10)
#define MACROLEN		(1 << 10)

static struct define {
	char name[NAMELEN];
	char def[MACROLEN];
} defines[MAXDEFS];
static int ndefines;

#define MAXBUFS			(1 << 3)

static struct buf {
	char buf[BUFSIZE];
	int len;
	int cur;
} bufs[MAXBUFS];
static int nbufs;

static void buf_new(void)
{
	bufs[nbufs - 1].cur = cur;
	bufs[nbufs - 1].len = len;
	nbufs++;
	cur = 0;
	len = 0;
	buf = bufs[nbufs - 1].buf;
}

static void buf_pop(void)
{
	nbufs--;
	cur = bufs[nbufs - 1].cur;
	len = bufs[nbufs - 1].len;
	buf = bufs[nbufs - 1].buf;
}

static void include(int fd)
{
	int n = 0;
	buf_new();
	while ((n = read(fd, buf + len, BUFSIZE - len)) > 0)
		len += n;
}

void cpp_init(int fd)
{
	include(fd);
}

static void jumpws(void)
{
	while (cur < len && isspace(buf[cur]))
		cur++;
}

static void read_word(char *dst)
{
	jumpws();
	while (cur < len && isalnum(buf[cur]) || buf[cur] == '_')
		*dst++ = buf[cur++];
	*dst = '\0';
}

static void read_tilleol(char *dst)
{
	while (cur < len && buf[cur] != '\n') {
		if (buf[cur] == '\\')
			cur += 2;
		else
			*dst++ = buf[cur++];
	}
	*dst = '\0';
}

static char *putstr(char *d, char *s)
{
	while (*s)
		*d++ = *s++;
	*d = '\0';
	return d;
}

#define MAXLOCS			(1 << 10)

static char *locs[MAXLOCS] = {".", NULL, "/usr/include"};
static int nlocs = 3;

void cpp_addpath(char *s)
{
	locs[nlocs++] = s;
}

static int include_find(char *name, int std)
{
	int i;
	int fd;
	for (i = std ? 1 : 0; i < nlocs; i++) {
		char path[1 << 10];
		char *s;
		if (!locs[i] && *name != '/')
			continue;
		s = putstr(path, locs[i]);
		if (locs[i])
			*s++ = '/';
		s = putstr(s, name);
		fd = open(path, O_RDONLY);
		if (fd != -1)
			return fd;
	}
	return -1;
}

static void cpp_cmd(void)
{
	char cmd[NAMELEN];
	cur++;
	read_word(cmd);
	if (!strcmp("define", cmd)) {
		struct define *d = &defines[ndefines++];
		read_word(d->name);
		read_tilleol(d->def);
		return;
	}
	if (!strcmp("include", cmd)) {
		char file[NAMELEN];
		char *s, *e;
		int fd;
		jumpws();
		s = buf + cur + 1;
		e = strchr(buf + cur + 1, buf[cur] == '"' ? '"' : '>');
		memcpy(file, s, e - s);
		file[e - s] = '\0';
		cur += e - s + 2;
		fd = include_find(file, *e == '>');
		if (fd == -1)
			return;
		include(fd);
		close(fd);
		return;
	}
}

static int macro_find(char *name)
{
	int i;
	for (i = 0; i < ndefines; i++)
		if (!strcmp(name, defines[i].name))
			return i;
	return -1;
}

static void macro_expand(void)
{
	char name[NAMELEN];
	struct define *m;
	read_word(name);
	buf_new();
	m = &defines[macro_find(name)];
	strcpy(buf, m->def);
	len = strlen(m->def);
}

static void jumpstr(void)
{
	if (buf[cur] == '\'') {
		while (cur < len && buf[++cur] != '\'')
			if (buf[cur] == '\\')
				cur++;
		cur++;
		return;
	}
	if (buf[cur] == '"') {
		while (cur < len && buf[++cur] != '"')
			if (buf[cur] == '\\')
				cur++;
		cur++;
		return;
	}
}

static void jumpcomment(void)
{
	while (++cur < len) {
		if (buf[cur] == '*' && buf[cur + 1] == '/') {
			cur += 2;
			break;
		}
	}
}

static int definedword;

int cpp_read(char *s)
{
	int old;
	if (definedword) {
		definedword = 0;
		macro_expand();
	}
	if (cur == len) {
		if (nbufs < 2)
			return -1;
		buf_pop();
	}
	old = cur;
	if (buf[cur] == '#') {
		cpp_cmd();
		return 0;
	}
	while (cur < len) {
		if (buf[cur] == '#')
			break;
		if (buf[cur] == '/' && buf[cur + 1] == '*')
			jumpcomment();
		if (strchr("'\"", buf[cur]))
			jumpstr();
		if (isalpha(buf[cur]) || buf[cur] == '_') {
			char word[NAMELEN];
			read_word(word);
			if (macro_find(word) != -1) {
				cur -= strlen(word);
				definedword = 1;
				break;
			}
		}
		cur++;
	}
	memcpy(s, buf + old, cur - old);
	return cur - old;
}
