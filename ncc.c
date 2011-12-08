/*
 * neatcc - a small and simple C compiler
 *
 * Copyright (C) 2010-2011 Ali Gholami Rudi
 *
 * This program is released under GNU GPL version 2.
 */
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "gen.h"
#include "tok.h"
#include "out.h"

static int nogen;		/* don't generate code */
/* nogen macros */
#define o_bop(op)		{if (!nogen) o_bop(op);}
#define o_uop(op)		{if (!nogen) o_uop(op);}
#define o_cast(bt)		{if (!nogen) o_cast(bt);}
#define o_memcpy()		{if (!nogen) o_memcpy();}
#define o_memset()		{if (!nogen) o_memset();}
#define o_call(argc, ret)	{if (!nogen) o_call(argc, ret);}
#define o_ret(ret)		{if (!nogen) o_ret(ret);}
#define o_assign(bt)		{if (!nogen) o_assign(bt);}
#define o_deref(bt)		{if (!nogen) o_deref(bt);}
#define o_load()		{if (!nogen) o_load();}
#define o_popnum(c)		(nogen ? 0 : o_popnum(c))
#define o_num(n)		{if (!nogen) o_num(n);}
#define o_local(addr)		{if (!nogen) o_local(addr);}
#define o_sym(sym)		{if (!nogen) o_sym(sym);}
#define o_tmpdrop(n)		{if (!nogen) o_tmpdrop(n);}
#define o_tmpswap()		{if (!nogen) o_tmpswap();}
#define o_tmpcopy()		{if (!nogen) o_tmpcopy();}
#define o_mklabel()		(nogen ? 0 : o_mklabel())
#define o_jz(addr)		(nogen ? 0 : o_jz(addr))
#define o_jnz(addr)		(nogen ? 0 : o_jnz(addr))
#define o_jmp(addr)		(nogen ? 0 : o_jmp(addr))
#define o_filljmp(addr)		{if (!nogen) o_filljmp(addr);}
#define o_filljmp2(addr, dst)	{if (!nogen) o_filljmp2(addr, dst);}
#define o_fork()		{if (!nogen) o_fork();}
#define o_forkpush()		{if (!nogen) o_forkpush();}
#define o_forkjoin()		{if (!nogen) o_forkjoin();}

#define MAXLOCALS	(1 << 10)
#define MAXGLOBALS	(1 << 10)
#define MAXARGS		(1 << 5)

#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))
#define MIN(a, b)		((a) < (b) ? (a) : (b))

#define TYPE_BT(t)		((t)->ptr ? LONGSZ : (t)->bt)
#define TYPE_SZ(t)		((t)->ptr ? LONGSZ : (t)->bt & BT_SZMASK)

/* type->flag values */
#define T_ARRAY		0x01
#define T_STRUCT	0x02
#define T_FUNC		0x04

/* variable definition flags */
#define F_STATIC	0x01
#define F_EXTERN	0x02

struct type {
	unsigned bt;
	unsigned flags;
	int ptr;
	int id;		/* for structs, functions and arrays */
	int addr;	/* the address is passed to gen.c; deref for value */
};

/* type stack */
static struct type ts[MAXTMP];
static int nts;

static void ts_push_bt(unsigned bt)
{
	ts[nts].ptr = 0;
	ts[nts].flags = 0;
	ts[nts].addr = 0;
	ts[nts++].bt = bt;
}

static void ts_push(struct type *t)
{
	struct type *d = &ts[nts++];
	memcpy(d, t, sizeof(*t));
}

static void ts_push_addr(struct type *t)
{
	ts_push(t);
	ts[nts - 1].addr = 1;
}

static void ts_pop(struct type *type)
{
	nts--;
	if (type)
		*type = ts[nts];
}

void err(char *msg)
{
	die("%s: %s", cpp_loc(tok_addr()), msg);
}

