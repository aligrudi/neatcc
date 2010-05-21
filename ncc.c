/*
 * neatcc - A small and simple x86_64 C compiler
 *
 * Copyright (C) 2010 Ali Gholami Rudi
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, as published by the
 * Free Software Foundation.
 */
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
#define T_STRUCT	0x02

struct type {
	unsigned bt;
	unsigned flags;
	int id;		/* for structs */
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

#define MAXENUMS		(1 << 10)

static struct enumval {
	char name[NAMELEN];
	int n;
} enums[MAXENUMS];
static int nenums;

static void enum_add(char *name, int val)
{
	struct enumval *ev = &enums[nenums++];
	strcpy(ev->name, name);
	ev->n = val;
}

static int enum_find(int *val, char *name)
{
	int i;
	for (i = 0; i < nenums; i++)
		if (!strcmp(name, enums[i].name)) {
			*val = enums[i].n;
			return 0;
		}
	return 1;
}

#define MAXTYPES		(1 << 7)
#define MAXFIELDS		(1 << 5)

static struct structinfo {
	char name[NAMELEN];
	struct name fields[MAXFIELDS];
	int nfields;
	int isunion;
	int size;
} structs[MAXTYPES];
static int nstructs;

static int struct_find(char *name, int isunion)
{
	int i;
	for (i = 0; i < nstructs; i++)
		if (!strcmp(name, structs[i].name) &&
				structs[i].isunion == isunion)
			return i;
	die("struct not found\n");
}

static struct name *struct_field(int id, char *name)
{
	struct structinfo *si = &structs[id];
	int i;
	for (i = 0; i < si->nfields; i++)
		if (!strcmp(name, si->fields[i].name))
			return &si->fields[i];
	die("field not found\n");
}

static int type_totsz(struct type *t)
{
	if (!t->ptr || t->flags & T_ARRAY && (t)->ptr == 1)
		return t->n * (t->flags & T_STRUCT ? structs[t->id].size :
				BT_SZ(t->bt));
	return 8;
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

static int shifts(int n)
{
	int i = -1;
	while (i++ < 16)
		if (n == 1 << i)
			break;
	return i;
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

static void structdef(void *data, struct name *name, int init)
{
	struct structinfo *si = data;
	if (si->isunion) {
		name->addr = 0;
		if (si->size < type_totsz(&name->type))
			si->size = type_totsz(&name->type);
	} else {
		name->addr = si->size;
		si->size += type_totsz(&name->type);
	}
	memcpy(&si->fields[si->nfields++], name, sizeof(*name));
}

static int readdefs(void (*def)(void *, struct name *, int init), void *data);

static int struct_create(char *name, int isunion)
{
	int id = nstructs++;
	struct structinfo *si = &structs[id];
	strcpy(si->name, name);
	si->isunion = isunion;
	tok_expect('{');
	while (tok_jmp('}')) {
		readdefs(structdef, si);
		tok_expect(';');
	}
	return id;
}

static void readexpr(void);

static void enum_create(void)
{
	long n = 0;
	tok_expect('{');
	while (tok_jmp('}')) {
		char name[NAMELEN];
		tok_expect(TOK_NAME);
		strcpy(name, tok_id());
		if (tok_see() == '=') {
			tok_get();
			readexpr();
			ts_pop(NULL);
			if (o_popnum(&n))
				die("const expr expected!\n");
		}
		enum_add(name, n++);
		tok_jmp(',');
	}
}

static int basetype(struct type *type)
{
	int sign = 1;
	int size = 4;
	int done = 0;
	int i = 0;
	int isunion;
	char name[NAMELEN];
	type->flags = 0;
	type->ptr = 0;
	type->n = 1;
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
		case TOK_UNION:
		case TOK_STRUCT:
			isunion = tok_get() == TOK_UNION;
			tok_expect(TOK_NAME);
			strcpy(name, tok_id());
			if (tok_see() == '{')
				type->id = struct_create(name, isunion);
			else
				type->id = struct_find(name, isunion);
			type->flags = T_STRUCT;
			type->bt = 8;
			return 0;
		case TOK_ENUM:
			tok_get();
			tok_expect(TOK_NAME);
			if (tok_see() == '{')
				enum_create();
			type->bt = 4 | BT_SIGNED;
			return 0;
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
	if (!tok_jmp(TOK_STR)) {
		struct type t;
		char buf[BUFSIZE];
		int len;
		t.bt = 1 | BT_SIGNED;
		t.ptr = 1;
		t.flags = 0;
		t.n = 1;
		ts_push(&t);
		len = tok_str(buf);
		o_symaddr(o_mkdat(NULL, buf, len), TYPE_BT(&t));
		o_addr();
		return;
	}
	if (!tok_jmp(TOK_NAME)) {
		int n;
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
		if (!enum_find(&n, tok_id())) {
			ts_push_bt(4 | BT_SIGNED);
			o_num(n, 4 | BT_SIGNED);
			return;
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
	unsigned bt = TYPE_BT(&ts[nts - 1]);
	o_tmpcopy();
	o_load();
	o_tmpswap();
	o_tmpcopy();
	o_num(1, 4);
	ts_push_bt(bt);
	ts_push_bt(bt);
	ts_binop_add(op);
	ts_pop(NULL);
	o_assign(bt);
	o_tmpdrop(1);
}

static void readfield(void)
{
	struct name *field;
	struct type t;
	tok_expect(TOK_NAME);
	ts_pop(&t);
	field = struct_field(t.id, tok_id());
	if (field->addr) {
		o_num(field->addr, 4);
		o_add();
	}
	if (!(field->type.flags & T_ARRAY))
		o_deref(TYPE_BT(&field->type));
	ts_push(&field->type);
}

static void readpost(void)
{
	readprimary();
	while (1) {
		if (!tok_jmp('[')) {
			struct type t;
			ts_pop(&t);
			readexpr();
			ts_pop(NULL);
			tok_expect(']');
			arrayderef(TYPE_DEREF_BT(&t));
			t.ptr--;
			t.flags &= ~T_ARRAY;
			t.n = 1;
			ts_push(&t);
			continue;
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
			continue;
		}
		if (!tok_jmp(TOK2("++"))) {
			inc_post(o_add);
			continue;
		}
		if (!tok_jmp(TOK2("--"))) {
			inc_post(o_sub);
			continue;
		}
		if (!tok_jmp('.')) {
			o_addr();
			readfield();
			continue;
		}
		if (!tok_jmp(TOK2("->"))) {
			readfield();
			continue;
		}
		break;
	}
}

static void readpre(void);

static void inc_pre(void (*op)(void))
{
	unsigned bt = TYPE_BT(&ts[nts - 1]);
	readpre();
	o_tmpcopy();
	o_num(1, 4);
	ts_push_bt(bt);
	ts_push_bt(bt);
	ts_binop_add(op);
	ts_pop(NULL);
	o_assign(bt);
}

static void readpre(void)
{
	if (!tok_jmp('&')) {
		struct type type;
		readpre();
		ts_pop(&type);
		type.ptr++;
		ts_push(&type);
		o_addr();
		return;
	}
	if (!tok_jmp('*')) {
		struct type t;
		readpre();
		ts_pop(&t);
		t.ptr--;
		t.flags &= ~T_ARRAY;
		t.n = 1;
		ts_push(&t);
		o_deref(TYPE_BT(&t));
		return;
	}
	if (!tok_jmp('!')) {
		struct type type;
		readpre();
		ts_pop(&type);
		o_lnot();
		ts_push_bt(4 | BT_SIGNED);
		return;
	}
	if (!tok_jmp('-')) {
		readpre();
		o_neg();
		return;
	}
	if (!tok_jmp('~')) {
		readpre();
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
	if (!tok_jmp(TOK_SIZEOF)) {
		struct type t;
		int op = !tok_jmp('(');
		if (readtype(&t)) {
			int nogen = !o_nogen();
			readexpr();
			if (nogen)
				o_dogen();
			ts_pop(&t);
			o_tmpdrop(1);
		}
		ts_push_bt(4);
		o_num(type_totsz(&t), 4);
		if (op)
			tok_expect(')');
		return;
	}
	readpost();
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
	while (1) {
		if (!tok_jmp(TOK2("<<"))) {
			shift(o_shl);
			continue;
		}
		if (!tok_jmp(TOK2(">>"))) {
			shift(o_shr);
			continue;
		}
		break;
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
	while (1) {
		if (!tok_jmp('<')) {
			cmp(o_lt);
			continue;
		}
		if (!tok_jmp('>')) {
			cmp(o_gt);
			continue;
		}
		if (!tok_jmp(TOK2("<="))) {
			cmp(o_le);
			continue;
		}
		if (!tok_jmp(TOK2(">="))) {
			cmp(o_ge);
			continue;
		}
		break;
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
	while (1) {
		if (!tok_jmp(TOK2("=="))) {
			eq(o_eq);
			continue;
		}
		if (!tok_jmp(TOK2("!="))) {
			eq(o_neq);
			continue;
		}
		break;
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
	long l1, l2;
	long c;
	int cexpr, nogen;
	reador();
	if (tok_jmp('?'))
		return;
	cexpr = !o_popnum(&c);
	ts_pop(NULL);
	if (cexpr) {
		if (!c)
			nogen = !o_nogen();
	} else {
		l1 = o_jz(0);
	}
	reador();
	if (!cexpr) {
		o_tmpfork();
		l2 = o_jmp(0);
	}
	ts_pop(NULL);
	tok_expect(':');
	if (cexpr) {
		if (c) {
			nogen = !o_nogen();
		} else {
			if (nogen)
				o_dogen();
			o_tmpdrop(1);
		}
	} else {
		o_filljmp(l1);
	}
	reador();
	if (cexpr) {
		if (c) {
			if (nogen)
				o_dogen();
			o_tmpdrop(1);
		}
	} else {
		o_tmpjoin();
		o_filljmp(l2);
	}
}

static void opassign(void (*bop)(void (*op)(void)), void (*op)(void))
{
	unsigned bt = TYPE_BT(&ts[nts - 1]);
	o_tmpcopy();
	readexpr();
	bop(op);
	ts_pop(NULL);
	o_assign(bt);
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
		opassign(ts_binop_add, o_add);
		return;
	}
	if (!tok_jmp(TOK2("-="))) {
		opassign(ts_binop_add, o_sub);
		return;
	}
	if (!tok_jmp(TOK2("*="))) {
		opassign(ts_binop, o_mul);
		return;
	}
	if (!tok_jmp(TOK2("/="))) {
		opassign(ts_binop, o_div);
		return;
	}
	if (!tok_jmp(TOK2("%="))) {
		opassign(ts_binop, o_mod);
		return;
	}
	if (!tok_jmp(TOK3("<<="))) {
		opassign(ts_binop, o_shl);
		return;
	}
	if (!tok_jmp(TOK3(">>="))) {
		opassign(ts_binop, o_shr);
		return;
	}
	if (!tok_jmp(TOK3("&="))) {
		opassign(ts_binop, o_and);
		return;
	}
	if (!tok_jmp(TOK3("|="))) {
		opassign(ts_binop, o_or);
		return;
	}
	if (!tok_jmp(TOK3("^="))) {
		opassign(ts_binop, o_xor);
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

static void localdef(void *data, struct name *name, int init)
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

static int readdefs(void (*def)(void *data, struct name *name, int init), void *data)
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
			long n;
			readexpr();
			ts_pop(NULL);
			if (o_popnum(&n))
				die("const expr expected\n");
			type->n = n;
			type->ptr++;
			type->flags |= T_ARRAY;
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
		def(data, &name, !tok_jmp('='));
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
	if (!readdefs(localdef, NULL)) {
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

static void globaldef(void *data, struct name *name, int init)
{
	name->addr = o_mkvar(name->name, type_totsz(&name->type));
	global_add(name);
}

static void readdecl(void)
{
	readdefs(globaldef, NULL);
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
