/* neatcc tokenizer */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ncc.h"

static struct mem tok_mem;	/* the data read via cpp_read() so far */
static struct mem tok;		/* the previous token */
static char *buf;
static long off, off_pre;	/* current and previous positions */
static long len;
static int tok_set;		/* the current token was read */

static char *tok3[] = {
	"<<=", ">>=", "...", "<<", ">>", "++", "--", "+=", "-=", "*=", "/=",
	"%=", "|=", "&=", "^=", "&&", "||", "==", "!=", "<=", ">=", "->"
};

static char *find_tok3(char *r)
{
	int i;
	for (i = 0; i < LEN(tok3); i++) {
		char *s = tok3[i];
		if (s[0] == r[0] && s[1] == r[1] && (!s[2] || s[2] == r[2]))
			return s;
	}
	return NULL;
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
	if (strchr(esc_code, (unsigned char) s[1])) {
		*c = esc[strchr(esc_code, (unsigned char) s[1]) - esc_code];
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
		while ((d = memchr(digs, tolower(s[i]), base))) {
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

long tok_num(char *tok, long *num)
{
	int base = 10;
	long num_bt = 4 | T_MSIGN;
	if (tok[0] == '0' && tolower(tok[1]) == 'x') {
		num_bt &= ~T_MSIGN;
		base = 16;
		tok += 2;
	}
	if (strchr(digs, tolower((unsigned char) tok[0]))) {
		long result = 0;
		char *c;
		if (base == 10 && tok[0] == '0')
			base = 8;
		while (tok[0] && (c = strchr(digs, tolower((unsigned char) tok[0])))) {
			result *= base;
			result += c - digs;
			tok++;
		}
		*num = result;
		while (tok[0]) {
			int c = tolower((unsigned char) tok[0]);
			if (c != 'u' && c != 'l')
				break;
			if (c == 'u')
				num_bt &= ~T_MSIGN;
			if (c == 'l')
				num_bt = (num_bt & T_MSIGN) | LONGSZ;
			tok++;
		}
		return num_bt;
	}
	if (tok[0] == '\'') {
		int ret;
		esc_char(&ret, tok + 1);
		*num = ret;
		return num_bt;
	}
	return 0;
}

static int id_char(int c)
{
	return isalnum(c) || c == '_';
}

static int skipws(void)
{
	long clen;
	char *cbuf;
	while (1) {
		if (off == len) {
			clen = 0;
			while (!clen)
				if (cpp_read(&cbuf, &clen))
					return 1;
			mem_put(&tok_mem, cbuf, clen);
			buf = mem_buf(&tok_mem);
			len = mem_len(&tok_mem);
		}
		while (off < len && isspace(buf[off]))
			off++;
		if (off == len)
			continue;
		if (buf[off] == '\\' && buf[off + 1] == '\n') {
			off += 2;
			continue;
		}
		if (buf[off] == '/' && buf[off + 1] == '/') {
			while (++off < len && buf[off] != '\n')
				if (buf[off] == '\\')
					off++;
			continue;
		}
		if (buf[off] == '/' && buf[off + 1] == '*') {
			while (++off < len) {
				if (buf[off] == '*' && buf[off + 1] == '/') {
					off += 2;
					break;
				}
			}
			continue;
		}
		break;
	}
	return 0;
}

static int tok_read(void)
{
	char *t3;
	int c;
	off_pre = off;
	mem_cut(&tok, 0);
	if (skipws())
		return 1;
	if (buf[off] == '"') {
		mem_putc(&tok, '"');
		while (buf[off] == '"') {
			off++;
			while (off < len && buf[off] != '"') {
				if (buf[off] == '\\') {
					off += esc_char(&c, buf + off);
					mem_putc(&tok, c);
				} else {
					mem_putc(&tok, (unsigned char) buf[off++]);
				}
			}
			if (off >= len || buf[off++] != '"')
				return 1;
			if (skipws())
				return 1;
		}
		mem_putc(&tok, '"');
		return 0;
	}
	if (isdigit((unsigned char) buf[off])) {
		if (buf[off] == '0' && (buf[off + 1] == 'x' || buf[off + 1] == 'X')) {
			mem_putc(&tok, (unsigned char) buf[off++]);
			mem_putc(&tok, (unsigned char) buf[off++]);
		}
		while (off < len && strchr(digs, tolower((unsigned char) buf[off])))
			mem_putc(&tok, (unsigned char) buf[off++]);
		while (off < len && strchr("uUlL", (unsigned char) buf[off]))
			mem_putc(&tok, (unsigned char) buf[off++]);
		return 0;
	}
	if (buf[off] == '\'') {
		int c, i;
		int n = esc_char(&c, buf + off + 1) + 1 + 1;
		for (i = 0; i < n; i++)
			mem_putc(&tok, (unsigned char) buf[off++]);
		return 0;
	}
	if (id_char((unsigned char) buf[off])) {
		while (off < len && id_char((unsigned char) buf[off]))
			mem_putc(&tok, (unsigned char) buf[off++]);
		return 0;
	}
	if (off + 2 <= len && (t3 = find_tok3(buf + off))) {
		off += strlen(t3);
		mem_put(&tok, t3, strlen(t3));
		return 0;
	}
	if (strchr(";,{}()[]<>*&!=+-/%?:|^~.", (unsigned char) buf[off])) {
		mem_putc(&tok, (unsigned char) buf[off++]);
		return 0;
	}
	return 1;
}

char *tok_get(void)
{
	if (!tok_set)
		if (tok_read())
			return "";
	tok_set = 0;
	return mem_buf(&tok);
}

char *tok_see(void)
{
	if (!tok_set)
		if (tok_read())
			return "";
	tok_set = 1;
	return mem_buf(&tok);
}

long tok_len(void)
{
	return mem_len(&tok);
}

long tok_addr(void)
{
	return tok_set ? off_pre : off;
}

void tok_jump(long addr)
{
	off = addr;
	off_pre = -1;
	tok_set = 0;
}

void tok_done(void)
{
	mem_done(&tok);
	mem_done(&tok_mem);
}