struct name {
	char name[NAMELEN];
	char elfname[NAMELEN];	/* local elf name for static variables in function */
	struct type type;
	long addr;	/* local stack offset, global data addr, struct offset */
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

static int local_find(char *name)
{
	int i;
	for (i = nlocals - 1; i >= 0; --i)
		if (!strcmp(locals[i].name, name))
			return i;
	return -1;
}

static int global_find(char *name)
{
	int i;
	for (i = nglobals - 1; i >= 0; i--)
		if (!strcmp(name, globals[i].name))
			return i;
	return -1;
}

static void global_add(struct name *name)
{
	if (nglobals >= MAXGLOBALS)
		err("nomem: MAXGLOBALS reached!\n");
	memcpy(&globals[nglobals++], name, sizeof(*name));
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
	if (t->flags & T_ARRAY && !t->ptr) {
		memcpy(t, &arrays[t->id].type, sizeof(*t));
		t->ptr++;
	}
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

/* return t's size */
static int type_totsz(struct type *t)
{
	if (t->ptr)
		return LONGSZ;
	if (t->flags & T_ARRAY)
		return arrays[t->id].n * type_totsz(&arrays[t->id].type);
	return t->flags & T_STRUCT ? structs[t->id].size : BT_SZ(t->bt);
}

/* return t's dereferenced size */
static unsigned type_szde(struct type *t)
{
	struct type de = *t;
	array2ptr(&de);
	de.ptr--;
	return type_totsz(&de);
}

/* dereference stack top if t->addr (ie. address is pushed to gen.c) */
static void ts_de(int deref)
{
	struct type *t = &ts[nts - 1];
	array2ptr(t);
	if (deref && t->addr && (t->ptr || !(t->flags & T_FUNC)))
		o_deref(TYPE_BT(t));
	t->addr = 0;
}

/* pop stack pop to *t and dereference if t->addr */
static void ts_pop_de(struct type *t)
{
	ts_de(1);
	ts_pop(t);
}

/* pop the top 2 stack values and dereference them if t->addr */
static void ts_pop_de2(struct type *t1, struct type *t2)
{
	ts_pop_de(t1);
	o_tmpswap();
	ts_pop_de(t2);
	o_tmpswap();
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
	int bt;
	ts_pop_de2(&t1, &t2);
	if (op == O_DIV || op == O_MOD)
		bt = TYPE_BT(&t2);
	else
		bt = bt_op(TYPE_BT(&t1), TYPE_BT(&t2));
	o_bop(op | (bt & BT_SIGNED ? O_SIGNED : 0));
	ts_push_bt(bt);
}

static void ts_addop(int op)
{
	struct type t1, t2;
	ts_pop_de2(&t1, &t2);
	if (!t1.ptr && !t2.ptr) {
		o_bop(op);
		ts_push_bt(bt_op(TYPE_BT(&t1), TYPE_BT(&t2)));
		return;
	}
	if (t1.ptr && !t2.ptr)
		o_tmpswap();
	if (!t1.ptr && t2.ptr)
		if (type_szde(&t2) > 1) {
			o_num(type_szde(&t2));
			o_bop(O_MUL);
		}
	if (t1.ptr && !t2.ptr)
		o_tmpswap();
	o_bop(op);
	if (t1.ptr && t2.ptr) {
		int sz = type_szde(&t1);
		if (sz > 1) {
			o_num(sz);
			o_bop(O_DIV);
		}
		ts_push_bt(4 | BT_SIGNED);
	} else {
		ts_push(t1.ptr ? &t1 : &t2);
	}
}

/* function prototypes for parsing function and variable declarations */
static int readname(struct type *main, char *name, struct type *base);
static int readtype(struct type *type);
static int readdefs(void (*def)(void *data, struct name *name, unsigned flags),
			void *data);
static int readdefs_int(void (*def)(void *data, struct name *name, unsigned flags),
			void *data);

/* function prototypes for parsing initializer expressions */
static int initsize(void);
static void initexpr(struct type *t, int off, void *obj,
		void (*set)(void *obj, int off, struct type *t));

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
		if (!tok_jmp('=')) {
			readexpr();
			ts_pop_de(NULL);
			if (o_popnum(&n))
				err("const expr expected!\n");
		}
		enum_add(name, n++);
		tok_jmp(',');
	}
}

/* used to differenciate labels from case and cond exprs */
static int ncexpr;
static int caseexpr;

static void readpre(void);

static char *tmp_str(char *buf, int len)
{
	static char name[NAMELEN];
	static int id;
	void *dat;
	sprintf(name, "__neatcc.s%d", id++);
	dat = o_mkdat(name, len, 0);
	memcpy(dat, buf, len);
	return name;
}

