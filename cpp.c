#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include "tok.h"

static char buf[BUFSIZE];
static int len;
static int cur;

#define MAXDEFS			(1 << 10)
#define MACROLEN		(1 << 10)

static struct define {
	char name[NAMELEN];
	char def[MACROLEN];
} defines[MAXDEFS];
static int ndefines;

void cpp_init(int fd)
{
	int n = 0;
	while ((n = read(fd, buf + len, sizeof(buf) - len)) > 0)
		len += n;
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
}

static int macro_find(char *name)
{
	int i;
	for (i = 0; i < ndefines; i++)
		if (!strcmp(name, defines[i].name))
			return i;
	return -1;
}

static int macro_expand(char *s)
{
	char name[NAMELEN];
	struct define *m;
	read_word(name);
	m = &defines[macro_find(name)];
	strcpy(s, m->def);
	return strlen(m->def);
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
	int old = cur;
	if (cur == len)
		return -1;
	if (buf[cur] == '#') {
		cpp_cmd();
		return 0;
	}
	if (definedword) {
		definedword = 0;
		return macro_expand(s);
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
