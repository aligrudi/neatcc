#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "gen.h"
#include "tok.h"

#define MAXLOCALS	(1 << 10)
#define MAXGLOBALS	(1 << 10)
#define MAXARGS		(1 << 5)
#define print(s)	write(2, (s), strlen(s));

#define TYPE_BT(t)		((t)->ptr ? 8 : (t)->bt)
#define TYPE_SZ(t)		((t)->ptr ? 8 : (t)->bt & BT_SZMASK)
#define TYPE_DEREF_BT(t)	((t)->ptr > 1 ? 8 : (t)->bt)
#define TYPE_DEREF_SZ(t)	((t)->ptr > 1 ? 8 : (t)->bt & BT_SZMASK)

#define T_ARRAY		0x01

struct type {
	unsigned bt;
	unsigned flags;
	int ptr;
	int n;
};

/* type stack */
static struct type ts[MAXTMP];
static int nts;

static void ts_push_bt(unsigned bt)
{
	ts[nts].ptr = 0;
	ts[nts].flags = 0;
	ts[nts++].bt = bt;
}

static void ts_push(struct type *type)
{
	ts[nts++] = *type;
}

static void ts_pop(struct type *type)
{
	nts--;
	if (type)
		*type = ts[nts];
}

static int type_totsz(struct type *t)
{
	if (!t->ptr || t->flags & T_ARRAY && (t)->ptr == 1)
		return BT_SZ(t->bt) * t->n;
	return 8;
}

struct name {
	char name[NAMELEN];
	struct type type;
	long addr;
};

static struct name locals[MAXLOCALS];
static int nlocals;
static struct name globals[MAXGLOBALS];
static int nglobals;

static void local_add(struct name *name)
{
	memcpy(&locals[nlocals++], name, sizeof(*name));
}

static void global_add(struct name *name)
{
	memcpy(&globals[nglobals++], name, sizeof(*name));
}

static void die(char *s)
{
	print(s);
	exit(1);
}

static int tok_jmp(int tok)
{
	if (tok_see() != tok)
		return 1;
	tok_get();
	return 0;
}

static void tok_expect(int tok)
{
	if (tok_get() != tok)
		die("syntax error\n");
}

static void readexpr(void);

static int basetype(struct type *type)
{
	int sign = 1;
	int size = 4;
	int done = 0;
	int i = 0;
	while (!done) {
		switch (tok_see()) {
		case TOK_VOID:
			sign = 0;
			size = 0;
			done = 1;
			break;
		case TOK_INT:
			done = 1;
			break;
		case TOK_CHAR:
			size = 1;
			done = 1;
			break;
		case TOK_SHORT:
			size = 2;
			break;
		case TOK_LONG:
			size = 8;
			break;
		case TOK_UNSIGNED:
			sign = 0;
			break;
		case TOK_ENUM:
		case TOK_STRUCT:
			tok_expect(TOK_NAME);
			done = 1;
			break;
		default:
			if (!i)
				return 1;
			done = 1;
			continue;
		}
		i++;
		tok_get();
	}
	type->bt = size | (sign ? BT_SIGNED : 0);
	type->ptr = 0;
	type->n = 1;
	return 0;
}

static void readptrs(struct type *type)
{
	while (!tok_jmp('*'))
		type->ptr++;
}

static int readtype(struct type *type)
{
	if (basetype(type))
		return 1;
	readptrs(type);
	return 0;
}