static void readprimary(void)
{
	if (!tok_jmp(TOK_NUM)) {
		long n;
		int bt = tok_num(&n);
		ts_push_bt(bt);
		o_num(n);
		return;
	}
	if (!tok_jmp(TOK_STR)) {
		struct type t;
		char buf[BUFSIZE];
		int len;
		t.bt = 1 | BT_SIGNED;
		t.ptr = 1;
		t.addr = 0;
		t.flags = 0;
		ts_push(&t);
		len = tok_str(buf);
		o_sym(tmp_str(buf, len));
		return;
	}
	if (!tok_jmp(TOK_NAME)) {
		struct name unkn = {""};
		char *name = unkn.name;
		int n;
		strcpy(name, tok_id());
		/* don't search for labels here */
		if (!ncexpr && !caseexpr && tok_see() == ':')
			return;
		if ((n = local_find(name)) != -1) {
			struct name *l = &locals[n];
			o_local(l->addr);
			ts_push_addr(&l->type);
			return;
		}
		if ((n = global_find(name)) != -1) {
			struct name *g = &globals[n];
			o_sym(*g->elfname ? g->elfname : g->name);
			ts_push_addr(&g->type);
			return;
		}
		if (!enum_find(&n, name)) {
			ts_push_bt(4 | BT_SIGNED);
			o_num(n);
			return;
		}
		if (tok_see() != '(')
			err("unknown symbol\n");
		global_add(&unkn);
		ts_push_bt(LONGSZ);
		o_sym(unkn.name);
		return;
	}
	if (!tok_jmp('(')) {
		struct type t;
		if (!readtype(&t)) {
			struct type o;
			tok_expect(')');
			readpre();
			ts_pop_de(&o);
			ts_push(&t);
			if (!t.ptr || !o.ptr)
				o_cast(TYPE_BT(&t));
		} else {
			readexpr();
			while (tok_jmp(')')) {
				tok_expect(',');
				ts_pop(NULL);
				o_tmpdrop(1);
				readexpr();
			}
		}
		return;
	}
}

static void arrayderef(void)
{
	struct type t;
	int sz;
	ts_pop_de(NULL);
	ts_pop(&t);
	if (!(t.flags & T_ARRAY && !t.ptr) && t.addr) {
		o_tmpswap();
		o_deref(TYPE_BT(&t));
		o_tmpswap();
	}
	array2ptr(&t);
	t.ptr--;
	sz = type_totsz(&t);
	t.addr = 1;
	if (sz > 1) {
		o_num(sz);
		o_bop(O_MUL);
	}
	o_bop(O_ADD);
	ts_push(&t);
}

static void inc_post(int op)
{
	struct type t = ts[nts - 1];
	/* pushing the value before inc */
	o_tmpcopy();
	ts_de(1);
	o_load();
	o_tmpswap();

	/* increment by 1 or pointer size */
	o_tmpcopy();
	ts_push(&t);
	ts_pop_de(&t);
	o_num(t.ptr > 0 ? type_szde(&t) : 1);
	o_bop(op);

	/* assign back */
	o_assign(TYPE_BT(&t));
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
		o_num(field->addr);
		o_bop(O_ADD);
	}
	ts_push_addr(&field->type);
}

#define MAXFUNCS		(1 << 10)

static struct funcinfo {
	struct type args[MAXARGS];
	struct type ret;
	int nargs;
	int varg;
	/* function and argument names; useful only when defining */
	char argnames[MAXARGS][NAMELEN];
	char name[NAMELEN];
} funcs[MAXFUNCS];
static int nfuncs;

static int func_create(struct type *ret, char *name, char argnames[][NAMELEN],
			struct type *args, int nargs, int varg)
{
	struct funcinfo *fi = &funcs[nfuncs++];
	int i;
	if (nfuncs >= MAXFUNCS)
		err("nomem: MAXFUNCS reached!\n");
	memcpy(&fi->ret, ret, sizeof(*ret));
	for (i = 0; i < nargs; i++)
		memcpy(&fi->args[i], &args[i], sizeof(*ret));
	fi->nargs = nargs;
	fi->varg = varg;
	strcpy(fi->name, name);
	for (i = 0; i < nargs; i++)
		strcpy(fi->argnames[i], argnames[i]);
	return fi - funcs;
}

static void readcall(void)
{
	struct type t;
	struct funcinfo *fi;
	int argc = 0;
	ts_pop(&t);
	if (t.flags & T_FUNC && t.ptr > 0)
		o_deref(LONGSZ);
	fi = t.flags & T_FUNC ? &funcs[t.id] : NULL;
	if (tok_see() != ')') {
		do {
			readexpr();
			ts_pop_de(NULL);
			argc++;
		} while (!tok_jmp(','));
	}
	tok_expect(')');
	o_call(argc, fi ? TYPE_BT(&fi->ret) : 4 | BT_SIGNED);
	if (fi) {
		if (TYPE_BT(&fi->ret))
			o_cast(TYPE_BT(&fi->ret));
		ts_push(&fi->ret);
	} else {
		ts_push_bt(4 | BT_SIGNED);
	}
}

