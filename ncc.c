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

#define T_ARRAY		0x01
#define T_STRUCT	0x02
#define T_FUNC		0x04

#define F_INIT		0x01
#define F_STATIC	0x02

struct type {
	unsigned bt;
	unsigned flags;
	int ptr;
	int id;		/* for structs, functions and arrays */
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

static void ts_push(struct type *t)
{
	memcpy(&ts[nts++], t, sizeof(*t));
	if (t->flags & (T_FUNC | T_ARRAY) && !t->ptr)
		o_addr();
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

static int global_find(char *name)
{
	int i;
	for (i = 0; i < nglobals; i++)
		if (!strcmp(name, globals[i].name))
			return i;
	return -1;
}

static void global_add(struct name *name)
{
	int found = global_find(name->name);
	int i = found == -1 ? nglobals++ : found;
	memcpy(&globals[i], name, sizeof(*name));
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
	for (i = nenums - 1; i >= 0; --i)
		if (!strcmp(name, enums[i].name)) {
			*val = enums[i].n;
			return 0;
		}
	return 1;
}

#define MAXTYPEDEFS		(1 << 5)

static struct typdefinfo {
	char name[NAMELEN];
	struct type type;
} typedefs[MAXTYPEDEFS];
static int ntypedefs;

static void typedef_add(char *name, struct type *type)
{
	struct typdefinfo *ti = &typedefs[ntypedefs++];
	strcpy(ti->name, name);
	memcpy(&ti->type, type, sizeof(*type));
}

static int typedef_find(char *name)
{
	int i;
	for (i = ntypedefs - 1; i >= 0; --i)
		if (!strcmp(name, typedefs[i].name))
			return i;
	return -1;
}

#define MAXARRAYS		(1 << 5)

static struct array {
	struct type type;
	int n;
} arrays[MAXARRAYS];
static int narrays;

static int array_add(struct type *type, int n)
{
	struct array *a = &arrays[narrays++];
	memcpy(&a->type, type, sizeof(*type));
	a->n = n;
	return a - arrays;
}

static void array2ptr(struct type *t)
{
	if (!(t->flags & T_ARRAY) || t->ptr)
		return;
	memcpy(t, &arrays[t->id].type, sizeof(*t));
	t->ptr++;
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
	for (i = nstructs - 1; i >= 0; --i)
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

#define MAXBREAK		(1 << 7)
static long breaks[MAXBREAK];
static int nbreaks;
static long continues[MAXBREAK];
static int ncontinues;

static void break_fill(long addr, int till)
{
	int i;
	for (i = till; i < nbreaks; i++)
		o_filljmp2(breaks[i], addr);
	nbreaks = till;
}

static void continue_fill(long addr, int till)
{
	int i;
	for (i = till; i < ncontinues; i++)
		o_filljmp2(continues[i], addr);
	ncontinues = till;
}

static int type_totsz(struct type *t)
{
	if (t->ptr)
		return 8;
	if (t->flags & T_ARRAY)
		return arrays[t->id].n * type_totsz(&arrays[t->id].type);
	return t->flags & T_STRUCT ? structs[t->id].size : BT_SZ(t->bt);
}

static unsigned type_szde(struct type *t)
{
	if (t->flags & T_ARRAY)
		return t->ptr > 0 ? 8 : TYPE_SZ(&arrays[t->id].type);
	else
		return t->ptr > 1 ? 8 : BT_SZ(t->bt);
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
	array2ptr(&t1);
	array2ptr(&t2);
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
		if (type_szde(&t2) > 1) {
			o_num(shifts(type_szde(&t2)), 1);
			o_shl();
		}
	o_sth();
	if (t1.ptr && t2.ptr) {
		o_num(shifts(type_szde(&t1)), 1);
		o_shr();
		ts_push_bt(4 | BT_SIGNED);
	} else {
		ts_push(&t2);
	}
}

static void structdef(void *data, struct name *name, unsigned flags)
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

static int readdefs(void (*def)(void *, struct name *, unsigned f), void *data);

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

static int basetype(struct type *type, unsigned *flags)
{
	int sign = 1;
	int size = 4;
	int done = 0;
	int i = 0;
	int isunion;
	char name[NAMELEN];
	*flags = 0;
	type->flags = 0;
	type->ptr = 0;
	while (!done) {
		switch (tok_see()) {
		case TOK_STATIC:
			*flags |= F_STATIC;
			break;
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
			type->flags |= T_STRUCT;
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
			if (tok_see() == TOK_NAME) {
				int id = typedef_find(tok_id());
				if (id != -1) {
					tok_get();
					memcpy(type, &typedefs[id].type,
						sizeof(*type));
					return 0;
				}
			}
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
	unsigned flags;
	if (basetype(type, &flags))
		return 1;
	readptrs(type);
	return 0;
}

static void readpre(void);

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
		ts_push(&t);
		len = tok_str(buf);
		o_symaddr(o_mkdat(NULL, buf, len, 0), TYPE_BT(&t));
		o_addr();
		return;
	}
	if (!tok_jmp(TOK_NAME)) {
		int n;
		for (i = nlocals - 1; i >= 0; --i) {
			struct type *t = &locals[i].type;
			if (!strcmp(locals[i].name, tok_id())) {
				o_local(locals[i].addr, TYPE_BT(t));
				ts_push(t);
				return;
			}
		}
		if ((n = global_find(tok_id())) != -1) {
			struct type *t = &globals[n].type;
			o_symaddr(globals[n].addr, TYPE_BT(t));
			ts_push(t);
			return;
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
		struct type t;
		if (!readtype(&t)) {
			struct type o;
			tok_expect(')');
			readpre();
			ts_pop(&o);
			ts_push(&t);
			if (!t.ptr || !o.ptr)
				o_cast(TYPE_BT(&t));
		} else {
			readexpr();
			tok_expect(')');
		}
		return;
	}
}

void arrayderef(struct type *t)
{
	int sz = type_totsz(t);
	if (sz > 1) {
		o_num(sz, 4);
		o_mul();
	}
	o_add();
	o_deref(TYPE_BT(t));
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
	o_deref(TYPE_BT(&field->type));
	ts_push(&field->type);
}

#define MAXFUNCS		(1 << 10)

static struct funcinfo {
	struct type args[MAXFIELDS];
	struct type ret;
	int nargs;
} funcs[MAXFUNCS];
static int nfuncs;
static unsigned ret_bt;

static int func_create(struct type *ret, struct name *args, int nargs)
{
	struct funcinfo *fi = &funcs[nfuncs++];
	int i;
	memcpy(&fi->ret, ret, sizeof(*ret));
	for (i = 0; i < nargs; i++)
		memcpy(&fi->args[i], &args[i].type, sizeof(*ret));
	fi->nargs = nargs;
	return fi - funcs;
}

static void readcall(void)
{
	struct type t;
	unsigned bt[MAXARGS];
	struct funcinfo *fi;
	int argc = 0;
	int i;
	if (tok_see() != ')') {
		readexpr();
		ts_pop(&t);
		bt[argc++] = TYPE_BT(&t);
	}
	while (!tok_jmp(',')) {
		readexpr();
		ts_pop(&t);
		bt[argc++] = TYPE_BT(&t);
	}
	tok_expect(')');
	ts_pop(&t);
	if (t.flags & T_FUNC && t.ptr > 0)
		o_deref(8);
	fi = t.flags & T_FUNC ? &funcs[t.id] : NULL;
	if (fi)
		for (i = 0; i < fi->nargs; i++)
			bt[i] = TYPE_BT(&fi->args[i]);
	o_call(argc, bt, fi ? TYPE_BT(&fi->ret) : 4 | BT_SIGNED);
	if (fi)
		ts_push(&fi->ret);
	else
		ts_push_bt(4 | BT_SIGNED);
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
			array2ptr(&t);
			t.ptr--;
			arrayderef(&t);
			ts_push(&t);
			continue;
		}
		if (!tok_jmp('(')) {
			readcall();
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
		if (!(type.flags & T_FUNC) && !type.ptr)
			type.ptr++;
		ts_push(&type);
		o_addr();
		return;
	}
	if (!tok_jmp('*')) {
		struct type t;
		readpre();
		ts_pop(&t);
		array2ptr(&t);
		if (!(t.flags & T_FUNC) || t.ptr > 0) {
			t.ptr--;
			o_deref(TYPE_BT(&t));
		}
		ts_push(&t);
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

static void doassign(void)
{
	struct type t;
	ts_pop(&t);
	if (!t.ptr && t.flags & T_STRUCT)
		o_memcpy(type_totsz(&t));
	else
		o_assign(TYPE_BT(&ts[nts - 1]));
}

static void readexpr(void)
{
	readcexpr();
	if (!tok_jmp('=')) {
		readexpr();
		doassign();
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

#define F_GLOBAL(flags)		(!((flags) & F_STATIC))

static void globaldef(void *data, struct name *name, unsigned flags)
{
	char *varname = flags & F_STATIC ? NULL : name->name;
	name->addr = o_mkvar(varname, type_totsz(&name->type), F_GLOBAL(flags));
	global_add(name);
}

static void o_localoff(long addr, int off, unsigned bt)
{
	o_local(addr, bt);
	if (off) {
		o_addr();
		o_num(off, 4);
		o_add();
		o_deref(bt);
	}
}

static struct type *innertype(struct type *t)
{
	if (t->flags & T_ARRAY && !t->ptr)
		return innertype(&arrays[t->id].type);
	return t;
}

static void initexpr(struct type *t, long addr, int off)
{
	if (tok_jmp('{')) {
		o_localoff(addr, off, TYPE_BT(t));
		ts_push(t);
		readexpr();
		doassign();
		ts_pop(NULL);
		o_tmpdrop(1);
		return;
	}
	if (!t->ptr && t->flags & T_STRUCT) {
		struct structinfo *si = &structs[t->id];
		int i;
		for (i = 0; i < si->nfields; i++) {
			struct name *field = &si->fields[i];
			if (!tok_jmp('.')) {
				tok_expect(TOK_NAME);
				field = struct_field(t->id, tok_id());
				tok_expect('=');
			}
			initexpr(&field->type, addr, off + field->addr);
			if (tok_jmp(',') || tok_see() == '}')
				break;
		}
	} else {
		struct type t_de;
		int i;
		memcpy(&t_de, t, sizeof(*t));
		if (t->flags & T_ARRAY)
			array2ptr(&t_de);
		t_de.ptr--;
		for (i = 0; ; i++) {
			long idx = i;
			struct type *it = &t_de;
			if (!tok_jmp('[')) {
				readexpr();
				o_popnum(&idx);
				ts_pop(NULL);
				tok_expect(']');
				tok_expect('=');
			}
			if (tok_see() != '{')
				it = innertype(&t_de);
			initexpr(it, addr, off + type_totsz(it) * idx);
			if (tok_jmp(',') || tok_see() == '}')
				break;
		}
	}
	tok_expect('}');
}

static void localdef(void *data, struct name *name, unsigned flags)
{
	if (flags & F_STATIC) {
		globaldef(data, name, flags);
		return;
	}
	name->addr = o_mklocal(type_totsz(&name->type));
	local_add(name);
	if (flags & F_INIT) {
		struct type *t = &name->type;
		if (tok_see() == '{') {
			o_local(name->addr, TYPE_BT(t));
			o_memset(0, type_totsz(t));
			o_tmpdrop(1);
		}
		initexpr(t, name->addr, 0);
	}
}

static void funcdef(struct name *name, struct name *args,
			int nargs, unsigned flags)
{
	int i;
	name->addr = o_func_beg(name->name, F_GLOBAL(flags));
	global_add(name);
	ret_bt = TYPE_BT(&funcs[name->type.id].ret);
	for (i = 0; i < nargs; i++) {
		args[i].addr = o_arg(i, type_totsz(&args[i].type));
		local_add(&args[i]);
	}
}

static int readargs(struct name *args)
{
	int nargs = 0;
	tok_expect('(');
	while (tok_see() != ')') {
		readtype(&args[nargs].type);
		if (!tok_jmp(TOK_NAME))
			strcpy(args[nargs++].name, tok_id());
		if (tok_jmp(','))
			break;
	}
	tok_expect(')');
	return nargs;
}

static int readdefs(void (*def)(void *data, struct name *name, unsigned flags),
			void *data)
{
	struct type base;
	unsigned flags;
	if (basetype(&base, &flags))
		return 1;
	while (tok_see() != ';' && tok_see() != '{') {
		struct type tpool[3];
		struct name name;
		int npool = 0;
		struct type *type = &tpool[npool++];
		struct type *func = NULL;
		struct type *ret = NULL;
		memset(tpool, 0, sizeof(tpool));
		memcpy(type, &base, sizeof(base));
		readptrs(type);
		if (!tok_jmp('(')) {
			ret = type;
			type = &tpool[npool++];
			func = type;
			readptrs(type);
		}
		tok_expect(TOK_NAME);
		strcpy(name.name, tok_id());
		while (!tok_jmp('[')) {
			long n;
			readexpr();
			ts_pop(NULL);
			if (o_popnum(&n))
				die("const expr expected\n");
			type->id = array_add(type, n);
			if (type->flags & T_FUNC)
				func = &arrays[type->id].type;
			type->flags = T_ARRAY;
			type->bt = 8;
			type->ptr = 0;
			tok_expect(']');
		}
		if (func)
			tok_expect(')');
		if (tok_see() == '(') {
			struct name args[MAXARGS];
			int nargs = readargs(args);
			int fdef = !func;
			if (!func) {
				ret = type;
				type = &tpool[npool++];
				func = type;
			}
			func->flags = T_FUNC;
			func->bt = 8;
			func->id = func_create(ret, args, nargs);
			if (fdef && tok_see() == '{') {
				memcpy(&name.type, func, sizeof(*func));
				funcdef(&name, args, nargs, flags);
				return 0;
			}
		}
		memcpy(&name.type, type, sizeof(*type));
		if (!tok_jmp('='))
			flags |= F_INIT;
		def(data, &name, flags);
		tok_jmp(',');
	}
	return 0;
}

static void typedefdef(void *data, struct name *name, unsigned flags)
{
	typedef_add(name->name, &name->type);
}

#define MAXCASES		(1 << 7)

static void readstmt(void);

static void readswitch(void)
{
	int break_beg = nbreaks;
	long val_addr = o_mklocal(8);
	long matched[MAXCASES];
	int nmatched = 0;
	struct type t;
	long next;
	int ref = 1;
	int i;
	tok_expect('(');
	readexpr();
	ts_pop(&t);
	o_local(val_addr, TYPE_BT(&t));
	o_tmpswap();
	o_assign(TYPE_BT(&t));
	o_tmpdrop(1);
	tok_expect(')');
	tok_expect('{');
	while (tok_jmp('}')) {
		int n = 0;
		while (tok_see() == TOK_CASE || tok_see() == TOK_DEFAULT) {
			if (n++ > 0)
				matched[nmatched++] = o_jmp(0);
			if (!ref++)
				o_filljmp(next);
			if (!tok_jmp(TOK_CASE)) {
				readexpr();
				o_local(val_addr, TYPE_BT(&t));
				o_eq();
				next = o_jz(0);
				ref = 0;
				tok_expect(':');
				o_tmpdrop(1);
				ts_pop(NULL);
				continue;
			}
			if (!tok_jmp(TOK_DEFAULT)) {
				tok_expect(':');
				continue;
			}
		}
		for (i = 0; i < nmatched; i++)
			o_filljmp(matched[i]);
		nmatched = 0;
		readstmt();
	}
	o_rmlocal(val_addr, 8);
	if (!ref++)
		o_filljmp(next);
	break_fill(o_mklabel(), break_beg);
}

#define MAXGOTO			(1 << 7)

static struct gotoinfo {
	char name[NAMELEN];
	long addr;
} gotos[MAXGOTO];
static int ngotos;

static struct labelinfo {
	char name[NAMELEN];
	long addr;
} labels[MAXGOTO];
static int nlabels;

static void goto_add(char *name)
{
	strcpy(gotos[ngotos].name, name);
	gotos[ngotos++].addr = o_jmp(0);
}

static void label_add(char *name)
{
	strcpy(labels[nlabels].name, name);
	labels[nlabels++].addr = o_mklabel();
}

static void goto_fill(void)
{
	int i, j;
	for (i = 0; i < ngotos; i++)
		for (j = 0; j < nlabels; j++)
			if (!strcmp(gotos[i].name, labels[j].name)) {
				o_filljmp2(gotos[i].addr, labels[j].addr);
				break;
			}
}

static void readstmt(void)
{
	o_tmpdrop(-1);
	nts = 0;
	if (!tok_jmp('{')) {
		int _nlocals = nlocals;
		int _nglobals = nglobals;
		int _nenums = nenums;
		int _ntypedefs = ntypedefs;
		int _nstructs = nstructs;
		int _nfuncs = nfuncs;
		int _narrays = narrays;
		while (tok_jmp('}'))
			readstmt();
		nlocals = _nlocals;
		nenums = _nenums;
		ntypedefs = _ntypedefs;
		nstructs = _nstructs;
		nfuncs = _nfuncs;
		narrays = _narrays;
		nglobals = _nglobals;
		return;
	}
	if (!readdefs(localdef, NULL)) {
		tok_expect(';');
		return;
	}
	if (!tok_jmp(TOK_TYPEDEF)) {
		readdefs(typedefdef, NULL);
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
		int break_beg = nbreaks;
		int continue_beg = ncontinues;
		l1 = o_mklabel();
		tok_expect('(');
		readexpr();
		tok_expect(')');
		l2 = o_jz(0);
		readstmt();
		o_jmp(l1);
		o_filljmp(l2);
		break_fill(o_mklabel(), break_beg);
		continue_fill(l1, continue_beg);
		return;
	}
	if (!tok_jmp(TOK_DO)) {
		long l1, l2;
		int break_beg = nbreaks;
		int continue_beg = ncontinues;
		l1 = o_mklabel();
		readstmt();
		tok_expect(TOK_WHILE);
		tok_expect('(');
		l2 = o_mklabel();
		readexpr();
		o_jnz(l1);
		tok_expect(')');
		break_fill(o_mklabel(), break_beg);
		continue_fill(l2, continue_beg);
		return;
	}
	if (!tok_jmp(TOK_FOR)) {
		long check, jump, end, body;
		int break_beg = nbreaks;
		int continue_beg = ncontinues;
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
		break_fill(o_mklabel(), break_beg);
		continue_fill(jump, continue_beg);
		return;
	}
	if (!tok_jmp(TOK_SWITCH)) {
		readswitch();
		return;
	}
	if (!tok_jmp(TOK_RETURN)) {
		int ret = tok_see() != ';';
		if (ret)
			readexpr();
		tok_expect(';');
		o_ret(ret_bt);
		return;
	}
	if (!tok_jmp(TOK_BREAK)) {
		tok_expect(';');
		breaks[nbreaks++] = o_jmp(0);
		return;
	}
	if (!tok_jmp(TOK_CONTINUE)) {
		tok_expect(';');
		continues[ncontinues++] = o_jmp(0);
		return;
	}
	if (!tok_jmp(TOK_GOTO)) {
		tok_expect(TOK_NAME);
		goto_add(tok_id());
		tok_expect(';');
		return;
	}
	readestmt();
	/* labels */
	if (!tok_jmp(':')) {
		label_add(tok_id());
		return;
	}
	tok_expect(';');
}

static void readdecl(void)
{
	if (!tok_jmp(TOK_TYPEDEF)) {
		readdefs(typedefdef, NULL);
		tok_expect(';');
		return;
	}
	readdefs(globaldef, NULL);
	if (tok_see() == '{') {
		readstmt();
		goto_fill();
		o_func_end();
		nlocals = 0;
		ngotos = 0;
		nlabels = 0;
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