static void readprimary(void)
{
	struct name name;
	int i;
	if (!tok_jmp(TOK_NUM)) {
		ts_push_bt(4 | BT_SIGNED);
		o_num(tok_num(), 4 | BT_SIGNED);
		return;
	}
	if (!tok_jmp(TOK_NAME)) {
		for (i = 0; i < nlocals; i++) {
			struct type *t = &locals[i].type;
			if (!strcmp(locals[i].name, tok_id())) {
				ts_push(t);
				o_local(locals[i].addr, TYPE_BT(t));
				if (t->flags & T_ARRAY)
					o_addr();
				return;
			}
		}
		for (i = 0; i < nglobals; i++) {
			struct type *t = &globals[i].type;
			if (!strcmp(globals[i].name, tok_id())) {
				ts_push(t);
				o_symaddr(globals[i].addr, TYPE_BT(t));
				if (t->flags & T_ARRAY)
					o_addr();
				return;
			}
		}
		strcpy(name.name, tok_id());
		name.addr = o_mkundef(name.name);
		global_add(&name);
		ts_push_bt(8);
		o_symaddr(name.addr, 8);
		return;
	}
	if (!tok_jmp('(')) {
		readexpr();
		tok_expect(')');
		return;
	}
}

void arrayderef(unsigned bt)
{
	if (BT_SZ(bt) > 1) {
		o_num(BT_SZ(bt), 4);
		o_mul();
	}
	o_add();
	o_deref(bt);
}

static void inc_post(void (*op)(void))
{
	struct type *t = &ts[nts - 1];
	o_tmpcopy();
	o_load();
	o_tmpswap();
	o_tmpcopy();
	o_num(1, 4);
	op();
	o_assign(t->bt);
	o_tmpdrop(1);
	return;
}

static void readpost(void)
{
	readprimary();
	if (!tok_jmp('[')) {
		struct type t1;
		ts_pop(&t1);
		readexpr();
		ts_pop(NULL);
		tok_expect(']');
		arrayderef(TYPE_DEREF_BT(&t1));
		t1.ptr--;
		ts_push(&t1);
		return;
	}
	if (!tok_jmp('(')) {
		int argc = 0;
		unsigned bt[MAXARGS];
		if (tok_see() != ')') {
			readexpr();
			bt[argc++] = 4 | BT_SIGNED;
			ts_pop(NULL);
		}
		while (!tok_jmp(',')) {
			readexpr();
			bt[argc++] = 4 | BT_SIGNED;
			ts_pop(NULL);
		}
		tok_expect(')');
		ts_pop(NULL);
		o_call(argc, bt, 4 | BT_SIGNED);
		ts_push_bt(4 | BT_SIGNED);
		return;
	}
	if (!tok_jmp(TOK2("++"))) {
		inc_post(o_add);
		return;
	}
	if (!tok_jmp(TOK2("--"))) {
		inc_post(o_sub);
		return;
	}
}

static void inc_pre(void (*op)(void))
{
	struct type *t = &ts[nts - 1];
	readpost();
	o_tmpcopy();
	o_num(1, 4);
	op();
	o_assign(t->bt);
}

static void readpre(void)
{
	if (!tok_jmp('&')) {
		struct type type;
		readpost();
		ts_pop(&type);
		type.ptr++;
		ts_push(&type);
		o_addr();
		return;
	}
	if (!tok_jmp('*')) {
		struct type type;
		readpost();
		ts_pop(&type);
		type.ptr--;
		ts_push(&type);
		o_deref(TYPE_BT(&type));
		return;
	}
	if (!tok_jmp('!')) {
		struct type type;
		readpost();
		ts_pop(&type);
		o_lnot();
		ts_push_bt(4 | BT_SIGNED);
		return;
	}
	if (!tok_jmp('-')) {
		readpost();
		o_neg();
		return;
	}
	if (!tok_jmp('~')) {
		readpost();
		o_not();
		return;
	}
	if (!tok_jmp(TOK2("++"))) {
		inc_pre(o_add);
		return;
	}
	if (!tok_jmp(TOK2("--"))) {
		inc_pre(o_sub);
		return;
	}
	readpost();
}

static int shifts(int n)
{
	int i = -1;
	while (i++ < 16)
		if (n == 1 << i)
			break;
	return i;
}

static unsigned bt_op(unsigned bt1, unsigned bt2)
{
	unsigned s1 = BT_SZ(bt1);
	unsigned s2 = BT_SZ(bt2);
	return (bt1 | bt2) & BT_SIGNED | (s1 > s2 ? s1 : s2);
}