static void readpost(void)
{
	readprimary();
	while (1) {
		if (!tok_jmp('[')) {
			readexpr();
			tok_expect(']');
			arrayderef();
			continue;
		}
		if (!tok_jmp('(')) {
			readcall();
			continue;
		}
		if (!tok_jmp(TOK2("++"))) {
			inc_post(O_ADD);
			continue;
		}
		if (!tok_jmp(TOK2("--"))) {
			inc_post(O_SUB);
			continue;
		}
		if (!tok_jmp('.')) {
			readfield();
			continue;
		}
		if (!tok_jmp(TOK2("->"))) {
			ts_de(1);
			readfield();
			continue;
		}
		break;
	}
}

static void inc_pre(int op)
{
	struct type t;
	readpre();
	/* copy the destination */
	o_tmpcopy();
	ts_push(&ts[nts - 1]);
	/* increment by 1 or pointer size */
	ts_pop_de(&t);
	o_num(t.ptr > 0 ? type_szde(&t) : 1);
	o_bop(op);
	/* assign the result */
	o_assign(TYPE_BT(&t));
	ts_de(0);
}

static void readpre(void)
{
	if (!tok_jmp('&')) {
		struct type type;
		readpre();
		ts_pop(&type);
		if (!type.addr)
			err("cannot use the address\n");
		type.ptr++;
		type.addr = 0;
		ts_push(&type);
		return;
	}
	if (!tok_jmp('*')) {
		struct type t;
		readpre();
		ts_pop(&t);
		array2ptr(&t);
		if (!t.ptr)
			err("dereferencing non-pointer\n");
		if (t.addr)
			o_deref(TYPE_BT(&t));
		t.ptr--;
		t.addr = 1;
		ts_push(&t);
		return;
	}
	if (!tok_jmp('!')) {
		readpre();
		ts_pop_de(NULL);
		o_uop(O_LNOT);
		ts_push_bt(4 | BT_SIGNED);
		return;
	}
	if (!tok_jmp('-')) {
		readpre();
		ts_de(1);
		o_uop(O_NEG);
		return;
	}
	if (!tok_jmp('~')) {
		readpre();
		ts_de(1);
		o_uop(O_NOT);
		return;
	}
	if (!tok_jmp(TOK2("++"))) {
		inc_pre(O_ADD);
		return;
	}
	if (!tok_jmp(TOK2("--"))) {
		inc_pre(O_SUB);
		return;
	}
	if (!tok_jmp(TOK_SIZEOF)) {
		struct type t;
		int op = !tok_jmp('(');
		if (readtype(&t)) {
			nogen++;
			if (op)
				readexpr();
			else
				readpre();
			nogen--;
			ts_pop(&t);
		}
		ts_push_bt(4);
		o_num(type_totsz(&t));
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
	ts_pop_de2(NULL, &t);
	o_bop(op | (BT_SIGNED & TYPE_BT(&t) ? O_SIGNED : 0));
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
	struct type t1, t2;
	int bt;
	readshift();
	ts_pop_de2(&t1, &t2);
	bt = bt_op(TYPE_BT(&t1), TYPE_BT(&t2));
	o_bop(op | (bt & BT_SIGNED ? O_SIGNED : 0));
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
	ts_pop_de2(NULL, NULL);
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
	ts_pop_de(NULL);
	conds[nconds++] = o_jz(0);
	while (!tok_jmp(TOK2("&&"))) {
		readbitor();
		ts_pop_de(NULL);
		conds[nconds++] = o_jz(0);
	}
	o_num(1);
	o_forkpush();
	passed = o_jmp(0);
	for (i = 0; i < nconds; i++)
		o_filljmp(conds[i]);
	o_num(0);
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
	ts_pop_de(NULL);
	conds[nconds++] = o_jnz(0);
	while (!tok_jmp(TOK2("||"))) {
		readand();
		ts_pop_de(NULL);
		conds[nconds++] = o_jnz(0);
	}
	o_num(0);
	o_forkpush();
	failed = o_jmp(0);
	for (i = 0; i < nconds; i++)
		o_filljmp(conds[i]);
	o_num(1);
	o_forkpush();
	o_forkjoin();
	o_filljmp(failed);
	ts_push_bt(4 | BT_SIGNED);
}

static void readcexpr(void);

static int readcexpr_const(void)
{
	long c;
	if (o_popnum(&c))
		return -1;
	if (!c)
		nogen++;
	readcexpr();
	/* both branches yield the same type; so ignore the first */
	ts_pop_de(NULL);
	tok_expect(':');
	if (c)
		nogen++;
	else
		nogen--;
	readcexpr();
	/* making sure t->addr == 0 on both branches */
	ts_de(1);
	if (c)
		nogen--;
	return 0;
}

static void readcexpr(void)
{
	long l1, l2;
	reador();
	if (tok_jmp('?'))
		return;
	ncexpr++;
	ts_pop_de(NULL);
	o_fork();
	if (readcexpr_const()) {
		l1 = o_jz(0);
		readcexpr();
		/* both branches yield the same type; so ignore the first */
		ts_pop_de(NULL);
		o_forkpush();
		l2 = o_jmp(0);

		tok_expect(':');
		o_filljmp(l1);
		readcexpr();
		/* making sure t->addr == 0 on both branches */
		ts_de(1);
		o_forkpush();
		o_forkjoin();
		o_filljmp(l2);
	}
	ncexpr--;
}

static void opassign(int op, int ptrop)
{
	struct type t = ts[nts - 1];
	o_tmpcopy();
	ts_push(&t);
	readexpr();
	ts_addop(op);
	o_assign(TYPE_BT(&ts[nts - 2]));
	ts_pop(NULL);
	ts_de(0);
}

static void doassign(void)
{
	struct type t = ts[nts - 1];
	if (!t.ptr && t.flags & T_STRUCT) {
		ts_pop(NULL);
		o_num(type_totsz(&t));
		o_memcpy();
	} else {
		ts_pop_de(NULL);
		o_assign(TYPE_BT(&ts[nts - 1]));
		ts_de(0);
	}
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

#define F_GLOBAL(flags)		(!((flags) & F_STATIC))

static void globalinit(void *obj, int off, struct type *t)
{
	struct name *name = obj;
	char *elfname = *name->elfname ? name->elfname : name->name;
	if (t->flags & T_ARRAY && tok_see() == TOK_STR) {
		struct type *t_de = &arrays[t->id].type;
		if (!t_de->ptr && !t_de->flags && TYPE_SZ(t_de) == 1) {
			char buf[BUFSIZE];
			int len;
			tok_expect(TOK_STR);
			len = tok_str(buf);
			memcpy((void *) name->addr + off, buf, len);
			return;
		}
	}
	readexpr();
	o_datset(elfname, off, TYPE_BT(t));
	ts_pop(NULL);
}

static void readfunc(struct name *name, int flags);

static void globaldef(void *data, struct name *name, unsigned flags)
{
	struct type *t = &name->type;
	char *elfname = *name->elfname ? name->elfname : name->name;
	int sz;
	if (t->flags & T_ARRAY && !t->ptr && !arrays[t->id].n)
		if (~flags & F_EXTERN)
			arrays[t->id].n = initsize();
	sz = type_totsz(t);
	if (!(flags & F_EXTERN) && (!(t->flags & T_FUNC) || t->ptr)) {
		if (tok_see() == '=')
			name->addr = (long) o_mkdat(elfname, sz, F_GLOBAL(flags));
		else
			o_mkbss(elfname, sz, F_GLOBAL(flags));
	}
	global_add(name);
	if (!tok_jmp('='))
		initexpr(t, 0, name, globalinit);
	if (tok_see() == '{' && name->type.flags & T_FUNC)
		readfunc(name, flags);
}

/* generate the address of local + off */
static void o_localoff(long addr, int off)
{
	o_local(addr);
	if (off) {
		o_num(off);
		o_bop(O_ADD);
	}
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
			o_localoff(addr, off);
			o_sym(tmp_str(buf, len));
			o_num(len);
			o_memcpy();
			o_tmpdrop(1);
			return;
		}
	}
	o_localoff(addr, off);
	ts_push(t);
	readexpr();
	doassign();
	ts_pop(NULL);
	o_tmpdrop(1);
}

