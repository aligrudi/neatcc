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
#define MAXARGS			(1 << 5)

static struct macro {
	char name[NAMELEN];
	char def[MACROLEN];
	char args[MAXARGS][NAMELEN];
	int nargs;
	int isfunc;
} macros[MAXDEFS];
static int nmacros;

#define MAXBUFS			(1 << 3)

static struct buf {
	char buf[BUFSIZE];
	int len;
	int cur;
} bufs[MAXBUFS];
static int nbufs;

static void buf_new(void)
{
	if (nbufs) {
		bufs[nbufs - 1].cur = cur;
		bufs[nbufs - 1].len = len;
	}
	nbufs++;
	cur = 0;
	len = 0;
	buf = bufs[nbufs - 1].buf;
}

static void buf_pop(void)
{
	nbufs--;
	if (nbufs) {
		cur = bufs[nbufs - 1].cur;
		len = bufs[nbufs - 1].len;
		buf = bufs[nbufs - 1].buf;
	}
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
	while (cur < len && buf[cur] != '\n')
		if (buf[cur] == '\\')
			cur += 2;
		else
			*dst++ = buf[cur++];
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

static char *locs[MAXLOCS] = {"/usr/include"};
static int nlocs = 1;

void cpp_addpath(char *s)
{
	locs[nlocs++] = s;
}

static int include_find(char *name, int std)
{
	int i;
	int fd;
	for (i = std ? nlocs - 1 : nlocs; i >= 0; i--) {
		char path[1 << 10];
		char *s;
		s = path;
		if (locs[i]) {
			s = putstr(s, locs[i]);
			*s++ = '/';
		}
		s = putstr(s, name);
		fd = open(path, O_RDONLY);
		if (fd != -1)
			return fd;
	}
	return -1;
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

static void readarg(char *s)
{
	int depth = 0;
	int beg = cur;
	while (cur < len && (depth || buf[cur] != ',' && buf[cur] != ')')) {
		switch (buf[cur]) {
		case '(':
		case '[':
		case '{':
			cur++;
			depth++;
			break;
		case ')':
		case ']':
		case '}':
			cur++;
			depth--;
			break;
		case '\'':
		case '"':
			jumpstr();
			break;
		default:
			if (buf[cur] == '/' && buf[cur + 1] == '*')
				jumpcomment();
			else
				cur++;
		}
	}
	memcpy(s, buf + beg, cur - beg);
	s[cur - beg] = '\0';
}

static int macro_find(char *name)
{
	int i;
	for (i = 0; i < nmacros; i++)
		if (!strcmp(name, macros[i].name))
			return i;
	return -1;
}

static int macro_new(char *name)
{
	int i;
	for (i = 0; i < nmacros; i++) {
		if (!strcmp(name, macros[i].name))
			return i;
		if (!*macros[i].name) {
			strcpy(macros[i].name, name);
			return i;
		}
	}
	strcpy(macros[nmacros++].name, name);
	return nmacros - 1;
}

static void macro_define(void)
{
	char name[NAMELEN];
	struct macro *d;
	read_word(name);
	d = &macros[macro_new(name)];
	d->isfunc = 0;
	if (buf[cur++] == '(') {
		jumpws();
		while (cur < len && buf[cur] != ')') {
			readarg(d->args[d->nargs++]);
			jumpws();
			if (buf[cur] != ',')
				break;
			jumpws();
		}
		d->isfunc = 1;
		cur++;
	}
	read_tilleol(d->def);
}

static void jumpifs(int jumpelse)
{
	int depth = 0;
	while (cur < len) {
		if (buf[cur] == '#') {
			char cmd[NAMELEN];
			cur++;
			read_word(cmd);
			if (!strcmp("else", cmd))
				if (!depth && !jumpelse)
					break;
			if (!strcmp("endif", cmd))
				if (!depth)
					break;
				else
					depth--;
			if (!strcmp("ifdef", cmd) || !strcmp("ifndef", cmd))
				depth++;
		}
		if (buf[cur] == '/' && buf[cur + 1] == '*') {
			jumpcomment();
			continue;
		}
		if (buf[cur] == '\'' || buf[cur] == '"') {
			jumpstr();
			continue;
		}
		cur++;
	}
}

static void cpp_cmd(void)
{
	char cmd[NAMELEN];
	cur++;
	read_word(cmd);
	if (!strcmp("define", cmd)) {
		macro_define();
		return;
	}
	if (!strcmp("undef", cmd)) {
		char name[NAMELEN];
		int idx;
		read_word(name);
		idx = macro_find(name);
		if (idx != -1)
			strcpy(macros[idx].name, "");
		return;
	}
	if (!strcmp("ifdef", cmd) || !strcmp("ifndef", cmd)) {
		char name[NAMELEN];
		int not = cmd[2] == 'n';
		read_word(name);
		if (!not && macro_find(name) < 0 || not && macro_find(name) >= 0)
			jumpifs(0);
		return;
	}
	if (!strcmp("else", cmd)) {
		jumpifs(1);
		return;
	}
	if (!strcmp("endif", cmd))
		return;
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

static int macro_arg(struct macro *m, char *arg)
{
	int i;
	for (i = 0; i < m->nargs; i++)
		if (!strcmp(arg, m->args[i]))
			return i;
	return -1;
}

static void macro_expand(void)
{
	char name[NAMELEN];
	char args[MAXARGS][MACROLEN];
	int nargs;
	struct macro *m;
	char *dst;
	int dstlen = 0;
	int beg;
	read_word(name);
	m = &macros[macro_find(name)];
	if (!m->isfunc) {
		buf_new();
		strcpy(buf, m->def);
		len = strlen(m->def);
		return;
	}
	if (buf[cur] == '(') {
		cur++;
		jumpws();
		while (cur < len && buf[cur] != ')') {
			readarg(args[nargs++]);
			jumpws();
			if (buf[cur] != ',')
				break;
			jumpws();
		}
		cur++;
		m->isfunc = 1;
	}
	buf_new();
	dst = buf;
	buf = m->def;
	len = strlen(m->def);
	beg = cur;
	while (cur < len) {
		if (buf[cur] == '/' && buf[cur + 1] == '*') {
			jumpcomment();
			continue;
		}
		if (strchr("'\"", buf[cur])) {
			jumpstr();
			continue;
		}
		if (isalpha(buf[cur]) || buf[cur] == '_') {
			int arg;
			char word[NAMELEN];
			read_word(word);
			if ((arg = macro_arg(m, word)) != -1) {
				int len = cur - beg - strlen(word);
				char *argstr = arg > nargs ? "" : args[arg];
				int arglen = strlen(argstr);
				memcpy(dst + dstlen, buf + beg, len);
				dstlen += len;
				memcpy(dst + dstlen, argstr, arglen);
				dstlen += arglen;
				beg = cur;
			}
			continue;
		}
		cur++;
	}
	memcpy(dst + dstlen, buf + beg, len - beg);
	dstlen += len - beg;
	buf = dst;
	len = dstlen;
	cur = 0;
	buf[len] = '\0';
}

void cpp_define(char *name, char *def)
{
	char *s;
	buf_new();
	s = buf;
	s = putstr(s, name);
	*s++ = '\t';
	s = putstr(s, def);
	len = s - buf;
	macro_define();
	buf_pop();
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
		if (buf[cur] == '/' && buf[cur + 1] == '*') {
			jumpcomment();
			continue;
		}
		if (buf[cur] == '\'' || buf[cur] == '"') {
			jumpstr();
			continue;
		}
		if (isalpha(buf[cur]) || buf[cur] == '_') {
			char word[NAMELEN];
			read_word(word);
			if (macro_find(word) != -1) {
				cur -= strlen(word);
				definedword = 1;
				break;
			}
			continue;
		}
		cur++;
	}
	memcpy(s, buf + old, cur - old);
	return cur - old;
}