static void ts_binop(void (*o_sth)(void))
{
	struct type t1, t2;
	ts_pop(&t1);
	ts_pop(&t2);
	o_sth();
	ts_push_bt(bt_op(TYPE_BT(&t1), TYPE_BT(&t2)));
}

static void ts_binop_add(void (*o_sth)(void))
{
	struct type t1, t2;
	ts_pop(&t1);
	ts_pop(&t2);
	if (!t1.ptr && !t2.ptr) {
		o_sth();
		ts_push_bt(bt_op(TYPE_BT(&t1), TYPE_BT(&t2)));
		return;
	}
	if (t1.ptr && !t2.ptr) {
		struct type t = t2;
		t2 = t1;
		t1 = t;
		o_tmpswap();
	}
	if (!t1.ptr && t2.ptr)
		if (TYPE_DEREF_SZ(&t2) > 1) {
			o_num(shifts(TYPE_DEREF_SZ(&t2)), 1);
			o_shl();
		}
	o_sth();
	if (t1.ptr && t2.ptr) {
		o_num(shifts(TYPE_DEREF_SZ(&t1)), 1);
		o_shr();
		ts_push_bt(4 | BT_SIGNED);
	} else {
		ts_push(&t2);
	}
}

static void readmul(void)
{
	readpre();
	while (1) {
		if (!tok_jmp('*')) {
			readpre();
			ts_binop(o_mul);
			continue;
		}
		if (!tok_jmp('/')) {
			readpre();
			ts_binop(o_div);
			continue;
		}
		if (!tok_jmp('%')) {
			readpre();
			ts_binop(o_mod);
			continue;
		}
		break;
	}
}

static void readadd(void)
{
	readmul();
	while (1) {
		if (!tok_jmp('+')) {
			readmul();
			ts_binop_add(o_add);
			continue;
		}
		if (!tok_jmp('-')) {
			readmul();
			ts_binop_add(o_sub);
			continue;
		}
		break;
	}
}

static void shift(void (*op)(void))
{
	struct type t;
	readadd();
	ts_pop(NULL);
	ts_pop(&t);
	op();
	ts_push_bt(TYPE_BT(&t));
}

static void readshift(void)
{
	readadd();
	if (!tok_jmp(TOK2("<<"))) {
		shift(o_shl);
		return;
	}
	if (!tok_jmp(TOK2(">>"))) {
		shift(o_shr);
		return;
	}
}

static void cmp(void (*op)(void))
{
	readshift();
	ts_pop(NULL);
	ts_pop(NULL);
	op();
	ts_push_bt(4 | BT_SIGNED);
}

static void readcmp(void)
{
	readshift();
	if (!tok_jmp('<')) {
		cmp(o_lt);
		return;
	}
	if (!tok_jmp('>')) {
		cmp(o_gt);
		return;
	}
	if (!tok_jmp(TOK2("<="))) {
		cmp(o_le);
		return;
	}
	if (!tok_jmp(TOK2(">="))) {
		cmp(o_ge);
		return;
	}
}

static void eq(void (*op)(void))
{
	readcmp();
	ts_pop(NULL);
	ts_pop(NULL);
	op();
	ts_push_bt(4 | BT_SIGNED);
}

static void readeq(void)
{
	readcmp();
	if (!tok_jmp(TOK2("=="))) {
		eq(o_eq);
		return;
	}
	if (!tok_jmp(TOK2("!="))) {
		eq(o_neq);
		return;
	}
}

static void readbitand(void)
{
	readeq();
	while (!tok_jmp('&')) {
		readeq();
		ts_binop(o_and);
	}
}

static void readxor(void)
{
	readbitand();
	while (!tok_jmp('^')) {
		readbitand();
		ts_binop(o_xor);
	}
}

static void readbitor(void)
{
	readxor();
	while (!tok_jmp('|')) {
		readxor();
		ts_binop(o_or);
	}
}

#define MAXCOND			(1 << 5)