/* current function name */
static char func_name[NAMELEN];

static void localdef(void *data, struct name *name, unsigned flags)
{
	struct type *t = &name->type;
	if ((flags & F_EXTERN) || (t->flags & T_FUNC) && !t->ptr) {
		global_add(name);
		return;
	}
	if (flags & F_STATIC) {
		sprintf(name->elfname, "__neatcc.%s.%s", func_name, name->name);
		globaldef(data, name, flags);
		return;
	}
	if (t->flags & T_ARRAY && !t->ptr && !arrays[t->id].n)
		arrays[t->id].n = initsize();
	name->addr = o_mklocal(type_totsz(&name->type));
	local_add(name);
	if (!tok_jmp('=')) {
		if (t->flags & (T_ARRAY | T_STRUCT) && !t->ptr) {
			o_local(name->addr);
			o_num(0);
			o_num(type_totsz(t));
			o_memset();
			o_tmpdrop(1);
		}
		initexpr(t, 0, &name->addr, localinit);
	}
}

/* read K&R function arguments */
void krdef(void *data, struct name *name, unsigned flags)
{
	struct funcinfo *fi = data;
	int i;
	for (i = 0; i < fi->nargs; i++)
		if (!strcmp(fi->argnames[i], name->name))
			memcpy(&fi->args[i], &name->type, sizeof(name->type));
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
	struct type t;
	int ncases = 0;		/* number of case labels */
	long last_failed = -1;	/* address of last failed jmp */
	long last_matched = -1;	/* address of last walk through jmp */
	long default_addr = -1;	/* address of the default label */
	tok_expect('(');
	readexpr();
	ts_pop_de(&t);
	o_local(val_addr);
	o_tmpswap();
	o_assign(TYPE_BT(&t));
	ts_de(0);
	o_tmpdrop(1);
	tok_expect(')');
	tok_expect('{');
	while (tok_jmp('}')) {
		if (tok_see() != TOK_CASE && tok_see() != TOK_DEFAULT) {
			readstmt();
			continue;
		}
		if (ncases)
			last_matched = o_jmp(0);
		if (tok_get() == TOK_CASE) {
			if (last_failed >= 0)
				o_filljmp(last_failed);
			caseexpr = 1;
			readexpr();
			ts_pop_de(NULL);
			caseexpr = 0;
			o_local(val_addr);
			o_deref(TYPE_BT(&t));
			o_bop(O_EQ);
			last_failed = o_jz(0);
			o_tmpdrop(1);
		} else {
			if (!ncases)
				last_failed = o_jmp(0);
			default_addr = o_mklabel();
		}
		tok_expect(':');
		if (last_matched >= 0)
			o_filljmp(last_matched);
		ncases++;
	}
	o_rmlocal(val_addr, LONGSZ);
	if (last_failed >= 0)
		o_filljmp2(last_failed,
			default_addr >= 0 ? default_addr : o_mklabel());
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
		ts_pop_de(NULL);
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
		ts_pop_de(NULL);
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
		ts_pop_de(NULL);
		o_jnz(l1);
		tok_expect(')');
		break_fill(o_mklabel(), break_beg);
		continue_fill(l2, continue_beg);
		tok_expect(';');
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
			ts_pop_de(NULL);
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
		if (ret) {
			readexpr();
			ts_pop_de(NULL);
		}
		tok_expect(';');
		o_ret(ret);
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

static void readfunc(struct name *name, int flags)
{
	struct funcinfo *fi = &funcs[name->type.id];
	int i;
	strcpy(func_name, fi->name);
	o_func_beg(func_name, fi->nargs, F_GLOBAL(flags), fi->varg);
	for (i = 0; i < fi->nargs; i++) {
		struct name arg = {"", "", fi->args[i], o_arg2loc(i)};
		strcpy(arg.name, fi->argnames[i]);
		local_add(&arg);
	}
	readstmt();
	goto_fill();
	o_func_end();
	func_name[0] = '\0';
	nlocals = 0;
	ngotos = 0;
	nlabels = 0;
}

static void readdecl(void)
{
	if (!tok_jmp(TOK_TYPEDEF)) {
		readdefs(typedefdef, NULL);
		tok_expect(';');
		return;
	}
	readdefs_int(globaldef, NULL);
	tok_jmp(';');
}

static void parse(void)
{
	while (tok_see() != TOK_EOF)
		readdecl();
}

static void compat_macros(void)
{
	cpp_define("__STDC__", "");
	cpp_define("__linux__", "");
#ifdef NEATCC_ARM
	cpp_define("__arm__", "");
#else
	cpp_define("__i386__", "");
#endif

	/* ignored keywords */
	cpp_define("const", "");
	cpp_define("register", "");
	cpp_define("volatile", "");
	cpp_define("inline", "");
	cpp_define("restrict", "");
	cpp_define("__inline__", "");
	cpp_define("__restrict__", "");
	cpp_define("__attribute__(x)", "");
	cpp_define("__builtin_va_list__", "long");
}

int main(int argc, char *argv[])
{
	char obj[128] = "";
	int ofd;
	int i = 1;
	compat_macros();
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
		if (argv[i][1] == 'o')
			strcpy(obj, argv[i][2] ? argv[i] + 2 : argv[++i]);
		i++;
	}
	if (i == argc)
		die("neatcc: no file given\n");
	if (cpp_init(argv[i]))
		die("neatcc: cannot open <%s>\n", argv[i]);
	parse();
	if (!*obj) {
		strcpy(obj, argv[i]);
		obj[strlen(obj) - 1] = 'o';
	}
	ofd = open(obj, O_WRONLY | O_TRUNC | O_CREAT, 0600);
	o_write(ofd);
	close(ofd);
	return 0;
}


