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

#define MAXBUFS			(1 << 5)

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
	cpp_define("__STDC__", "");
	cpp_define("__x86_64__", "");
	cpp_define("__linux__", "");
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
	while (cur < len && (isalnum(buf[cur]) || buf[cur] == '_'))
		*dst++ = buf[cur++];
	*dst = '\0';
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

static void read_tilleol(char *dst)
{
	while (cur < len && isspace(buf[cur]) && buf[cur] != '\n')
		cur++;
	while (cur < len && buf[cur] != '\n') {
		if (buf[cur] == '\\')
			cur += 2;
		else if (buf[cur] == '/' && buf[cur + 1] == '*')
			jumpcomment();
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
	if (buf[cur] == '(') {
		cur++;
		jumpws();
		while (cur < len && buf[cur] != ')') {
			readarg(d->args[d->nargs++]);
			jumpws();
			if (buf[cur++] != ',')
				break;
			jumpws();
		}
		d->isfunc = 1;
	}
	read_tilleol(d->def);
}

int cpp_read(char *buf);

static char ebuf[BUFSIZE];
static int elen;
static int ecur;
static int cppeval;

static long evalexpr(void);

static int cpp_eval(void)
{
	int bufid;
	int ret;
	char evalbuf[BUFSIZE];
	read_tilleol(evalbuf);
	buf_new();
	strcpy(buf, evalbuf);
	len = strlen(evalbuf);
	bufid = nbufs;
	elen = 0;
	ecur = 0;
	cppeval = 1;
	while (bufid < nbufs || cur < len)
		elen += cpp_read(ebuf + elen);
	cppeval = 0;
	ret = evalexpr();
	buf_pop();
	return ret;
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
			if (!strcmp("elif", cmd))
				if (!depth && !jumpelse && cpp_eval())
					break;
			if (!strcmp("endif", cmd)) {
				if (!depth)
					break;
				else
					depth--;
			}
			if (!strcmp("ifdef", cmd) || !strcmp("ifndef", cmd) ||
					!strcmp("if", cmd))
				depth++;
			continue;
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
	if (!strcmp("ifdef", cmd) || !strcmp("ifndef", cmd) ||
						!strcmp("if", cmd)) {
		char name[NAMELEN];
		int matched = 0;
		if (cmd[2]) {
			int not = cmd[2] == 'n';
			read_word(name);
			matched = not ? macro_find(name) < 0 :
					macro_find(name) >= 0;
		} else {
			matched = cpp_eval();
		}
		if (!matched)
			jumpifs(0);
		return;
	}
	if (!strcmp("else", cmd) || !strcmp("elif", cmd)) {
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
	int nargs = 0;
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
			if (cppeval && !strcmp("defined", word)) {
				int parens = 0;
				jumpws();
				if (buf[cur] == '(') {
					parens = 1;
					cur++;
				}
				read_word(word);
				if (parens) {
					jumpws();
					cur++;
				}
			}
			continue;
		}
		cur++;
	}
	memcpy(s, buf + old, cur - old);
	s[cur - old] = '\0';
	return cur - old;
}

/* preprocessor constant expression evaluation */

static char etok[NAMELEN];
static int enext;

static char *tok2[] = {
	"<<", ">>", "&&", "||", "==", "!=", "<=", ">="
};

static int eval_tok(void)
{
	char *s = etok;
	int i;
	while (ecur < elen) {
		while (ecur < elen && isspace(ebuf[ecur]))
			ecur++;
		if (ebuf[ecur] == '/' && ebuf[ecur + 1] == '*') {
			while (ecur < elen && (ebuf[ecur - 2] != '*' ||
						ebuf[ecur - 1] != '/'))
				ecur++;
			continue;
		}
		break;
	}
	if (ecur >= elen)
		return TOK_EOF;
	if (isalpha(ebuf[ecur]) || ebuf[ecur] == '_') {
		while (isalnum(ebuf[ecur]) || ebuf[ecur] == '_')
			*s++ = ebuf[ecur++];
		*s = '\0';
		return TOK_NAME;
	}
	if (isdigit(ebuf[ecur])) {
		while (isdigit(ebuf[ecur]))
			*s++ = ebuf[ecur++];
		while (tolower(ebuf[ecur]) == 'u' || tolower(ebuf[ecur]) == 'l')
			ecur++;
		return TOK_NUM;
	}
	for (i = 0; i < ARRAY_SIZE(tok2); i++)
		if (TOK2(tok2[i]) == TOK2(ebuf + ecur)) {
			int ret = TOK2(tok2[i]);
			ecur += 2;
			return ret;
		}
	return ebuf[ecur++];
}