static void readand(void)
{
	long conds[MAXCOND];
	int nconds = 0;
	long passed;
	int i;
	readbitor();
	if (tok_see() != TOK2("&&"))
		return;
	conds[nconds++] = o_jz(0);
	ts_pop(NULL);
	while (!tok_jmp(TOK2("&&"))) {
		readbitor();
		conds[nconds++] = o_jz(0);
		ts_pop(NULL);
	}
	o_num(1, 4 | BT_SIGNED);
	o_tmpfork();
	passed = o_jmp(0);
	for (i = 0; i < nconds; i++)
		o_filljmp(conds[i]);
	o_num(0, 4 | BT_SIGNED);
	o_tmpjoin();
	o_filljmp(passed);
	ts_push_bt(4 | BT_SIGNED);
}

static void reador(void)
{
	long conds[MAXCOND];
	int nconds = 0;
	long failed;
	int i;
	readand();
	if (tok_see() != TOK2("||"))
		return;
	conds[nconds++] = o_jnz(0);
	ts_pop(NULL);
	while (!tok_jmp(TOK2("||"))) {
		readand();
		conds[nconds++] = o_jnz(0);
		ts_pop(NULL);
	}
	o_num(0, 4 | BT_SIGNED);
	o_tmpfork();
	failed = o_jmp(0);
	for (i = 0; i < nconds; i++)
		o_filljmp(conds[i]);
	o_num(1, 4 | BT_SIGNED);
	o_tmpjoin();
	o_filljmp(failed);
	ts_push_bt(4 | BT_SIGNED);
}

static void readcexpr(void)
{
	reador();
	if (!tok_jmp('?')) {
		long l1, l2;
		l1 = o_jz(0);
		ts_pop(NULL);
		reador();
		o_tmpfork();
		l2 = o_jmp(0);
		ts_pop(NULL);
		tok_expect(':');
		o_filljmp(l1);
		reador();
		o_tmpjoin();
		o_filljmp(l2);
	}
}

static void opassign(void (*op)(void))
{
	o_tmpcopy();
	readexpr();
	op();
	ts_pop(NULL);
	o_assign(TYPE_BT(&ts[nts - 1]));
}

static void readexpr(void)
{
	readcexpr();
	if (!tok_jmp('=')) {
		readexpr();
		ts_pop(NULL);
		o_assign(TYPE_BT(&ts[nts - 1]));
		return;
	}
	if (!tok_jmp(TOK2("+="))) {
		opassign(o_add);
		return;
	}
	if (!tok_jmp(TOK2("-="))) {
		opassign(o_sub);
		return;
	}
	if (!tok_jmp(TOK2("*="))) {
		opassign(o_mul);
		return;
	}
	if (!tok_jmp(TOK2("/="))) {
		opassign(o_div);
		return;
	}
	if (!tok_jmp(TOK2("%="))) {
		opassign(o_mod);
		return;
	}
	if (!tok_jmp(TOK3("<<="))) {
		opassign(o_shl);
		return;
	}
	if (!tok_jmp(TOK3(">>="))) {
		opassign(o_shr);
		return;
	}
	if (!tok_jmp(TOK3("&="))) {
		opassign(o_and);
		return;
	}
	if (!tok_jmp(TOK3("|="))) {
		opassign(o_or);
		return;
	}
	if (!tok_jmp(TOK3("^="))) {
		opassign(o_xor);
		return;
	}
}

static void readestmt(void)
{
	do {
		o_tmpdrop(-1);
		nts = 0;
		readexpr();
	} while (!tok_jmp(','));
}

static void localdef(struct name *name, int init)
{
	name->addr = o_mklocal(type_totsz(&name->type));
	local_add(name);
	if (init) {
		struct type *t = &name->type;
		o_local(locals[nlocals - 1].addr, TYPE_BT(t));
		readexpr();
		o_assign(TYPE_BT(t));
	}
}

static void funcdef(struct name *name, struct name *args, int nargs)
{
	int i;
	name->addr = o_func_beg(name->name);
	global_add(name);
	for (i = 0; i < nargs; i++) {
		args[i].addr = o_arg(i, type_totsz(&args[i].type));
		local_add(&args[i]);
	}
}

