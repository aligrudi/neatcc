#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "gen.h"
#include "tok.h"

#define MAXLOCALS	(1 << 10)
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

static struct local {
	char name[NAMELEN];
	long addr;
	struct type type;
} locals[MAXLOCALS];
static int nlocals;

static void local_add(char *name, long addr, struct type type)
{
	strcpy(locals[nlocals].name, name);
	locals[nlocals].addr = addr;
	locals[nlocals].type = type;
	nlocals++;
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
	int i;
	if (!tok_jmp(TOK_NUM)) {
		ts_push_bt(4 | BT_SIGNED);
		o_num(atoi(tok_id()), 4 | BT_SIGNED);
		return;
	}
	if (!tok_jmp(TOK_NAME)) {
		for (i = 0; i < nlocals; i++) {
			struct type *t = &locals[nlocals - 1].type;
			if (!strcmp(locals[i].name, tok_id())) {
				ts_push(t);
				o_local(locals[i].addr, TYPE_BT(t));
				if (t->flags & T_ARRAY)
					o_addr();
				return;
			}
		}
		ts_push_bt(8);
		o_symaddr(tok_id(), 8);
		return;
	}
	if (!tok_jmp('(')) {
		readexpr();
		tok_expect(')');
		return;
	}
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
		o_arrayderef(TYPE_DEREF_BT(&t1));
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
	}
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
	readpost();
}

static void shifts(int n)
{
	int i = -1;
	while (i++ < 16)
		if (n == 1 << i)
			break;
	return i;
}

static void ts_binop(void (*o_sth)(void))
{
	struct type t1, t2;
	ts_pop(&t1);
	ts_pop(&t2);
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

static void readadd(void)
{
	readpre();
	while (1) {
		if (!tok_jmp('+')) {
			readpre();
			ts_binop(o_add);
			continue;
		}
		if (!tok_jmp('-')) {
			readpre();
			ts_binop(o_sub);
			continue;
		}
		break;
	}
}

static void readcexpr(void)
{
	readadd();
	if (!tok_jmp('?')) {
		long l2;
		long l1 = o_jzstub();
		readadd();
		l2 = o_jmpstub();
		tok_expect(':');
		o_filljmp(l1);
		/* this is needed to fix tmp stack position */
		o_droptmp(1);
		readadd();
		o_filljmp(l2);
	}
}

static void readexpr(void)
{
	readcexpr();
	if (!tok_jmp('=')) {
		readexpr();
		o_assign(4 | BT_SIGNED);
	}
}

static void readstmt(void)
{
	struct type base = {0};
	o_droptmp(-1);
	nts = 0;
	if (!tok_jmp('{')) {
		while (tok_jmp('}'))
			readstmt();
		return;
	}
	if (!basetype(&base)) {
		struct type type = base;
		char name[NAMELEN];
		int n = 1;
		readptrs(&type);
		tok_expect(TOK_NAME);
		strcpy(name, tok_id());
		if (!tok_jmp('[')) {
			tok_expect(TOK_NUM);
			n = atoi(tok_id());
			type.ptr++;
			type.flags = T_ARRAY;
			tok_expect(']');
		}
		local_add(name, o_mklocal(TYPE_SZ(&type) * n), type);
		/* initializer */
		if (!tok_jmp('=')) {
			struct type *t = &locals[nlocals - 1].type;
			o_local(locals[nlocals - 1].addr, TYPE_BT(t));
			readexpr();
			ts_pop(NULL);
			ts_push_bt(TYPE_BT(t));
			o_assign(TYPE_BT(t));
			tok_expect(';');
		}
		return;
	}
	if (!tok_jmp(TOK_IF)) {
		long l1, l2;
		tok_expect('(');
		readexpr();
		tok_expect(')');
		l1 = o_jzstub();
		readstmt();
		if (!tok_jmp(TOK_ELSE)) {
			l2 = o_jmpstub();
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
		l2 = o_jzstub();
		readstmt();
		o_jz(l1);
		o_filljmp(l2);
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
	readexpr();
	tok_expect(';');
}

static void readdecl(void)
{
	char name[NAMELEN];
	struct type type;
	readtype(&type);
	tok_expect(TOK_NAME);
	strcpy(name, tok_id());
	if (!tok_jmp(';'))
		return;
	if (!tok_jmp('(')) {
		/* read args */
		char args[MAXARGS][NAMELEN];
		struct type types[MAXARGS];
		int nargs = 0;
		int i;
		while (tok_see() != ')') {
			readtype(&types[nargs]);
			if (!tok_jmp(TOK_NAME))
				strcpy(args[nargs++], tok_id());
			if (tok_jmp(','))
				break;
		}
		tok_expect(')');
		if (!tok_jmp(';'))
			return;
		o_func_beg(name);
		for (i = 0; i < nargs; i++)
			local_add(args[i], o_arg(i, TYPE_BT(&types[i])),
							types[i]);
		readstmt();
		o_func_end();
		return;
	}
	die("syntax error\n");
}

static void parse(void)
{
	while (tok_see() != TOK_EOF)
		readdecl();
}

int main(int argc, char *argv[])
{
	char obj[128];
	char *src = argv[1];
	int ifd, ofd;
	ifd = open(src, O_RDONLY);
	tok_init(ifd);
	close(ifd);
	parse();

	strcpy(obj, src);
	obj[strlen(obj) - 1] = 'o';
	ofd = open(obj, O_WRONLY | O_TRUNC | O_CREAT, 0600);
	out_write(ofd);
	close(ofd);
	return 0;
}
