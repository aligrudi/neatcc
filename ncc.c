/*
 * neatcc - A small and simple C compiler
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

#define TYPE_BT(t)		((t)->ptr ? LONGSZ : (t)->bt)
#define TYPE_SZ(t)		((t)->ptr ? LONGSZ : (t)->bt & BT_SZMASK)

#define T_ARRAY		0x01
#define T_STRUCT	0x02
#define T_FUNC		0x04

#define F_INIT		0x01
#define F_STATIC	0x02
#define F_EXTERN	0x04

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

void die(char *msg)
{
	print(msg);
	exit(1);
}

void err(char *msg)
{
	char err[1 << 7];
	int len = cpp_loc(err, tok_addr());
	strcpy(err + len, msg);
	die(err);
}

struct name {
	char name[NAMELEN];
	struct type type;
	long addr;
	int unused;		/* unreferenced external symbols */
};

static struct name locals[MAXLOCALS];
static int nlocals;
static struct name globals[MAXGLOBALS];
static int nglobals;

static void local_add(struct name *name)
{
	if (nlocals >= MAXLOCALS)
		err("nomem: MAXLOCALS reached!\n");
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
	if (nglobals >= MAXGLOBALS)
		err("nomem: MAXGLOBALS reached!\n");
	memcpy(&globals[i], name, sizeof(*name));
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
	if (nenums >= MAXENUMS)
		err("nomem: MAXENUMS reached!\n");
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

#define MAXTYPEDEFS		(1 << 10)

static struct typdefinfo {
	char name[NAMELEN];
	struct type type;
} typedefs[MAXTYPEDEFS];
static int ntypedefs;

static void typedef_add(char *name, struct type *type)
{
	struct typdefinfo *ti = &typedefs[ntypedefs++];
	if (ntypedefs >= MAXTYPEDEFS)
		err("nomem: MAXTYPEDEFS reached!\n");
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

#define MAXARRAYS		(1 << 10)

static struct array {
	struct type type;
	int n;
} arrays[MAXARRAYS];
static int narrays;

static int array_add(struct type *type, int n)
{
	struct array *a = &arrays[narrays++];
	if (narrays >= MAXARRAYS)
		err("nomem: MAXARRAYS reached!\n");
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

#define MAXSTRUCTS		(1 << 10)
#define MAXFIELDS		(1 << 7)

static struct structinfo {
	char name[NAMELEN];
	struct name fields[MAXFIELDS];
	int nfields;
	int isunion;
	int size;
} structs[MAXSTRUCTS];
static int nstructs;

static int struct_find(char *name, int isunion)
{
	int i;
	for (i = nstructs - 1; i >= 0; --i)
		if (*structs[i].name && !strcmp(name, structs[i].name) &&
				structs[i].isunion == isunion)
			return i;
	i = nstructs++;
	if (nstructs >= MAXSTRUCTS)
		err("nomem: MAXTYPES reached!\n");
	memset(&structs[i], 0, sizeof(structs[i]));
	strcpy(structs[i].name, name);
	structs[i].isunion = isunion;
	return i;
}

static struct name *struct_field(int id, char *name)
{
	struct structinfo *si = &structs[id];
	int i;
	for (i = 0; i < si->nfields; i++)
		if (!strcmp(name, si->fields[i].name))
			return &si->fields[i];
	err("field not found\n");
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
		return LONGSZ;
	if (t->flags & T_ARRAY)
		return arrays[t->id].n * type_totsz(&arrays[t->id].type);
	return t->flags & T_STRUCT ? structs[t->id].size : BT_SZ(t->bt);
}

static unsigned type_szde(struct type *t)
{
	struct type de = *t;
	array2ptr(&de);
	de.ptr--;
	return type_totsz(&de);
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
		err("syntax error\n");
}

static unsigned bt_op(unsigned bt1, unsigned bt2)
{
	unsigned s1 = BT_SZ(bt1);
	unsigned s2 = BT_SZ(bt2);
	return (bt1 | bt2) & BT_SIGNED | (s1 > s2 ? s1 : s2);
}

static void ts_binop(int op)
{
	struct type t1, t2;
	ts_pop(&t1);
	ts_pop(&t2);
	o_bop(op);
	ts_push_bt(bt_op(TYPE_BT(&t1), TYPE_BT(&t2)));
}

static void ts_addop(int op)
{
	struct type t1, t2;
	ts_pop(&t1);
	ts_pop(&t2);
	array2ptr(&t1);
	array2ptr(&t2);
	if (!t1.ptr && !t2.ptr) {
		o_bop(op);
		ts_push_bt(bt_op(TYPE_BT(&t1), TYPE_BT(&t2)));
		return;
	}
	if (t1.ptr && !t2.ptr)
		o_tmpswap();
	if (!t1.ptr && t2.ptr)
		if (type_szde(&t2) > 1) {
			o_num(type_szde(&t2), 4);
			o_bop(O_MUL);
		}
	if (t1.ptr && !t2.ptr)
		o_tmpswap();
	o_bop(op);
	if (t1.ptr && t2.ptr) {
		int sz = type_szde(&t1);
		if (sz > 1) {
			o_num(sz, 4);
			o_bop(O_DIV);
		}
		ts_push_bt(4 | BT_SIGNED);
	} else {
		ts_push(t1.ptr ? &t1 : &t2);
	}
}

#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))
#define MIN(a, b)		((a) < (b) ? (a) : (b))

static int type_alignment(struct type *t)
{
	if (t->flags & T_ARRAY && !t->ptr)
		return type_alignment(&arrays[t->id].type);
	if (t->flags & T_STRUCT && !t->ptr)
		return type_alignment(&structs[t->id].fields[0].type);
	return MIN(LONGSZ, type_totsz(t));
}

static void structdef(void *data, struct name *name, unsigned flags)
{
	struct structinfo *si = data;
	if (si->isunion) {
		name->addr = 0;
		if (si->size < type_totsz(&name->type))
			si->size = type_totsz(&name->type);
	} else {
		struct type *t = &name->type;
		int alignment = type_alignment(t);
		if (t->flags & T_ARRAY && !t->ptr)
			alignment = MIN(LONGSZ, type_totsz(&arrays[t->id].type));
		si->size = ALIGN(si->size, alignment);
		name->addr = si->size;
		si->size += type_totsz(&name->type);
	}
	memcpy(&si->fields[si->nfields++], name, sizeof(*name));
}

static int readdefs(void (*def)(void *, struct name *, unsigned f), void *data);

static int struct_create(char *name, int isunion)
{
	int id = struct_find(name, isunion);
	struct structinfo *si = &structs[id];
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
				err("const expr expected!\n");
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
	char name[NAMELEN] = "";
	*flags = 0;
	type->flags = 0;
	type->ptr = 0;
	while (!done) {
		switch (tok_see()) {
		case TOK_STATIC:
			*flags |= F_STATIC;
			break;
		case TOK_EXTERN:
			*flags |= F_EXTERN;
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
			size = LONGSZ;
			break;
		case TOK_SIGNED:
			break;
		case TOK_UNSIGNED:
			sign = 0;
			break;
		case TOK_UNION:
		case TOK_STRUCT:
			isunion = tok_get() == TOK_UNION;
			if (!tok_jmp(TOK_NAME))
				strcpy(name, tok_id());
			if (tok_see() == '{')
				type->id = struct_create(name, isunion);
			else
				type->id = struct_find(name, isunion);
			type->flags |= T_STRUCT;
			type->bt = LONGSZ;
			return 0;
		case TOK_ENUM:
			tok_get();
			tok_jmp(TOK_NAME);
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

static int readname(struct type *main, char *name,
			struct type *base, unsigned flags);

static int readtype(struct type *type)
{
	return readname(type, NULL, NULL, 0);
}

static void readptrs(struct type *type)
{
	while (!tok_jmp('*')) {
		type->ptr++;
		if (!type->bt)
			type->bt = 1;
	}
}

/* used to differenciate labels from case and cond exprs */
static int ncexpr;
static int caseexpr;

static void readpre(void);

static void readprimary(void)
{
	int i;
	if (!tok_jmp(TOK_NUM)) {
		long n;
		int bt = tok_num(&n);
		ts_push_bt(bt);
		o_num(n, bt);
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
		o_symaddr(out_mkdat(NULL, buf, len, 0), TYPE_BT(&t));
		o_addr();
		return;
	}
	if (!tok_jmp(TOK_NAME)) {
		struct name unkn = {0};
		char *name = unkn.name;
		int n;
		strcpy(name, tok_id());
		/* don't search for labels here */
		if (!ncexpr && !caseexpr && tok_see() == ':')
			return;
		for (i = nlocals - 1; i >= 0; --i) {
			struct type *t = &locals[i].type;
			if (!strcmp(locals[i].name, name)) {
				o_local(locals[i].addr, TYPE_BT(t));
				ts_push(t);
				return;
			}
		}
		if ((n = global_find(name)) != -1) {
			struct name *g = &globals[n];
			struct type *t = &g->type;
			if (g->unused) {
				g->unused = 0;
				if (t->flags & T_FUNC && !t->ptr)
					g->addr = out_mkundef(name, 0);
				else
					g->addr = out_mkundef(name, type_totsz(t));
			}
			o_symaddr(g->addr, TYPE_BT(t));
			ts_push(t);
			return;
		}
		if (!enum_find(&n, name)) {
			ts_push_bt(4 | BT_SIGNED);
			o_num(n, 4 | BT_SIGNED);
			return;
		}
		if (tok_see() != '(')
			err("unknown symbol\n");
		unkn.addr = out_mkundef(unkn.name, 0);
		global_add(&unkn);
		ts_push_bt(LONGSZ);
		o_symaddr(unkn.addr, LONGSZ);
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
		o_bop(O_MUL);
	}
	o_bop(O_ADD);
	o_deref(TYPE_BT(t));
}

static void inc_post(int inc_one, int inc_many)
{
	struct type *t = &ts[nts - 1];
	int sz = type_szde(t);
	o_tmpcopy();
	o_load();
	o_tmpswap();
	if (!t->ptr || sz == 1) {
		o_uop(inc_one);
	} else {
		o_tmpcopy();
		o_num(sz, 4);
		o_bop(inc_many);
		o_assign(TYPE_BT(t));
	}
	o_tmpdrop(1);
}

static void readfield(void)
{
	struct name *field;
	struct type t;
	tok_expect(TOK_NAME);
	ts_pop(&t);
	array2ptr(&t);
	field = struct_field(t.id, tok_id());
	if (field->addr) {
		o_num(field->addr, 4);
		o_bop(O_ADD);
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
	if (nfuncs >= MAXFUNCS)
		err("nomem: MAXFUNCS reached!\n");
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
	ts_pop(&t);
	if (t.flags & T_FUNC && t.ptr > 0)
		o_deref(LONGSZ);
	fi = t.flags & T_FUNC ? &funcs[t.id] : NULL;
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
			inc_post(O_INC, O_ADD);
			continue;
		}
		if (!tok_jmp(TOK2("--"))) {
			inc_post(O_DEC, O_SUB);
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

static void inc_pre(int inc_one, int inc_many)
{
	struct type *t;
	int sz;
	readpre();
	t = &ts[nts - 1];
	sz = (t->flags & T_ARRAY || t->ptr) ? type_szde(t) : 1;
	if (sz == 1) {
		o_uop(inc_one);
	} else {
		o_tmpcopy();
		o_num(sz, 4);
		o_bop(inc_many);
		o_assign(TYPE_BT(t));
	}
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
		array2ptr(&t);
		t.ptr--;
		o_deref(TYPE_BT(&t));
		ts_push(&t);
		return;
	}
	if (!tok_jmp('!')) {
		struct type type;
		readpre();
		ts_pop(&type);
		o_uop(O_LNOT);
		ts_push_bt(4 | BT_SIGNED);
		return;
	}
	if (!tok_jmp('-')) {
		readpre();
		o_uop(O_NEG);
		return;
	}
	if (!tok_jmp('~')) {
		readpre();
		o_uop(O_NOT);
		return;
	}
	if (!tok_jmp(TOK2("++"))) {
		inc_pre(O_INC, O_ADD);
		return;
	}
	if (!tok_jmp(TOK2("--"))) {
		inc_pre(O_DEC, O_SUB);
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
			ts_binop(O_MUL);
			continue;
		}
		if (!tok_jmp('/')) {
			readpre();
			ts_binop(O_DIV);
			continue;
		}
		if (!tok_jmp('%')) {
			readpre();
			ts_binop(O_MOD);
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
			ts_addop(O_ADD);
			continue;
		}
		if (!tok_jmp('-')) {
			readmul();
			ts_addop(O_SUB);
			continue;
		}
		break;
	}
}

static void shift(int op)
{
	struct type t;
	readadd();
	ts_pop(NULL);
	ts_pop(&t);
	o_bop(op);
	ts_push_bt(TYPE_BT(&t));
}

static void readshift(void)
{
	readadd();
	while (1) {
		if (!tok_jmp(TOK2("<<"))) {
			shift(O_SHL);
			continue;
		}
		if (!tok_jmp(TOK2(">>"))) {
			shift(O_SHR);
			continue;
		}
		break;
	}
}

static void cmp(int op)
{
	readshift();
	ts_pop(NULL);
	ts_pop(NULL);
	o_bop(op);
	ts_push_bt(4 | BT_SIGNED);
}

static void readcmp(void)
{
	readshift();
	while (1) {
		if (!tok_jmp('<')) {
			cmp(O_LT);
			continue;
		}
		if (!tok_jmp('>')) {
			cmp(O_GT);
			continue;
		}
		if (!tok_jmp(TOK2("<="))) {
			cmp(O_LE);
			continue;
		}
		if (!tok_jmp(TOK2(">="))) {
			cmp(O_GE);
			continue;
		}
		break;
	}
}

static void eq(int op)
{
	readcmp();
	ts_pop(NULL);
	ts_pop(NULL);
	o_bop(op);
	ts_push_bt(4 | BT_SIGNED);
}

static void readeq(void)
{
	readcmp();
	while (1) {
		if (!tok_jmp(TOK2("=="))) {
			eq(O_EQ);
			continue;
		}
		if (!tok_jmp(TOK2("!="))) {
			eq(O_NEQ);
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
		ts_binop(O_AND);
	}
}

static void readxor(void)
{
	readbitand();
	while (!tok_jmp('^')) {
		readbitand();
		ts_binop(O_XOR);
	}
}

static void readbitor(void)
{
	readxor();
	while (!tok_jmp('|')) {
		readxor();
		ts_binop(O_OR);
	}
}

#define MAXCOND			(1 << 7)

static void readand(void)
{
	long conds[MAXCOND];
	int nconds = 0;
	long passed;
	int i;
	readbitor();
	if (tok_see() != TOK2("&&"))
		return;
	o_fork();
	conds[nconds++] = o_jz(0);
	ts_pop(NULL);
	while (!tok_jmp(TOK2("&&"))) {
		readbitor();
		conds[nconds++] = o_jz(0);
		ts_pop(NULL);
	}
	o_num(1, 4 | BT_SIGNED);
	o_forkpush();
	passed = o_jmp(0);
	for (i = 0; i < nconds; i++)
		o_filljmp(conds[i]);
	o_num(0, 4 | BT_SIGNED);
	o_forkpush();
	o_forkjoin();
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
	o_fork();
	conds[nconds++] = o_jnz(0);
	ts_pop(NULL);
	while (!tok_jmp(TOK2("||"))) {
		readand();
		conds[nconds++] = o_jnz(0);
		ts_pop(NULL);
	}
	o_num(0, 4 | BT_SIGNED);
	o_forkpush();
	failed = o_jmp(0);
	for (i = 0; i < nconds; i++)
		o_filljmp(conds[i]);
	o_num(1, 4 | BT_SIGNED);
	o_forkpush();
	o_forkjoin();
	o_filljmp(failed);
	ts_push_bt(4 | BT_SIGNED);
}

static int readcexpr_const(void)
{
	long c;
	int nogen;
	if (o_popnum(&c))
		return -1;
	if (!c)
		nogen = !o_nogen();
	reador();
	ts_pop(NULL);
	tok_expect(':');
	if (c) {
		nogen = !o_nogen();
	} else {
		if (nogen)
			o_dogen();
		o_tmpdrop(1);
	}
	reador();
	if (c) {
		if (nogen)
			o_dogen();
		o_tmpdrop(1);
	}
	return 0;
}

static void readcexpr(void)
{
	long l1, l2;
	reador();
	if (tok_jmp('?'))
		return;
	ncexpr++;
	ts_pop(NULL);
	o_fork();
	if (readcexpr_const()) {
		l1 = o_jz(0);
		reador();
		o_forkpush();
		l2 = o_jmp(0);
		ts_pop(NULL);

		tok_expect(':');
		o_filljmp(l1);
		reador();
		o_forkpush();
		o_forkjoin();
		o_filljmp(l2);
	}
	ncexpr--;
}

static void opassign(int op, int ptrop)
{
	struct type *t = &ts[nts - 1];
	if (ptrop && (t->flags & T_ARRAY || t->ptr)) {
		o_tmpcopy();
		readexpr();
		ts_addop(op);
		o_assign(TYPE_BT(&ts[nts - 1]));
		ts_pop(NULL);
	} else {
		readexpr();
		o_bop(op | O_SET);
		ts_pop(NULL);
	}
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
		opassign(O_ADD, 1);
		return;
	}
	if (!tok_jmp(TOK2("-="))) {
		opassign(O_SUB, 1);
		return;
	}
	if (!tok_jmp(TOK2("*="))) {
		opassign(O_MUL, 0);
		return;
	}
	if (!tok_jmp(TOK2("/="))) {
		opassign(O_DIV, 0);
		return;
	}
	if (!tok_jmp(TOK2("%="))) {
		opassign(O_MOD, 0);
		return;
	}
	if (!tok_jmp(TOK3("<<="))) {
		opassign(O_SHL, 0);
		return;
	}
	if (!tok_jmp(TOK3(">>="))) {
		opassign(O_SHR, 0);
		return;
	}
	if (!tok_jmp(TOK3("&="))) {
		opassign(O_AND, 0);
		return;
	}
	if (!tok_jmp(TOK3("|="))) {
		opassign(O_OR, 0);
		return;
	}
	if (!tok_jmp(TOK3("^="))) {
		opassign(O_XOR, 0);
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

static void o_localoff(long addr, int off, unsigned bt)
{
	o_local(addr, bt);
	if (off) {
		o_addr();
		o_num(off, 4);
		o_bop(O_ADD);
		o_deref(bt);
	}
}

static struct type *innertype(struct type *t)
{
	if (t->flags & T_ARRAY && !t->ptr)
		return innertype(&arrays[t->id].type);
	return t;
}

static void initexpr(struct type *t, int off, void *obj,
		void (*set)(void *obj, int off, struct type *t))
{
	if (tok_jmp('{')) {
		set(obj, off, t);
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
			initexpr(&field->type, off + field->addr, obj, set);
			if (tok_jmp(',') || tok_see() == '}')
				break;
		}
	} else if (t->flags & T_ARRAY) {
		struct type *t_de = &arrays[t->id].type;
		int i;
		for (i = 0; ; i++) {
			long idx = i;
			struct type *it = t_de;
			if (!tok_jmp('[')) {
				readexpr();
				o_popnum(&idx);
				ts_pop(NULL);
				tok_expect(']');
				tok_expect('=');
			}
			if (tok_see() != '{')
				it = innertype(t_de);
			initexpr(it, off + type_totsz(it) * idx, obj, set);
			if (tok_jmp(',') || tok_see() == '}')
				break;
		}
	}
	tok_expect('}');
}

static void jumpbrace(void)
{
	int depth = 0;
	while (tok_see() != '}' || depth--)
		if (tok_get() == '{')
			depth++;
	tok_expect('}');
}

static int initsize(void)
{
	long addr = tok_addr();
	int n = 0;
	if (!tok_jmp(TOK_STR)) {
		n = tok_str(NULL);
		tok_jump(addr);
		return n;
	}
	o_nogen();
	tok_expect('{');
	while (tok_jmp('}')) {
		long idx = n;
		if (!tok_jmp('[')) {
			readexpr();
			o_popnum(&idx);
			ts_pop(NULL);
			tok_expect(']');
			tok_expect('=');
		}
		if (n < idx + 1)
			n = idx + 1;
		while (tok_see() != '}' && tok_see() != ',')
			if (tok_get() == '{')
				jumpbrace();
		tok_jmp(',');
	}
	o_dogen();
	tok_jump(addr);
	return n;
}

#define F_GLOBAL(flags)		(!((flags) & F_STATIC))

static void globalinit(void *obj, int off, struct type *t)
{
	long addr = *(long *) obj;
	if (t->flags & T_ARRAY && tok_see() == TOK_STR) {
		struct type *t_de = &arrays[t->id].type;
		if (!t_de->ptr && !t_de->flags && TYPE_SZ(t_de) == 1) {
			char buf[BUFSIZE];
			int len;
			tok_expect(TOK_STR);
			len = tok_str(buf);
			out_datcpy(addr, off, buf, len);
			return;
		}
	}
	readexpr();
	o_datset(addr, off, TYPE_BT(t));
	ts_pop(NULL);
}

static void globaldef(void *data, struct name *name, unsigned flags)
{
	struct type *t = &name->type;
	char *varname = flags & F_STATIC ? NULL : name->name;
	int sz;
	if (t->flags & T_ARRAY && !t->ptr && !arrays[t->id].n)
		arrays[t->id].n = initsize();
	sz = type_totsz(t);
	if (flags & F_EXTERN || t->flags & T_FUNC && !t->ptr)
		name->unused = 1;
	else if (flags & F_INIT)
		name->addr = out_mkdat(varname, NULL, sz, F_GLOBAL(flags));
	else
		name->addr = out_mkvar(varname, sz, F_GLOBAL(flags));
	global_add(name);
	if (flags & F_INIT)
		initexpr(t, 0, &name->addr, globalinit);
}

static void localinit(void *obj, int off, struct type *t)
{
	long addr = *(long *) obj;
	if (t->flags & T_ARRAY && tok_see() == TOK_STR) {
		struct type *t_de = &arrays[t->id].type;
		if (!t_de->ptr && !t_de->flags && TYPE_SZ(t_de) == 1) {
			char buf[BUFSIZE];
			int len;
			tok_expect(TOK_STR);
			len = tok_str(buf);
			o_localoff(addr, off, TYPE_BT(t));
			o_symaddr(out_mkdat(NULL, buf, len, 0), TYPE_BT(t));
			o_memcpy(len);
			o_tmpdrop(1);
			return;
		}
	}
	o_localoff(addr, off, TYPE_BT(t));
	ts_push(t);
	readexpr();
	doassign();
	ts_pop(NULL);
	o_tmpdrop(1);
}

static void localdef(void *data, struct name *name, unsigned flags)
{
	struct type *t = &name->type;
	if (flags & (F_STATIC | F_EXTERN)) {
		globaldef(data, name, flags);
		return;
	}
	if (t->flags & T_ARRAY && !t->ptr && !arrays[t->id].n)
		arrays[t->id].n = initsize();
	name->addr = o_mklocal(type_totsz(&name->type));
	local_add(name);
	if (flags & F_INIT) {
		if (t->flags & (T_ARRAY | T_STRUCT) && !t->ptr) {
			o_local(name->addr, TYPE_BT(t));
			o_memset(0, type_totsz(t));
			o_tmpdrop(1);
		}
		initexpr(t, 0, &name->addr, localinit);
	}
}

static void funcdef(char *name, struct type *type, struct name *args,
			int nargs, unsigned flags)
{
	struct name global;
	int i;
	strcpy(global.name, name);
	memcpy(&global.type, type, sizeof(*type));
	global.addr = o_func_beg(name, F_GLOBAL(flags));
	global.unused = 0;
	global_add(&global);
	ret_bt = TYPE_BT(&funcs[type->id].ret);
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
		if (!tok_jmp(TOK3("...")))
			break;
		readname(&args[nargs].type, args[nargs].name, NULL, 0);
		array2ptr(&args[nargs].type);
		nargs++;
		if (tok_jmp(','))
			break;
	}
	tok_expect(')');
	if (nargs == 1 && !TYPE_BT(&args[0].type))
		return 0;
	return nargs;
}

static int readname(struct type *main, char *name,
			struct type *base, unsigned flags)
{
	struct type tpool[3];
	int npool = 0;
	struct type *type = &tpool[npool++];
	struct type *func = NULL;
	struct type *ret = NULL;
	int arsz[10];
	int nar = 0;
	int i;
	memset(tpool, 0, sizeof(tpool));
	if (name)
		*name = '\0';
	if (!base) {
		if (basetype(type, &flags))
			return 1;
	} else {
		memcpy(type, base, sizeof(*base));
	}
	readptrs(type);
	if (!tok_jmp('(')) {
		ret = type;
		type = &tpool[npool++];
		func = type;
		readptrs(type);
	}
	if (!tok_jmp(TOK_NAME) && name)
		strcpy(name, tok_id());
	while (!tok_jmp('[')) {
		long n = 0;
		if (tok_jmp(']')) {
			readexpr();
			ts_pop(NULL);
			if (o_popnum(&n))
				err("const expr expected\n");
			tok_expect(']');
		}
		arsz[nar++] = n;
	}
	for (i = nar - 1; i >= 0; i--) {
		type->id = array_add(type, arsz[i]);
		if (func && i == nar - 1)
			func = &arrays[type->id].type;
		type->flags = T_ARRAY;
		type->bt = LONGSZ;
		type->ptr = 0;
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
		func->bt = LONGSZ;
		func->id = func_create(ret, args, nargs);
		if (fdef && tok_see() == '{') {
			funcdef(name, func, args, nargs, flags);
			return 1;
		}
	}
	memcpy(main, type, sizeof(*type));
	return 0;
}

static int readdefs(void (*def)(void *data, struct name *name, unsigned flags),
			void *data)
{
	struct type base;
	unsigned flags;
	if (basetype(&base, &flags))
		return 1;
	while (tok_see() != ';' && tok_see() != '{') {
		struct name name;
		name.unused = 0;
		if (readname(&name.type, name.name, &base, flags))
			break;
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

static void readstmt(void);

#define MAXCASES		(1 << 7)

static void readswitch(void)
{
	int break_beg = nbreaks;
	long val_addr = o_mklocal(LONGSZ);
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
				caseexpr = 1;
				readexpr();
				caseexpr = 0;
				o_local(val_addr, TYPE_BT(&t));
				o_bop(O_EQ);
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
	o_rmlocal(val_addr, LONGSZ);
	if (!ref++)
		o_filljmp(next);
	break_fill(o_mklabel(), break_beg);
}

#define MAXGOTO			(1 << 10)

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
		long l_check, l_jump, j_fail, j_pass;
		int break_beg = nbreaks;
		int continue_beg = ncontinues;
		int has_cond = 0;
		tok_expect('(');
		if (tok_see() != ';')
			readestmt();
		tok_expect(';');
		l_check = o_mklabel();
		if (tok_see() != ';') {
			readestmt();
			j_fail = o_jz(0);
			has_cond = 1;
		}
		tok_expect(';');
		j_pass = o_jmp(0);
		l_jump = o_mklabel();
		if (tok_see() != ')')
			readestmt();
		tok_expect(')');
		o_jmp(l_check);
		o_filljmp(j_pass);
		readstmt();
		o_jmp(l_jump);
		if (has_cond)
			o_filljmp(j_fail);
		break_fill(o_mklabel(), break_beg);
		continue_fill(l_jump, continue_beg);
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
	int ofd;
	int i = 1;
	while (i < argc && argv[i][0] == '-') {
		if (argv[i][1] == 'I')
			cpp_addpath(argv[i][2] ? argv[i] + 2 : argv[++i]);
		if (argv[i][1] == 'D') {
			char *name = argv[i] + 2;
			char *def = "";
			char *eq = strchr(name, '=');
			if (eq) {
				*eq = '\0';
				def = eq + 1;
			}
			cpp_define(name, def);
		}
		i++;
	}
	if (i == argc)
		die("neatcc: no file given\n");
	if (cpp_init(argv[i]))
		die("neatcc: cannot open input file\n");
	parse();
	strcpy(obj, argv[i]);
	obj[strlen(obj) - 1] = 'o';
	ofd = open(obj, O_WRONLY | O_TRUNC | O_CREAT, 0600);
	out_write(ofd);
	close(ofd);
	return 0;
}
