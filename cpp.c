/* neatcc preprocessor */
#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "mem.h"
#include "ncc.h"
#include "tok.h"

static char *buf;
static int len;
static int cur;

static struct macro {
	char name[NAMELEN];	/* macro name */
	char def[MDEFLEN];	/* macro definition */
	char args[NARGS][NAMELEN];
	int nargs;		/* number of arguments */
	int isfunc;		/* macro is a function */
	int undef;		/* macro is removed */
} macros[NDEFS];
static int mcount = 1;		/* number of macros */
static int mhead[256];		/* macro hash table heads */
static int mnext[NDEFS];	/* macro hash table next entries */

#define BUF_FILE		0
#define BUF_MACRO		1
#define BUF_ARG			2
#define BUF_EVAL		3
#define BUF_TEMP		4

/* preprocessing input buffers for files, macros and macro arguments */
static struct buf {
	char *buf;
	int len;
	int cur;
	int type;
	/* for BUF_FILE */
	char path[NAMELEN];
	/* for BUF_MACRO */
	struct macro *macro;
	char args[NARGS][MARGLEN];	/* arguments passed to a macro */
	/* for BUF_ARG */
	int arg_buf;			/* the bufs index of the owning macro */
} bufs[NBUFS];
static int nbufs;
static int bufs_limit = 1;		/* cpp_read() limit; useful in cpp_eval() */

void die(char *fmt, ...)
{
	va_list ap;
	char msg[512];
	va_start(ap, fmt);
	vsprintf(msg, fmt, ap);
	va_end(ap);
	write(2, msg, strlen(msg));
	exit(1);
}

static void buf_new(int type, char *dat, int dlen)
{
	if (nbufs) {
		bufs[nbufs - 1].buf = buf;
		bufs[nbufs - 1].cur = cur;
		bufs[nbufs - 1].len = len;
	}
	if (nbufs >= NBUFS)
		die("nomem: NBUFS reached!\n");
	nbufs++;
	cur = 0;
	buf = dat;
	len = dlen;
	bufs[nbufs - 1].type = type;
}

static void buf_file(char *path, char *dat, int dlen)
{
	buf_new(BUF_FILE, dat, dlen);
	strcpy(bufs[nbufs - 1].path, path ? path : "");
}

static void buf_macro(struct macro *m)
{
	buf_new(BUF_MACRO, m->def, strlen(m->def));
	bufs[nbufs - 1].macro = m;
}

