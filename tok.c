#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include "tok.h"

static char buf[BUFSIZE];
static int len;
static int cur;
static char name[NAMELEN];
static int next;

static struct {
	char *name;
	unsigned id;
} kwds[] = {
	{"void", TOK_VOID},
	{"static", TOK_STATIC},
	{"return", TOK_RETURN},
	{"unsigned", TOK_UNSIGNED},
	{"signed", TOK_SIGNED},
	{"short", TOK_SHORT},
	{"long", TOK_LONG},
	{"int", TOK_INT},
	{"char", TOK_CHAR},
	{"struct", TOK_STRUCT},
	{"enum", TOK_ENUM},
	{"if", TOK_IF},
	{"else", TOK_ELSE},
	{"for", TOK_FOR},
	{"while", TOK_WHILE},
	{"do", TOK_DO},
	{"switch", TOK_SWITCH},
	{"case", TOK_CASE},
	{"sizeof", TOK_SIZEOF},
};

static int id_char(int c)
{
	return isalnum(c) || c == '_';
}

int tok_get(void)
{
	if (next != -1) {
		int tok = next;
		next = -1;
		return tok;
	}
	while (cur < len && isspace(buf[cur]))
		cur++;
	if (cur == len)
		return TOK_EOF;
	if (isdigit(buf[cur])) {
		char *s = name;
		while (cur < len && isdigit(buf[cur]))
			*s++ = buf[cur++];
		*s = '\0';
		return TOK_NUM;
	}
	if (id_char(buf[cur])) {
		char *s = name;
		int i;
		while (cur < len && id_char(buf[cur]))
			*s++ = buf[cur++];
		*s = '\0';
		for (i = 0; i < ARRAY_SIZE(kwds); i++)
			if (!strcmp(kwds[i].name, name))
				return kwds[i].id;
		return TOK_NAME;
	}
	if (strchr(";{}()[]*=", buf[cur]))
		return buf[cur++];
	return -1;
}

int tok_see(void)
{
	if (next == -1)
		next = tok_get();
	return next;
}

void tok_init(int fd)
{
	int n = 0;
	while ((n = read(fd, buf + len, sizeof(buf) - len)) > 0)
		len += n;
	next = -1;
}

char *tok_id(void)
{
	return name;
}