/* parsing function and variable declarations */

/* read the base type of a variable */
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
	type->addr = 0;
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

static void readptrs(struct type *type)
{
	while (!tok_jmp('*')) {
		type->ptr++;
		if (!type->bt)
			type->bt = 1;
	}
}

/* read function arguments */
static int readargs(struct type *args, char argnames[][NAMELEN], int *varg)
{
	int nargs = 0;
	tok_expect('(');
	*varg = 0;
	while (tok_see() != ')') {
		if (!tok_jmp(TOK3("..."))) {
			*varg = 1;
			break;
		}
		if (readname(&args[nargs], argnames[nargs], NULL)) {
			/* argument has no type, assume int */
			tok_expect(TOK_NAME);
			memset(&args[nargs], 0, sizeof(struct type));
			args[nargs].bt = 4 | BT_SIGNED;
			strcpy(argnames[nargs], tok_id());
		}
		/* argument arrays are pointers */
		array2ptr(&args[nargs]);
		nargs++;
		if (tok_jmp(','))
			break;
	}
	tok_expect(')');
	/* void argument */
	if (nargs == 1 && !TYPE_BT(&args[0]))
		return 0;
	return nargs;
}

/*
 * readarrays() parses array specifiers when reading a definition in
 * readname().  The "type" parameter contains the type contained in the
 * inner array; for instance, type in "int *a[10][20]" would be an int
 * pointer.  When returning, the "type" parameter is changed to point
 * to the final array.  The function returns a pointer to the type in
 * the inner array; this is useful when the type is not complete yet,
 * like when creating an array of function pointers as in
 * "int (*f[10])(int)".  If there is no array brackets, NULL is returned.
 */