static int readdefs(void (*def)(struct name *name, int init))
{
	struct type base;
	if (basetype(&base))
		return 1;
	while (tok_see() != ';' && tok_see() != '{') {
		struct name name;
		struct type *type = &name.type;
		memcpy(type, &base, sizeof(base));
		readptrs(type);
		tok_expect(TOK_NAME);
		strcpy(name.name, tok_id());
		if (!tok_jmp('[')) {
			tok_expect(TOK_NUM);
			type->n = tok_num();
			type->ptr++;
			type->flags = T_ARRAY;
			tok_expect(']');
		}
		if (!tok_jmp('(')) {
			struct name args[MAXARGS];
			int nargs = 0;
			while (tok_see() != ')') {
				readtype(&args[nargs].type);
				if (!tok_jmp(TOK_NAME))
					strcpy(args[nargs++].name, tok_id());
				if (tok_jmp(','))
					break;
			}
			tok_expect(')');
			if (tok_see() != '{')
				continue;
			funcdef(&name, args, nargs);
			return 0;
		}
		def(&name, !tok_jmp('='));
	}
	return 0;
}

static void readstmt(void)
{
	o_tmpdrop(-1);
	nts = 0;
	if (!tok_jmp('{')) {
		while (tok_jmp('}'))
			readstmt();
		return;
	}
	if (!readdefs(localdef)) {
		tok_expect(';');
		return;
	}
	if (!tok_jmp(TOK_IF)) {
		long l1, l2;
		tok_expect('(');
		readexpr();
		tok_expect(')');
		l1 = o_jz(0);
		readstmt();
		if (!tok_jmp(TOK_ELSE)) {
			l2 = o_jmp(0);
			o_filljmp(l1);
			readstmt();
			o_filljmp(l2);
		} else {
			o_filljmp(l1);
		}
		return;
	}
	if (!tok_jmp(TOK_WHILE)) {
		long l1, l2;
		l1 = o_mklabel();
		tok_expect('(');
		readexpr();
		tok_expect(')');
		l2 = o_jz(0);
		readstmt();
		o_jz(l1);
		o_filljmp(l2);
		return;
	}
	if (!tok_jmp(TOK_FOR)) {
		long check, jump, end, body;
		tok_expect('(');
		if (tok_see() != ';')
			readestmt();
		tok_expect(';');
		check = o_mklabel();
		if (tok_see() != ';')
			readestmt();
		tok_expect(';');
		end = o_jz(0);
		body = o_jmp(0);
		jump = o_mklabel();
		if (tok_see() != ')')
			readestmt();
		tok_expect(')');
		o_jmp(check);
		o_filljmp(body);
		readstmt();
		o_jmp(jump);
		o_filljmp(end);
		return;
	}
	if (!tok_jmp(TOK_RETURN)) {
		int ret = tok_see() != ';';
		if (ret)
			readexpr();
		tok_expect(';');
		o_ret(4 | BT_SIGNED);
		return;
	}
	readestmt();
	tok_expect(';');
}

static void globaldef(struct name *name, int init)
{
	name->addr = o_mkvar(name->name, type_totsz(&name->type));
	global_add(name);
}

static void readdecl(void)
{
	readdefs(globaldef);
	if (tok_see() == '{') {
		readstmt();
		o_func_end();
		nlocals = 0;
		return;
	}
	tok_expect(';');
}

static void parse(void)
{
	while (tok_see() != TOK_EOF)
		readdecl();
}

int main(int argc, char *argv[])
{
	char obj[128];
	int ifd, ofd;
	int i = 1;
	while (i < argc && argv[i][0] == '-')
		i++;
	if (i == argc)
		die("no file given\n");
	ifd = open(argv[i], O_RDONLY);
	tok_init(ifd);
	close(ifd);
	parse();

	strcpy(obj, argv[i]);
	obj[strlen(obj) - 1] = 'o';
	ofd = open(obj, O_WRONLY | O_TRUNC | O_CREAT, 0600);
	out_write(ofd);
	close(ofd);
	return 0;
}
