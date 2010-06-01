#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
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
	{"extern", TOK_EXTERN},
	{"return", TOK_RETURN},
	{"unsigned", TOK_UNSIGNED},
	{"signed", TOK_SIGNED},
	{"short", TOK_SHORT},
	{"long", TOK_LONG},
	{"int", TOK_INT},
	{"char", TOK_CHAR},
	{"struct", TOK_STRUCT},
	{"union", TOK_UNION},
	{"enum", TOK_ENUM},
	{"typedef", TOK_TYPEDEF},
	{"if", TOK_IF},
	{"else", TOK_ELSE},
	{"for", TOK_FOR},
	{"while", TOK_WHILE},
	{"do", TOK_DO},
	{"switch", TOK_SWITCH},
	{"case", TOK_CASE},
	{"sizeof", TOK_SIZEOF},
	{"break", TOK_BREAK},
	{"continue", TOK_CONTINUE},
	{"default", TOK_DEFAULT},
	{"goto", TOK_GOTO},
};

static char *tok3[] = {
	"<<", ">>", "++", "--", "<<=", ">>=", "...", "+=", "-=", "*=", "/=",
	"%=", "|=", "&=", "^=", "&&", "||", "==", "!=", "<=", ">=", "->", "/*"
};

static int get_tok3(int num)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(tok3); i++)
		if (num == TOK3(tok3[i]))
			return num;
	return 0;
}

static char *esc_code = "abefnrtv";
static char *esc = "\a\b\e\f\n\r\t\v";

static int esc_char(int *c, char *s)
{
	if (*s != '\\') {
		*c = *s;
		return 1;
	}
	if (strchr(esc_code, s[1])) {
		*c = esc[strchr(esc_code, s[1]) - esc_code];
		return 2;
	}
	*c = s[1];
	return 2;
}

long tok_num(void)
{
	if (buf[cur] == '0' && buf[cur + 1] == 'x') {
		long result = 0;
		cur += 2;
		while (isalnum(buf[cur])) {
			int c = buf[cur];
			result <<= 4;
			if (c >= '0' && c <= '9')
				result |= c - '0';
			else
				result |= 10 + tolower(c) - 'a';
			cur++;
		}
		return result;
	}
	if (isdigit(buf[cur])) {
		long result = 0;
		while (isdigit(buf[cur])) {
			result *= 10;
			result += buf[cur++] - '0';
		}
		return result;
	}
	if (buf[cur] == '\'') {
		int ret;
		cur += 2 + esc_char(&ret, buf + cur + 1);
		return ret;
	}
	return -1;
}

int tok_str(char *out)
{
	char *s = out;
	char *r = buf + cur;
	char *e = buf + len;
	r++;
	while (r < e && *r != '"') {
		if (*r == '\\') {
			int c;
			r += esc_char(&c, r);
			*s++ = c;
		} else {
			*s++ = *r++;
		}
	}
	*s++ = '\0';
	cur = r - buf + 1;
	return s - out;
}

static int id_char(int c)
{
	return isalnum(c) || c == '_';
}

static int skipws(void)
{
	while (1) {
		while (cur < len && isspace(buf[cur]))
			cur++;
		if (cur == len)
			return 1;
		if (TOK2(buf + cur) != TOK2("/*"))
			return 0;
		while (++cur < len) {
			if (buf[cur] == '*' && buf[cur + 1] == '/') {
				cur += 2;
				break;
			}
		}
	}
	return 0;
}

int tok_get(void)
{
	int num;
	if (next != -1) {
		int tok = next;
		next = -1;
		return tok;
	}
	if (skipws())
		return TOK_EOF;
	if (buf[cur] == '"')
		return TOK_STR;
	if (isdigit(buf[cur]) || buf[cur] == '\'')
		return TOK_NUM;
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
	if ((num = get_tok3(TOK3(buf + cur)))) {
		cur += 3;
		return num;
	}
	if ((num = get_tok3(TOK2(buf + cur)))) {
		cur += 2;
		return num;
	}
	if (strchr(";,{}()[]<>*&!=+-/%?:|^~.", buf[cur]))
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