static struct type *readarrays(struct type *type)
{
	long arsz[16];
	struct type *inner = NULL;
	int nar = 0;
	int i;
	while (!tok_jmp('[')) {
		long n = 0;
		if (tok_jmp(']')) {
			readexpr();
			ts_pop_de(NULL);
			if (o_popnum(&n))
				err("const expr expected\n");
			tok_expect(']');
		}
		arsz[nar++] = n;
	}
	for (i = nar - 1; i >= 0; i--) {
		type->id = array_add(type, arsz[i]);
		if (!inner)
			inner = &arrays[type->id].type;
		type->flags = T_ARRAY;
		type->bt = LONGSZ;
		type->ptr = 0;
	}
	return inner;
}

/*
 * readname() reads a variable definition; the name is copied into
 * "name" and the type is copied into "main" argument.  The "base"
 * argument, if not NULL, indicates the base type of the variable.
 * For instance, the base type of "a" and "b" in "int *a, b[10]" is
 * "int".  If NULL, basetype() is called directly to read the base
 * type of the variable.  readname() returns zero, only if the
 * variable can be read.
 */
static int readname(struct type *main, char *name, struct type *base)
{
	struct type tpool[3];
	int npool = 0;
	struct type *type = &tpool[npool++];
	struct type *ptype = NULL;	/* type inside parenthesis */
	struct type *btype = NULL;	/* type before parenthesis */
	struct type *inner;
	unsigned flags;
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
		btype = type;
		type = &tpool[npool++];
		ptype = type;
		readptrs(type);
	}
	if (!tok_jmp(TOK_NAME) && name)
		strcpy(name, tok_id());
	inner = readarrays(type);
	if (ptype && inner)
		ptype = inner;
	if (ptype)
		tok_expect(')');
	if (tok_see() == '(') {
		struct type args[MAXARGS];
		char argnames[MAXARGS][NAMELEN];
		int varg = 0;
		int nargs = readargs(args, argnames, &varg);
		if (!ptype) {
			btype = type;
			type = &tpool[npool++];
			ptype = type;
		}
		ptype->flags = T_FUNC;
		ptype->bt = LONGSZ;
		ptype->id = func_create(btype, name, argnames, args, nargs, varg);
		if (tok_see() != ';')
			while (tok_see() != '{' && !readdefs(krdef, &funcs[ptype->id]))
				tok_expect(';');
	} else {
		if (ptype && readarrays(type))
			array2ptr(type);
	}
	memcpy(main, type, sizeof(*type));
	return 0;
}