static int eval_see(void)
{
	if (enext == -1)
		enext = eval_tok();
	return enext;
}

static int eval_get(void)
{
	if (enext != -1) {
		int ret = enext;
		enext = -1;
		return ret;
	}
	return eval_tok();
}

static long eval_num(void)
{
	return atol(etok);
}

static int eval_jmp(int tok)
{
	if (eval_see() == tok) {
		eval_get();
		return 0;
	}
	return 1;
}

static void eval_expect(int tok)
{
	eval_jmp(tok);
}

static char *eval_id(void)
{
	return etok;
}

static long evalcexpr(void);

static long evalatom(void)
{
	if (!eval_jmp(TOK_NUM))
		return eval_num();
	if (!eval_jmp(TOK_NAME)) {
		int parens = !eval_jmp('(');
		long ret;
		eval_expect(TOK_NAME);
		ret = macro_find(eval_id()) >= 0;
		if (parens)
			eval_expect(')');
		return ret;
	}
	if (!eval_jmp('(')) {
		long ret = evalcexpr();
		eval_expect(')');
		return ret;
	}
	return -1;
}

static long evalpre(void)
{
	if (!eval_jmp('!'))
		return !evalpre();
	if (!eval_jmp('-'))
		return -evalpre();
	if (!eval_jmp('~'))
		return ~evalpre();
	return evalatom();
}

static long evalmul(void)
{
	long ret = evalpre();
	while (1) {
		if (!eval_jmp('*')) {
			ret *= evalpre();
			continue;
		}
		if (!eval_jmp('/')) {
			ret /= evalpre();
			continue;
		}
		if (!eval_jmp('%')) {
			ret %= evalpre();
			continue;
		}
		break;
	}
	return ret;
}

static long evaladd(void)
{
	long ret = evalmul();
	while (1) {
		if (!eval_jmp('+')) {
			ret += evalmul();
			continue;
		}
		if (!eval_jmp('-')) {
			ret -= evalmul();
			continue;
		}
		break;
	}
	return ret;
}

static long evalshift(void)
{
	long ret = evaladd();
	while (1) {
		if (!eval_jmp(TOK2("<<"))) {
			ret <<= evaladd();
			continue;
		}
		if (!eval_jmp(TOK2(">>"))) {
			ret >>= evaladd();
			continue;
		}
		break;
	}
	return ret;
}

static long evalcmp(void)
{
	long ret = evalshift();
	while (1) {
		if (!eval_jmp('<')) {
			ret = ret < evalshift();
			continue;
		}
		if (!eval_jmp('>')) {
			ret = ret > evalshift();
			continue;
		}
		if (!eval_jmp(TOK2("<="))) {
			ret = ret <= evalshift();
			continue;
		}
		if (!eval_jmp(TOK2(">="))) {
			ret = ret >= evalshift();
			continue;
		}
		break;
	}
	return ret;
}

static long evaleq(void)
{
	long ret = evalcmp();
	while (1) {
		if (!eval_jmp(TOK2("=="))) {
			ret = ret == evalcmp();
			continue;
		}
		if (!eval_jmp(TOK2("!="))) {
			ret = ret != evalcmp();
			continue;
		}
		break;
	}
	return ret;
}

static long evalbitand(void)
{
	long ret = evaleq();
	while (!eval_jmp('&'))
		ret &= evaleq();
	return ret;
}

static long evalxor(void)
{
	long ret = evalbitand();
	while (!eval_jmp('^'))
		ret ^= evalbitand();
	return ret;
}

static long evalbitor(void)
{
	long ret = evalxor();
	while (!eval_jmp('|'))
		ret |= evalxor();
	return ret;
}

static long evaland(void)
{
	long ret = evalbitor();
	while (!eval_jmp(TOK2("&&")))
		ret = ret && evalbitor();
	return ret;
}

static long evalor(void)
{
	long ret = evaland();
	while (!eval_jmp(TOK2("||")))
		ret = ret || evaland();
	return ret;
}

static long evalcexpr(void)
{
	long ret = evalor();
	if (eval_jmp('?'))
		return ret;
	if (ret)
		return evalor();
	while (eval_get() != ':')
		;
	return evalor();
}

static long evalexpr(void)
{
	enext = -1;
	return evalcexpr();
}