static void buf_arg(char *arg, int mbuf)
{
	buf_new(BUF_ARG, arg, strlen(arg));
	bufs[nbufs - 1].arg_buf = mbuf;
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

static int buf_iseval(void)
{
	int i;
	for (i = nbufs - 1; i >= 0; i--)
		if (bufs[i].type == BUF_EVAL)
			return 1;
	return 0;
}

static size_t file_size(int fd)
{
	struct stat st;
	if (!fstat(fd, &st))
		return st.st_size;
	return 0;
}

static int include_file(char *path)
{
	int fd = open(path, O_RDONLY);
	int n = 0, nr = 0;
	char *dat;
	int size;
	if (fd == -1)
		return -1;
	size = file_size(fd) + 1;
	dat = malloc(size);
	while ((n = read(fd, dat + nr, size - nr)) > 0)
		nr += n;
	close(fd);
	dat[nr] = '\0';
	buf_file(path, dat, nr);
	return 0;
}

int cpp_init(char *path)
{
	return include_file(path);
}

static int jumpws(void)
{
	int old = cur;
	while (cur < len && isspace(buf[cur]))
		cur++;
	return cur == old;
}

static void read_word(char *dst)
{
	jumpws();
	while (cur < len && (isalnum(buf[cur]) || buf[cur] == '_'))
		*dst++ = buf[cur++];
	*dst = '\0';
}

static int jumpcomment(void)
{
	if (buf[cur] == '/' && buf[cur + 1] == '*') {
		while (++cur < len) {
			if (buf[cur] == '*' && buf[cur + 1] == '/') {
				cur += 2;
				return 0;
			}
		}
	}
	if (buf[cur] == '/' && buf[cur + 1] == '/') {
		while (++cur < len)
			if (buf[cur] == '\n')
				break;
		return 0;
	}
	return 1;
}

static int jumpstr(void)
{
	if (buf[cur] == '\'') {
		while (cur < len && buf[++cur] != '\'')
			if (buf[cur] == '\\')
				cur++;
		cur++;
		return 0;
	}
	if (buf[cur] == '"') {
		while (cur < len && buf[++cur] != '"')
			if (buf[cur] == '\\')
				cur++;
		cur++;
		return 0;
	}
	return 1;
}

static void read_tilleol(char *dst)
{
	while (cur < len && isspace(buf[cur]) && buf[cur] != '\n')
		cur++;
	while (cur < len && buf[cur] != '\n') {
		int last = cur;
		if (buf[cur] == '\\' && buf[cur + 1] == '\n') {
			cur += 2;
			continue;
		}
		if (!jumpstr()) {
			memcpy(dst, buf + last, cur - last);
			dst += cur - last;
			continue;
		}
		if (!jumpcomment())
			continue;
		*dst++ = buf[cur++];
	}
	*dst = '\0';
}

static char *locs[NLOCS] = {};
static int nlocs = 0;

void cpp_addpath(char *s)
{
	locs[nlocs++] = s;
}

static int include_find(char *name, int std)
{
	int i;
	for (i = std ? nlocs - 1 : nlocs; i >= 0; i--) {
		char path[1 << 10];
		if (locs[i])
			sprintf(path, "%s/%s", locs[i], name);
		else
			strcpy(path, name);
		if (!include_file(path))
			return 0;
	}
	return -1;
}

static void readarg(char *s)
{
	int depth = 0;
	int beg = cur;
	while (cur < len && (depth || (buf[cur] != ',' && buf[cur] != ')'))) {
		if (!jumpstr() || !jumpcomment())
			continue;
		switch (buf[cur++]) {
		case '(':
		case '[':
		case '{':
			depth++;
			break;
		case ')':
		case ']':
		case '}':
			depth--;
			break;
		}
	}
	if (s) {
		memcpy(s, buf + beg, cur - beg);
		s[cur - beg] = '\0';
	}
}

/* find a macro; if undef is nonzero, search #undef-ed macros too */
static int macro_find(char *name, int undef)
{
	int i = mhead[(unsigned char) name[0]];
	while (i > 0) {
		if (!strcmp(name, macros[i].name))
			if (!macros[i].undef || undef)
				return i;
		i = mnext[i];
	}
	return -1;
}

static void macro_undef(char *name)
{
	int i = macro_find(name, 0);
	if (i >= 0)
		macros[i].undef = 1;
}

static int macro_new(char *name)
{
	int i = macro_find(name, 1);
	if (i >= 0)
		return i;
	if (mcount >= NDEFS)
		die("nomem: NDEFS reached!\n");
	i = mcount++;
	strcpy(macros[i].name, name);
	mnext[i] = mhead[(unsigned char) name[0]];
	mhead[(unsigned char) name[0]] = i;
	return i;
}

static void macro_define(void)
{
	char name[NAMELEN];
	struct macro *d;
	read_word(name);
	d = &macros[macro_new(name)];
	d->isfunc = 0;
	d->nargs = 0;
	if (buf[cur] == '(') {
		cur++;
		jumpws();
		while (cur < len && buf[cur] != ')') {
			readarg(d->args[d->nargs++]);
			jumpws();
			if (buf[cur] != ',')
				break;
			cur++;
			jumpws();
		}
		cur++;
		d->isfunc = 1;
	}
	read_tilleol(d->def);
}

static char ebuf[MARGLEN];
static int elen;
static int ecur;

static long evalexpr(void);

static int cpp_eval(void)
{
	char evalbuf[MARGLEN];
	int old_limit;
	int ret, clen;
	char *cbuf;
	read_tilleol(evalbuf);
	buf_new(BUF_EVAL, evalbuf, strlen(evalbuf));
	elen = 0;
	ecur = 0;
	old_limit = bufs_limit;
	bufs_limit = nbufs;
	while (!cpp_read(&cbuf, &clen)) {
		memcpy(ebuf + elen, cbuf, clen);
		elen += clen;
	}
	bufs_limit = old_limit;
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
		if (!jumpcomment())
			continue;
		if (!jumpstr())
			continue;
		cur++;
	}
}

static int cpp_cmd(void)
{
	char cmd[NAMELEN];
	cur++;
	read_word(cmd);
	if (!strcmp("define", cmd)) {
		macro_define();
		return 0;
	}
	if (!strcmp("undef", cmd)) {
		char name[NAMELEN];
		read_word(name);
		macro_undef(name);
		return 0;
	}
	if (!strcmp("ifdef", cmd) || !strcmp("ifndef", cmd) ||
						!strcmp("if", cmd)) {
		char name[NAMELEN];
		int matched = 0;
		if (cmd[2]) {
			int not = cmd[2] == 'n';
			read_word(name);
			matched = not ? macro_find(name, 0) < 0 :
					macro_find(name, 0) >= 0;
		} else {
			matched = cpp_eval();
		}
		if (!matched)
			jumpifs(0);
		return 0;
	}
	if (!strcmp("else", cmd) || !strcmp("elif", cmd)) {
		jumpifs(1);
		return 0;
	}
	if (!strcmp("endif", cmd))
		return 0;
	if (!strcmp("include", cmd)) {
		char file[NAMELEN];
		char *s, *e;
		jumpws();
		s = buf + cur + 1;
		e = strchr(buf + cur + 1, buf[cur] == '"' ? '"' : '>');
		memcpy(file, s, e - s);
		file[e - s] = '\0';
		cur += e - s + 2;
		if (include_find(file, *e == '>') == -1)
			err("cannot include <%s>\n", file);
		return 0;
	}
	return 1;
}

static int macro_arg(struct macro *m, char *arg)
{
	int i;
	for (i = 0; i < m->nargs; i++)
		if (!strcmp(arg, m->args[i]))
			return i;
	return -1;
}

