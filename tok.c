#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "gen.h"
#include "mem.h"
#include "ncc.h"
#include "tok.h"

static struct mem tok_mem;	/* the data read via cpp_read() so far */
static struct mem str;		/* the last tok_str() string */
static char *buf;
static int len;
static int cur;
static char name[NAMELEN];
static int next = -1;
static int pre;

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
	"<<=", ">>=", "...", "<<", ">>", "++", "--", "+=", "-=", "*=", "/=",
	"%=", "|=", "&=", "^=", "&&", "||", "==", "!=", "<=", ">=", "->"
};

static int get_tok3(int num)
{
	int i;
	for (i = 0; i < LEN(tok3); i++)
		if (num == TOK3(tok3[i]))
			return num;
	return 0;
}

static char *esc_code = "abefnrtv";
static char *esc = "\a\b\e\f\n\r\t\v";
static char *digs = "0123456789abcdef";

static int esc_char(int *c, char *s)
{
	if (*s != '\\') {
		*c = (unsigned char) *s;
		return 1;
	}
	if (strchr(esc_code, s[1])) {
		*c = esc[strchr(esc_code, s[1]) - esc_code];
		return 2;
	}
	if (isdigit(s[1]) || s[1] == 'x') {
		int ret = 0;
		int base = 8;
		int i = 1;
		char *d;
		if (s[1] == 'x') {
			base = 16;
			i++;
		}
		while ((d = memchr(digs, s[i], base))) {
			ret *= base;
			ret += d - digs;
			i++;
		}
		*c = ret;
		return i;
	}
	*c = (unsigned char) s[1];
	return 2;
}

static long num;
static int num_bt;

int tok_num(long *n)
{
	*n = num;
	return num_bt;
}

static void readnum(void)
{
	int base = 10;
	num_bt = 4 | BT_SIGNED;
	if (buf[cur] == '0' && buf[cur + 1] == 'x') {
		num_bt &= ~BT_SIGNED;
		base = 16;
		cur += 2;
	}
	if (strchr(digs, tolower(buf[cur]))) {
		long result = 0;
		char *c;
		if (base == 10 && buf[cur] == '0')
			base = 8;
		while (cur < len && (c = strchr(digs, tolower(buf[cur])))) {
			result *= base;
			result += c - digs;
			cur++;
		}
		num = result;
		while (cur < len) {
			int c = tolower(buf[cur]);
			if (c != 'u' && c != 'l')
				break;
			if (c == 'u')
				num_bt &= ~BT_SIGNED;
			if (c == 'l')
				num_bt = (num_bt & BT_SIGNED) | LONGSZ;
			cur++;
		}
		return;
	}
	if (buf[cur] == '\'') {
		int ret;
		cur += 2 + esc_char(&ret, buf + cur + 1);
		num = ret;
		return;
	}
	num = -1;
}

void tok_str(char **buf, int *len)
{
	if (len)
		*len = mem_len(&str) + 1;
	if (buf)
		*buf = mem_buf(&str);
}

static void readstr(struct mem *mem)
{
	char *s = buf + cur;
	char *e = buf + len;
	int c;
	s++;
	while (s < e && *s != '"') {
		if (*s == '\\') {
			s += esc_char(&c, s);
			mem_putc(mem, c);
		} else {
			mem_putc(mem, (unsigned char) *s++);
		}
	}
	cur = s - buf + 1;
}

static int id_char(int c)
{
	return isalnum(c) || c == '_';
}

static int skipws(void)
{
	int clen;
	char *cbuf;
	while (1) {
		if (cur == len) {
			clen = 0;
			while (!clen)
				if (cpp_read(&cbuf, &clen))
					return 1;
			mem_put(&tok_mem, cbuf, clen);
			buf = mem_buf(&tok_mem);
			len = mem_len(&tok_mem);
		}
		while (cur < len && isspace(buf[cur]))
			cur++;
		if (cur == len)
			continue;
		if (buf[cur] == '\\' && buf[cur + 1] == '\n') {
			cur += 2;
			continue;
		}
		if (buf[cur] == '/' && buf[cur + 1] == '/') {
			while (cur < len && buf[cur] != '\n')
				cur++;
			continue;
		}
		if (buf[cur] == '/' && buf[cur + 1] == '*') {
			while (++cur < len) {
				if (buf[cur] == '*' && buf[cur + 1] == '/') {
					cur += 2;
					break;
				}
			}
			continue;
		}
		break;
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
	pre = cur;
	if (skipws())
		return TOK_EOF;
	if (buf[cur] == '"') {
		mem_cut(&str, 0);
		while (buf[cur] == '"') {
			readstr(&str);
			if (skipws())
				return TOK_EOF;
		}
		return TOK_STR;
	}
	if (isdigit(buf[cur]) || buf[cur] == '\'') {
		readnum();
		return TOK_NUM;
	}
	if (id_char(buf[cur])) {
		char *s = name;
		int i;
		while (cur < len && id_char(buf[cur]))
			*s++ = buf[cur++];
		*s = '\0';
		for (i = 0; i < LEN(kwds); i++)
			if (!strcmp(kwds[i].name, name))
				return kwds[i].id;
		return TOK_NAME;
	}
	if (cur + 3 <= len && (num = get_tok3(TOK3(buf + cur)))) {
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

char *tok_id(void)
{
	return name;
}

long tok_addr(void)
{
	return next == -1 ? cur : pre;
}

void tok_jump(long addr)
{
	cur = addr;
	pre = cur - 1;
	next = -1;
}