static int readtype(struct type *type)
{
	return readname(type, NULL, NULL);
}

/*
 * readdef() reads a variable definitions statement.  The definition
 * statement can appear in anywhere: global variables, function
 * local variables, struct fields, and typedefs.  For each defined
 * variable, def() callback is called with the appropriate name
 * struct and flags; the callback should finish parsing the definition
 * by possibly reading the initializer expression and saving the name
 * struct.
 */
static int readdefs(void (*def)(void *data, struct name *name, unsigned flags),
			void *data)
{
	struct type base;
	unsigned base_flags;
	if (basetype(&base, &base_flags))
		return 1;
	if (tok_see() == ';' || tok_see() == '{')
		return 0;
	do {
		struct name name = {{""}};
		if (readname(&name.type, name.name, &base))
			break;
		def(data, &name, base_flags);
	} while (!tok_jmp(','));
	return 0;
}

/* just like readdefs, but default to int type; for handling K&R functions */
static int readdefs_int(void (*def)(void *data, struct name *name, unsigned flags),
			void *data)
{
	struct type base;
	unsigned flags = 0;
	if (basetype(&base, &flags)) {
		if (tok_see() != TOK_NAME)
			return 1;
		memset(&base, 0, sizeof(base));
		base.bt = 4 | BT_SIGNED;
	}
	if (tok_see() != ';') {
		do {
			struct name name = {{""}};
			if (readname(&name.type, name.name, &base))
				break;
			def(data, &name, flags);
		} while (!tok_jmp(','));
	}
	return 0;
}


/* parsing initializer expressions */

static void jumpbrace(void)
{
	int depth = 0;
	while (tok_see() != '}' || depth--)
		if (tok_get() == '{')
			depth++;
	tok_expect('}');
}

/* compute the size of the initializer expression */
static int initsize(void)
{
	long addr = tok_addr();
	int n = 0;
	if (tok_jmp('='))
		err("array size unspecified");
	if (!tok_jmp(TOK_STR)) {
		n = tok_str(NULL);
		tok_jump(addr);
		return n;
	}
	tok_expect('{');
	while (tok_jmp('}')) {
		long idx = n;
		if (!tok_jmp('[')) {
			readexpr();
			ts_pop_de(NULL);
			o_popnum(&idx);
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
	tok_jump(addr);
	return n;
}

static struct type *innertype(struct type *t)
{
	if (t->flags & T_ARRAY && !t->ptr)
		return innertype(&arrays[t->id].type);
	return t;
}

/* read the initializer expression and initialize basic types using set() cb */
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
		for (i = 0; i < si->nfields && tok_see() != '}'; i++) {
			struct name *field = &si->fields[i];
			if (!tok_jmp('.')) {
				tok_expect(TOK_NAME);
				field = struct_field(t->id, tok_id());
				tok_expect('=');
			}
			initexpr(&field->type, off + field->addr, obj, set);
			if (tok_jmp(','))
				break;
		}
	} else if (t->flags & T_ARRAY) {
		struct type *t_de = &arrays[t->id].type;
		int i;
		/* handling extra braces as in: char s[] = {"sth"} */
		if (TYPE_SZ(t_de) == 1 && tok_see() == TOK_STR) {
			set(obj, off, t);
			tok_expect('}');
			return;
		}
		for (i = 0; tok_see() != '}'; i++) {
			long idx = i;
			struct type *it = t_de;
			if (!tok_jmp('[')) {
				readexpr();
				ts_pop_de(NULL);
				o_popnum(&idx);
				tok_expect(']');
				tok_expect('=');
			}
			if (tok_see() != '{' && (tok_see() != TOK_STR ||
						!(it->flags & T_ARRAY)))
				it = innertype(t_de);
			initexpr(it, off + type_totsz(it) * idx, obj, set);
			if (tok_jmp(','))
				break;
		}
	}
	tok_expect('}');
}