static int buf_arg_find(char *name)
{
	int i;
	for (i = nbufs - 1; i >= 0; i--) {
		struct buf *mbuf = &bufs[i];
		struct macro *m = mbuf->macro;
		if (mbuf->type == BUF_MACRO && macro_arg(m, name) >= 0)
			return i;
		if (mbuf->type == BUF_ARG)
			i = mbuf->arg_buf;
	}
	return -1;
}

static void macro_expand(char *name)
{
	struct macro *m;
	int mbuf;
	if ((mbuf = buf_arg_find(name)) >= 0) {
		int arg = macro_arg(bufs[mbuf].macro, name);
		char *dat = bufs[mbuf].args[arg];
		buf_arg(dat, mbuf);
		return;
	}
	m = &macros[macro_find(name, 0)];
	if (!m->isfunc) {
		buf_macro(m);
		return;
	}
	jumpws();
	if (buf[cur] == '(') {
		int i = 0;
		struct buf *mbuf = &bufs[nbufs];
		cur++;
		jumpws();
		while (cur < len && buf[cur] != ')') {
			readarg(mbuf->args[i++]);
			jumpws();
			if (buf[cur] != ',')
				break;
			cur++;
			jumpws();
		}
		while (i < m->nargs)
			mbuf->args[i++][0] = '\0';
		cur++;
		buf_macro(m);
	}
}

static int buf_expanding(char *macro)
{
	int i;
	for (i = nbufs - 1; i >= 0; i--) {
		if (bufs[i].type == BUF_ARG)
			return 0;
		if (bufs[i].type == BUF_MACRO &&
				!strcmp(macro, bufs[i].macro->name))
			return 1;
	}
	return 0;
}

/* return 1 for plain macros and arguments and 2 for function macros */
static int expandable(char *word)
{
	int i;
	if (buf_arg_find(word) >= 0)
		return 1;
	if (buf_expanding(word))
		return 0;
	i = macro_find(word, 0);
	return i >= 0 ? macros[i].isfunc + 1 : 0;
}

void cpp_define(char *name, char *def)
{
	char tmp_buf[MDEFLEN];
	sprintf(tmp_buf, "%s\t%s", name, def);
	buf_new(BUF_TEMP, tmp_buf, strlen(tmp_buf));
	macro_define();
	buf_pop();
}

static int seen_macro;		/* seen a macro; 2 if a function macro */
static char seen_name[NAMELEN];	/* the name of the last macro */

static int hunk_off;
static int hunk_len;

int cpp_read(char **obuf, int *olen)
{
	int old, end;
	int jump_name = 0;
	*olen = 0;
	*obuf = "";
	if (seen_macro == 1) {
		macro_expand(seen_name);
		seen_macro = 0;
	}
	if (cur == len) {
		struct buf *cbuf = &bufs[nbufs - 1];
		if (nbufs < bufs_limit + 1)
			return -1;
		if (cbuf->type == BUF_FILE)
			free(buf);
		buf_pop();
	}
	old = cur;
	if (buf[cur] == '#')
		if (!cpp_cmd())
			return 0;
	while (cur < len) {
		if (!jumpws())
			continue;
		if (buf[cur] == '#')
			break;
		if (!jumpcomment())
			continue;
		if (seen_macro == 2) {
			if (buf[cur] == '(')
				macro_expand(seen_name);
			seen_macro = 0;
			old = cur;
			continue;
		}
		if (!jumpstr())
			continue;
		if (isalnum(buf[cur]) || buf[cur] == '_') {
			char word[NAMELEN];
			read_word(word);
			seen_macro = expandable(word);
			if (seen_macro) {
				strcpy(seen_name, word);
				jump_name = 1;
				break;
			}
			if (buf_iseval() && !strcmp("defined", word)) {
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
	/* macros are expanded later; ignoring their names */
	end = jump_name ? cur - strlen(seen_name) : cur;
	if (!buf_iseval()) {
		hunk_off += hunk_len;
		hunk_len = end - old;
	}
	*obuf = buf + old;
	*olen = end - old;
	return 0;
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
		*s = '\0';
		return TOK_NUM;
	}
	for (i = 0; i < LEN(tok2); i++)
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
		ret = macro_find(eval_id(), 0) >= 0;
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

static int buf_loc(char *s, int off)
{
	char *e = s + off;
	int n = 1;
	while ((s = strchr(s, '\n')) && s < e) {
		n++;
		s++;
	}
	return n;
}

char *cpp_loc(long addr)
{
	static char loc[256];
	int line = -1;
	int i;
	for (i = nbufs - 1; i > 0; i--)
		if (bufs[i].type == BUF_FILE)
			break;
	if (addr >= hunk_off && i == nbufs - 1)
		line = buf_loc(buf, (cur - hunk_len) + (addr - hunk_off));
	else
		line = buf_loc(bufs[i].buf, bufs[i].cur);
	sprintf(loc, "%s:%d", bufs[i].path, line);
	return loc;
}
