/*
 * THE NEATCC C COMPILER
 *
 * Copyright (C) 2010-2016 Ali Gholami Rudi
 *
 * This program is released under the Modified BSD license.
 */
/*
 * neatcc parser
 *
 * The parser reads tokens from the tokenizer (tok_*) and calls the
 * appropriate code generation functions (o_*).  The generator
 * maintains a stack of values pushed via, for instance, o_num()
 * and generates the necessary code for the accesses to the items
 * in this stack, like o_bop() for performing a binary operations
 * on the top two items of the stack.  The parser maintains the
 * types of values pushed to the generator stack in its type stack
 * (ts_*).  For instance, for binary operations two types are
 * popped first and the resulting type is pushed to the type stack
 * (ts_binop()).
 *
 */
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "ncc.h"

#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))
#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) < (b) ? (b) : (a))

#define TYPE_BT(t)		((t)->ptr ? ULNG : (t)->bt)
#define TYPE_SZ(t)		((t)->ptr ? ULNG : (t)->bt & BT_SZMASK)
#define TYPE_VOID(t)		(!(t)->bt && !(t)->flags && !(t)->ptr)

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
static struct type ts[NTMPS];
static int nts;

static void ts_push_bt(unsigned bt)
{
	ts[nts].ptr = 0;
	ts[nts].flags = 0;
	ts[nts].addr = 0;
	ts[nts++].bt = bt;
#ifdef NCCWORDCAST
	o_cast(bt);		/* casting to architecture word */
#endif
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

void err(char *fmt, ...)
{
	va_list ap;
	char msg[512];
	va_start(ap, fmt);
	vsprintf(msg, fmt, ap);
	va_end(ap);
	die("%s: %s", cpp_loc(tok_addr()), msg);
}

void *mextend(void *old, long oldsz, long newsz, long memsz)
{
	void *new = malloc(newsz * memsz);
	memcpy(new, old, oldsz * memsz);
	memset(new + oldsz * memsz, 0, (newsz - oldsz) * memsz);
	free(old);
	return new;
}

struct name {
	char name[NAMELEN];
	char elfname[NAMELEN];	/* local elf name for function static variables */
	struct type type;
	long addr;		/* local stack offset, global data addr, struct offset */
};

static struct name locals[NLOCALS];
static int nlocals;
static struct name globals[NGLOBALS];
static int nglobals;

static void local_add(struct name *name)
{
	if (nlocals >= NLOCALS)
		err("nomem: NLOCALS reached!\n");
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
	if (nglobals >= NGLOBALS)
		err("nomem: NGLOBALS reached!\n");
	memcpy(&globals[nglobals++], name, sizeof(*name));
}

#define LABEL()			(++label)

static int label;		/* last used label id */
static int l_break;		/* current break label */
static int l_cont;		/* current continue label */

static struct enumval {
	char name[NAMELEN];
	int n;
} enums[NENUMS];
static int nenums;

static void enum_add(char *name, int val)
{
	struct enumval *ev = &enums[nenums++];
	if (nenums >= NENUMS)
		err("nomem: NENUMS reached!\n");
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

static struct typdefinfo {
	char name[NAMELEN];
	struct type type;
} typedefs[NTYPEDEFS];
static int ntypedefs;

static void typedef_add(char *name, struct type *type)
{
	struct typdefinfo *ti = &typedefs[ntypedefs++];
	if (ntypedefs >= NTYPEDEFS)
		err("nomem: NTYPEDEFS reached!\n");
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

static struct array {
	struct type type;
	int n;
} arrays[NARRAYS];
static int narrays;

static int array_add(struct type *type, int n)
{
	struct array *a = &arrays[narrays++];
	if (narrays >= NARRAYS)
		err("nomem: NARRAYS reached!\n");
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

static struct structinfo {
	char name[NAMELEN];
	struct name fields[NFIELDS];
	int nfields;
	int isunion;
	int size;
} structs[NSTRUCTS];
static int nstructs;

static int struct_find(char *name, int isunion)
{
	int i;
	for (i = nstructs - 1; i >= 0; --i)
		if (*structs[i].name && !strcmp(name, structs[i].name) &&
				structs[i].isunion == isunion)
			return i;
	i = nstructs++;
	if (nstructs >= NSTRUCTS)
		err("nomem: NSTRUCTS reached!\n");
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
	return NULL;
}

/* return t's size */
static int type_totsz(struct type *t)
{
	if (t->ptr)
		return ULNG;
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

/* the previous identifier; to handle labels */
static char tok_previden[NAMELEN];

static char *tok_iden(void)
{
	snprintf(tok_previden, sizeof(tok_previden), "%s", tok_get());
	return tok_previden;
}

static int tok_jmp(char *tok)
{
	if (strcmp(tok, tok_see()))
		return 1;
	tok_get();
	return 0;
}

static int tok_comes(char *tok)
{
	return !strcmp(tok, tok_see());
}

static void tok_req(char *tok)
{
	if (strcmp(tok, tok_get()))
		err("syntax error\n");
}

static int tok_grp(void)
{
	int c = (unsigned char) tok_see()[0];
	if (c == '"')
		return '"';
	if (c == '\'' || isdigit(c))
		return '0';
	if (c == '_' || isalpha(c))
		return 'a';
	return 0;
}

/* the result of a binary operation on variables of type bt1 and bt2 */
static unsigned bt_op(unsigned bt1, unsigned bt2)
{
	int sz = MAX(BT_SZ(bt1), BT_SZ(bt2));
	return ((bt1 | bt2) & BT_SIGNED) | MAX(sz, UINT);
}

/* the result of a unary operation on variables of bt */
static unsigned bt_uop(unsigned bt)
{
	return bt_op(bt, UINT);
}

/* push the result of a binary operation on the type stack */
static void ts_binop(int op)
{
	struct type t1, t2;
	unsigned bt1, bt2, bt;
	ts_pop_de2(&t1, &t2);
	bt1 = TYPE_BT(&t1);
	bt2 = TYPE_BT(&t2);
	bt = bt_op(bt1, bt2);
	if (op == O_DIV || op == O_MOD)
		bt = BT(bt2 & BT_SIGNED, bt);
	o_bop(op | (bt & BT_SIGNED ? O_SIGNED : 0));
	ts_push_bt(bt);
}

/* push the result of an additive binary operation on the type stack */
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
		ts_push_bt(SLNG);
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
	return MIN(ULNG, type_totsz(t));
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
			alignment = MIN(ULNG, type_totsz(&arrays[t->id].type));
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
	tok_req("{");
	while (tok_jmp("}")) {
		readdefs(structdef, si);
		tok_req(";");
	}
	return id;
}

static void readexpr(void);

static void enum_create(void)
{
	long n = 0;
	tok_req("{");
	while (tok_jmp("}")) {
		char name[NAMELEN];
		strcpy(name, tok_get());
		if (!tok_jmp("=")) {
			readexpr();
			ts_pop_de(NULL);
			if (o_popnum(&n))
				err("const expr expected!\n");
		}
		enum_add(name, n++);
		tok_jmp(",");
	}
}

/* used to differentiate labels from case and cond exprs */
static int ncexpr;
static int caseexpr;

static void readpre(void);

static char *tmp_str(char *buf, int len)
{
	static char name[NAMELEN];
	static int id;
	sprintf(name, "__neatcc.s%d", id++);
	buf[len] = '\0';
	o_dscpy(o_dsnew(name, len + 1, 0), buf, len + 1);
	return name;
}

static void readprimary(void)
{
	if (tok_grp() == '0') {
		long n;
		int bt = tok_num(tok_get(), &n);
		o_num(n);
		ts_push_bt(bt);
		return;
	}
	if (tok_grp() == '"') {
		struct type t = {};	/* char type inside the arrays */
		struct type a = {};	/* the char array type */
		char *buf = tok_get() + 1;
		int len = tok_len() - 2;
		t.bt = 1 | BT_SIGNED;
		a.id = array_add(&t, len + 1);
		a.flags = T_ARRAY;
		o_sym(tmp_str(buf, len));
		ts_push(&a);
		return;
	}
	if (tok_grp() == 'a') {
		struct name unkn = {""};
		char *name = unkn.name;
		int n;
		strcpy(name, tok_iden());
		/* don't search for labels here */
		if (!ncexpr && !caseexpr && tok_comes(":"))
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
			o_num(n);
			ts_push_bt(SINT);
			return;
		}
		if (!tok_comes("("))
			err("unknown symbol <%s>\n", name);
		global_add(&unkn);
		o_sym(unkn.name);
		ts_push_bt(ULNG);
		return;
	}
	if (!tok_jmp("(")) {
		struct type t;
		if (!readtype(&t)) {
			struct type o;
			tok_req(")");
			readpre();
			ts_pop_de(&o);
			ts_push(&t);
			if (!t.ptr || !o.ptr)
				o_cast(TYPE_BT(&t));
		} else {
			readexpr();
			while (tok_jmp(")")) {
				tok_req(",");
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
	ts_pop(&t);
	array2ptr(&t);
	field = struct_field(t.id, tok_get());
	if (field->addr) {
		o_num(field->addr);
		o_bop(O_ADD);
	}
	ts_push_addr(&field->type);
}

static struct funcinfo {
	struct type args[NARGS];
	struct type ret;
	int nargs;
	int varg;
	/* function and argument names; useful only when defining */
	char argnames[NARGS][NAMELEN];
	char name[NAMELEN];
} funcs[NFUNCS];
static int nfuncs;

static int func_create(struct type *ret, char *name, char argnames[][NAMELEN],
			struct type *args, int nargs, int varg)
{
	struct funcinfo *fi = &funcs[nfuncs++];
	int i;
	if (nfuncs >= NFUNCS)
		err("nomem: NFUNCS reached!\n");
	memcpy(&fi->ret, ret, sizeof(*ret));
	for (i = 0; i < nargs; i++)
		memcpy(&fi->args[i], &args[i], sizeof(*ret));
	fi->nargs = nargs;
	fi->varg = varg;
	strcpy(fi->name, name ? name : "");
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
		o_deref(ULNG);
	fi = t.flags & T_FUNC ? &funcs[t.id] : NULL;
	if (!tok_comes(")")) {
		do {
			readexpr();
			ts_pop_de(NULL);
			argc++;
		} while (!tok_jmp(","));
	}
	tok_req(")");
	o_call(argc, fi ? TYPE_BT(&fi->ret) : SINT);
	if (fi) {
		if (TYPE_BT(&fi->ret))
			o_cast(TYPE_BT(&fi->ret));
		ts_push(&fi->ret);
	} else {
		ts_push_bt(SINT);
	}
}

static void readpost(void)
{
	readprimary();
	while (1) {
		if (!tok_jmp("[")) {
			readexpr();
			tok_req("]");
			arrayderef();
			continue;
		}
		if (!tok_jmp("(")) {
			readcall();
			continue;
		}
		if (!tok_jmp("++")) {
			inc_post(O_ADD);
			continue;
		}
		if (!tok_jmp("--")) {
			inc_post(O_SUB);
			continue;
		}
		if (!tok_jmp(".")) {
			readfield();
			continue;
		}
		if (!tok_jmp("->")) {
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
	struct type t;
	if (!tok_jmp("&")) {
		readpre();
		ts_pop(&t);
		if (!t.addr)
			err("cannot use the address\n");
		t.ptr++;
		t.addr = 0;
		ts_push(&t);
		return;
	}
	if (!tok_jmp("*")) {
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
	if (!tok_jmp("!")) {
		readpre();
		ts_pop_de(NULL);
		o_uop(O_LNOT);
		ts_push_bt(SINT);
		return;
	}
	if (!tok_jmp("+")) {
		readpre();
		ts_de(1);
		ts_pop(&t);
		ts_push_bt(bt_uop(TYPE_BT(&t)));
		return;
	}
	if (!tok_jmp("-")) {
		readpre();
		ts_de(1);
		ts_pop(&t);
		o_uop(O_NEG);
		ts_push_bt(bt_uop(TYPE_BT(&t)));
		return;
	}
	if (!tok_jmp("~")) {
		readpre();
		ts_de(1);
		ts_pop(&t);
		o_uop(O_NOT);
		ts_push_bt(bt_uop(TYPE_BT(&t)));
		return;
	}
	if (!tok_jmp("++")) {
		inc_pre(O_ADD);
		return;
	}
	if (!tok_jmp("--")) {
		inc_pre(O_SUB);
		return;
	}
	if (!tok_jmp("sizeof")) {
		struct type t;
		int op = !tok_jmp("(");
		if (readtype(&t)) {
			long m = o_mark();
			if (op)
				readexpr();
			else
				readpre();
			o_back(m);
			ts_pop(&t);
			o_tmpdrop(1);
		}
		o_num(type_totsz(&t));
		ts_push_bt(ULNG);
		if (op)
			tok_req(")");
		return;
	}
	readpost();
}

static void readmul(void)
{
	readpre();
	while (1) {
		if (!tok_jmp("*")) {
			readpre();
			ts_binop(O_MUL);
			continue;
		}
		if (!tok_jmp("/")) {
			readpre();
			ts_binop(O_DIV);
			continue;
		}
		if (!tok_jmp("%")) {
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
		if (!tok_jmp("+")) {
			readmul();
			ts_addop(O_ADD);
			continue;
		}
		if (!tok_jmp("-")) {
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
	ts_push_bt(bt_uop(TYPE_BT(&t)));
}

static void readshift(void)
{
	readadd();
	while (1) {
		if (!tok_jmp("<<")) {
			shift(O_SHL);
			continue;
		}
		if (!tok_jmp(">>")) {
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
	ts_push_bt(SINT);
}

static void readcmp(void)
{
	readshift();
	while (1) {
		if (!tok_jmp("<")) {
			cmp(O_LT);
			continue;
		}
		if (!tok_jmp(">")) {
			cmp(O_GT);
			continue;
		}
		if (!tok_jmp("<=")) {
			cmp(O_LE);
			continue;
		}
		if (!tok_jmp(">=")) {
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
	ts_push_bt(SINT);
}

static void readeq(void)
{
	readcmp();
	while (1) {
		if (!tok_jmp("==")) {
			eq(O_EQ);
			continue;
		}
		if (!tok_jmp("!=")) {
			eq(O_NEQ);
			continue;
		}
		break;
	}
}

static void readbitand(void)
{
	readeq();
	while (!tok_jmp("&")) {
		readeq();
		ts_binop(O_AND);
	}
}

static void readxor(void)
{
	readbitand();
	while (!tok_jmp("^")) {
		readbitand();
		ts_binop(O_XOR);
	}
}

static void readbitor(void)
{
	readxor();
	while (!tok_jmp("|")) {
		readxor();
		ts_binop(O_OR);
	}
}

static void savelocal(long val, int bt)
{
	o_local(val);
	o_tmpswap();
	o_assign(UINT);
	o_tmpdrop(1);
}

static void loadlocal(long val, int bt)
{
	o_local(val);
	o_deref(bt);
	o_rmlocal(val, bt);
}

static void readand(void)
{
	int l_out, l_fail;
	long val;
	readbitor();
	if (!tok_comes("&&"))
		return;
	val = o_mklocal(UINT);
	l_out = LABEL();
	l_fail = LABEL();
	ts_pop_de(NULL);
	o_jz(l_fail);
	while (!tok_jmp("&&")) {
		readbitor();
		ts_pop_de(NULL);
		o_jz(l_fail);
	}
	o_num(1);
	savelocal(val, UINT);
	o_jmp(l_out);
	o_label(l_fail);
	o_num(0);
	savelocal(val, UINT);
	o_label(l_out);
	loadlocal(val, SINT);
	ts_push_bt(SINT);
}

static void reador(void)
{
	int l_pass, l_end;
	long val;
	readand();
	if (!tok_comes("||"))
		return;
	val = o_mklocal(UINT);
	l_pass = LABEL();
	l_end = LABEL();
	ts_pop_de(NULL);
	o_uop(O_LNOT);
	o_jz(l_pass);
	while (!tok_jmp("||")) {
		readand();
		ts_pop_de(NULL);
		o_uop(O_LNOT);
		o_jz(l_pass);
	}
	o_num(0);
	savelocal(val, SINT);
	o_jmp(l_end);
	o_label(l_pass);
	o_num(1);
	savelocal(val, SINT);
	o_label(l_end);
	loadlocal(val, SINT);
	ts_push_bt(SINT);
}

static void readcexpr(void);

static int readcexpr_const(void)
{
	long c, m = 0;
	if (o_popnum(&c))
		return -1;
	if (!c)
		m = o_mark();
	readcexpr();
	/* both branches yield the same type; so ignore the first */
	ts_pop_de(NULL);
	tok_req(":");
	if (!c) {
		o_back(m);
		o_tmpdrop(1);
	}
	if (c)
		m = o_mark();
	readcexpr();
	/* making sure t->addr == 0 on both branches */
	ts_de(1);
	if (c) {
		o_back(m);
		o_tmpdrop(1);
	}
	return 0;
}

static void readcexpr(void)
{
	reador();
	if (tok_jmp("?"))
		return;
	ncexpr++;
	ts_pop_de(NULL);
	if (readcexpr_const()) {
		long val = 0;
		int l_fail = LABEL();
		int l_end = LABEL();
		struct type ret;
		o_jz(l_fail);
		readcexpr();
		/* both branches yield the same type; so ignore the first */
		ts_pop_de(&ret);
		if (!TYPE_VOID(&ret)) {
			val = o_mklocal(ULNG);
			savelocal(val, ULNG);
		}
		o_jmp(l_end);

		tok_req(":");
		o_label(l_fail);
		readcexpr();
		/* making sure t->addr == 0 on both branches */
		ts_de(1);
		if (!TYPE_VOID(&ret)) {
			savelocal(val, ULNG);
		}
		o_label(l_end);
		if (!TYPE_VOID(&ret)) {
			loadlocal(val, ULNG);
		}
	}
	ncexpr--;
}

static void opassign(int op, int ptrop)
{
	struct type t = ts[nts - 1];
	o_tmpcopy();
	ts_push(&t);
	readexpr();
	if (op == O_ADD || op == O_SUB)
		ts_addop(op);
	else
		ts_binop(op);
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
	if (!tok_jmp("=")) {
		readexpr();
		doassign();
		return;
	}
	if (!tok_jmp("+=")) {
		opassign(O_ADD, 1);
		return;
	}
	if (!tok_jmp("-=")) {
		opassign(O_SUB, 1);
		return;
	}
	if (!tok_jmp("*=")) {
		opassign(O_MUL, 0);
		return;
	}
	if (!tok_jmp("/=")) {
		opassign(O_DIV, 0);
		return;
	}
	if (!tok_jmp("%=")) {
		opassign(O_MOD, 0);
		return;
	}
	if (!tok_jmp("<<=")) {
		opassign(O_SHL, 0);
		return;
	}
	if (!tok_jmp(">>=")) {
		opassign(O_SHR, 0);
		return;
	}
	if (!tok_jmp("&=")) {
		opassign(O_AND, 0);
		return;
	}
	if (!tok_jmp("|=")) {
		opassign(O_OR, 0);
		return;
	}
	if (!tok_jmp("^=")) {
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
	} while (!tok_jmp(","));
}

#define F_GLOBAL(flags)		(!((flags) & F_STATIC))

static void globalinit(void *obj, int off, struct type *t)
{
	struct name *name = obj;
	char *elfname = *name->elfname ? name->elfname : name->name;
	if (t->flags & T_ARRAY && tok_grp() == '"') {
		struct type *t_de = &arrays[t->id].type;
		if (!t_de->ptr && !t_de->flags && TYPE_SZ(t_de) == 1) {
			char *buf = tok_get() + 1;
			int len = tok_len() - 2;
			buf[len] = '\0';
			o_dscpy(name->addr + off, buf, len + 1);
			return;
		}
	}
	readexpr();
	o_dsset(elfname, off, TYPE_BT(t));
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
		if (tok_comes("="))
			name->addr = o_dsnew(elfname, sz, F_GLOBAL(flags));
		else
			o_bsnew(elfname, sz, F_GLOBAL(flags));
	}
	global_add(name);
	if (!tok_jmp("="))
		initexpr(t, 0, name, globalinit);
	if (tok_comes("{") && name->type.flags & T_FUNC)
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
	if (t->flags & T_ARRAY && tok_grp() == '"') {
		struct type *t_de = &arrays[t->id].type;
		if (!t_de->ptr && !t_de->flags && TYPE_SZ(t_de) == 1) {
			char *buf = tok_get() + 1;
			int len = tok_len() - 2;
			o_localoff(addr, off);
			o_sym(tmp_str(buf, len));
			o_num(len + 1);
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
	if ((flags & F_EXTERN) || ((t->flags & T_FUNC) && !t->ptr)) {
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
	if (!tok_jmp("=")) {
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

static void typedefdef(void *data, struct name *name, unsigned flags)
{
	typedef_add(name->name, &name->type);
}

static void readstmt(void);

static void readswitch(void)
{
	int o_break = l_break;
	long val_addr = o_mklocal(ULNG);
	struct type t;
	int ncases = 0;			/* number of case labels */
	int l_failed = LABEL();		/* address of last failed jmp */
	int l_matched = LABEL();	/* address of last walk through jmp */
	int l_default = 0;		/* default case label */
	l_break = LABEL();
	tok_req("(");
	readexpr();
	ts_pop_de(&t);
	o_local(val_addr);
	o_tmpswap();
	o_assign(TYPE_BT(&t));
	ts_de(0);
	o_tmpdrop(1);
	tok_req(")");
	tok_req("{");
	while (tok_jmp("}")) {
		if (!tok_comes("case") && !tok_comes("default")) {
			readstmt();
			continue;
		}
		if (ncases)
			o_jmp(l_matched);
		if (!strcmp("case", tok_get())) {
			o_label(l_failed);
			l_failed = LABEL();
			caseexpr = 1;
			readexpr();
			ts_pop_de(NULL);
			caseexpr = 0;
			o_local(val_addr);
			o_deref(TYPE_BT(&t));
			o_bop(O_EQ);
			o_jz(l_failed);
			o_tmpdrop(1);
		} else {
			if (!ncases)
				o_jmp(l_failed);
			l_default = LABEL();
			o_label(l_default);
		}
		tok_req(":");
		o_label(l_matched);
		l_matched = LABEL();
		ncases++;
	}
	o_rmlocal(val_addr, ULNG);
	o_jmp(l_break);
	o_label(l_failed);
	if (l_default)
		o_jmp(l_default);
	o_label(l_break);
	l_break = o_break;
}

static char label_name[NLABELS][NAMELEN];
static int label_ids[NLABELS];
static int nlabels;

static int label_id(char *name)
{
	int i;
	for (i = nlabels - 1; i >= 0; --i)
		if (!strcmp(label_name[i], name))
			return label_ids[i];
	strcpy(label_name[nlabels], name);
	label_ids[nlabels] = LABEL();
	return label_ids[nlabels++];
}

static void readstmt(void)
{
	o_tmpdrop(-1);
	nts = 0;
	if (!tok_jmp("{")) {
		int _nlocals = nlocals;
		int _nglobals = nglobals;
		int _nenums = nenums;
		int _ntypedefs = ntypedefs;
		int _nstructs = nstructs;
		int _nfuncs = nfuncs;
		int _narrays = narrays;
		while (tok_jmp("}"))
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
		tok_req(";");
		return;
	}
	if (!tok_jmp("typedef")) {
		readdefs(typedefdef, NULL);
		tok_req(";");
		return;
	}
	if (!tok_jmp("if")) {
		int l_fail = LABEL();
		int l_end = LABEL();
		tok_req("(");
		readestmt();
		tok_req(")");
		ts_pop_de(NULL);
		o_jz(l_fail);
		readstmt();
		if (!tok_jmp("else")) {
			o_jmp(l_end);
			o_label(l_fail);
			readstmt();
			o_label(l_end);
		} else {
			o_label(l_fail);
		}
		return;
	}
	if (!tok_jmp("while")) {
		int o_break = l_break;
		int o_cont = l_cont;
		l_break = LABEL();
		l_cont = LABEL();
		o_label(l_cont);
		tok_req("(");
		readestmt();
		tok_req(")");
		ts_pop_de(NULL);
		o_jz(l_break);
		readstmt();
		o_jmp(l_cont);
		o_label(l_break);
		l_break = o_break;
		l_cont = o_cont;
		return;
	}
	if (!tok_jmp("do")) {
		int o_break = l_break;
		int o_cont = l_cont;
		int l_beg = LABEL();
		l_break = LABEL();
		l_cont = LABEL();
		o_label(l_beg);
		readstmt();
		tok_req("while");
		tok_req("(");
		o_label(l_cont);
		readexpr();
		ts_pop_de(NULL);
		o_uop(O_LNOT);
		o_jz(l_beg);
		tok_req(")");
		o_label(l_break);
		tok_req(";");
		l_break = o_break;
		l_cont = o_cont;
		return;
	}
	if (!tok_jmp("for")) {
		int o_break = l_break;
		int o_cont = l_cont;
		int l_check = LABEL();	/* for condition label */
		int l_body = LABEL();	/* for block label */
		l_cont = LABEL();
		l_break = LABEL();
		tok_req("(");
		if (!tok_comes(";"))
			readestmt();
		tok_req(";");
		o_label(l_check);
		if (!tok_comes(";")) {
			readestmt();
			ts_pop_de(NULL);
			o_jz(l_break);
		}
		tok_req(";");
		o_jmp(l_body);
		o_label(l_cont);
		if (!tok_comes(")"))
			readestmt();
		tok_req(")");
		o_jmp(l_check);
		o_label(l_body);
		readstmt();
		o_jmp(l_cont);
		o_label(l_break);
		l_break = o_break;
		l_cont = o_cont;
		return;
	}
	if (!tok_jmp("switch")) {
		readswitch();
		return;
	}
	if (!tok_jmp("return")) {
		int ret = !tok_comes(";");
		if (ret) {
			readexpr();
			ts_pop_de(NULL);
		}
		tok_req(";");
		o_ret(ret);
		return;
	}
	if (!tok_jmp("break")) {
		tok_req(";");
		o_jmp(l_break);
		return;
	}
	if (!tok_jmp("continue")) {
		tok_req(";");
		o_jmp(l_cont);
		return;
	}
	if (!tok_jmp("goto")) {
		o_jmp(label_id(tok_get()));
		tok_req(";");
		return;
	}
	readestmt();
	/* labels */
	if (!tok_jmp(":")) {
		o_label(label_id(tok_previden));
		return;
	}
	tok_req(";");
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
	label = 0;
	nlabels = 0;
	readstmt();
	o_func_end();
	func_name[0] = '\0';
	nlocals = 0;
}

static void readdecl(void)
{
	if (!tok_jmp("typedef")) {
		readdefs(typedefdef, NULL);
		tok_req(";");
		return;
	}
	readdefs_int(globaldef, NULL);
	tok_jmp(";");
}

static void parse(void)
{
	while (tok_jmp(""))
		readdecl();
}

static void compat_macros(void)
{
	cpp_define("__STDC__", "");
	cpp_define("__linux__", "");
	cpp_define(I_ARCH, "");

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
	int size = UINT;
	int done = 0;
	int i = 0;
	int isunion;
	char name[NAMELEN] = "";
	*flags = 0;
	type->flags = 0;
	type->ptr = 0;
	type->addr = 0;
	while (!done) {
		if (!tok_jmp("static")) {
			*flags |= F_STATIC;
		} else if (!tok_jmp("extern")) {
			*flags |= F_EXTERN;
		} else if (!tok_jmp("void")) {
			sign = 0;
			size = 0;
			done = 1;
		} else if (!tok_jmp("int")) {
			done = 1;
		} else if (!tok_jmp("char")) {
			size = UCHR;
			done = 1;
		} else if (!tok_jmp("short")) {
			size = USHT;
		} else if (!tok_jmp("long")) {
			size = ULNG;
		} else if (!tok_jmp("signed")) {
			sign = 1;
		} else if (!tok_jmp("unsigned")) {
			sign = 0;
		} else if (tok_comes("union") || tok_comes("struct")) {
			isunion = !strcmp("union", tok_get());
			if (tok_grp() == 'a')
				strcpy(name, tok_get());
			if (tok_comes("{"))
				type->id = struct_create(name, isunion);
			else
				type->id = struct_find(name, isunion);
			type->flags |= T_STRUCT;
			type->bt = ULNG;
			return 0;
		} else if (!tok_jmp("enum")) {
			if (tok_grp() == 'a')
				tok_get();
			if (tok_comes("{"))
				enum_create();
			type->bt = SINT;
			return 0;
		} else {
			if (tok_grp() == 'a') {
				int id = typedef_find(tok_see());
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
	}
	type->bt = size | (sign ? BT_SIGNED : 0);
	return 0;
}

static void readptrs(struct type *type)
{
	while (!tok_jmp("*")) {
		type->ptr++;
		if (!type->bt)
			type->bt = 1;
	}
}

/* read function arguments */
static int readargs(struct type *args, char argnames[][NAMELEN], int *varg)
{
	int nargs = 0;
	tok_req("(");
	*varg = 0;
	while (!tok_comes(")")) {
		if (!tok_jmp("...")) {
			*varg = 1;
			break;
		}
		if (readname(&args[nargs], argnames[nargs], NULL)) {
			/* argument has no type, assume int */
			memset(&args[nargs], 0, sizeof(struct type));
			args[nargs].bt = SINT;
			strcpy(argnames[nargs], tok_get());
		}
		/* argument arrays are pointers */
		array2ptr(&args[nargs]);
		nargs++;
		if (tok_jmp(","))
			break;
	}
	tok_req(")");
	/* void argument */
	if (nargs == 1 && !TYPE_BT(&args[0]))
		return 0;
	return nargs;
}

/* read K&R function arguments */
static void krdef(void *data, struct name *name, unsigned flags)
{
	struct funcinfo *fi = data;
	int i;
	for (i = 0; i < fi->nargs; i++)
		if (!strcmp(fi->argnames[i], name->name))
			memcpy(&fi->args[i], &name->type, sizeof(name->type));
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
	while (!tok_jmp("[")) {
		long n = 0;
		if (tok_jmp("]")) {
			readexpr();
			ts_pop_de(NULL);
			if (o_popnum(&n))
				err("const expr expected\n");
			tok_req("]");
		}
		arsz[nar++] = n;
	}
	for (i = nar - 1; i >= 0; i--) {
		type->id = array_add(type, arsz[i]);
		if (!inner)
			inner = &arrays[type->id].type;
		type->flags = T_ARRAY;
		type->bt = ULNG;
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
	if (!tok_jmp("(")) {
		btype = type;
		type = &tpool[npool++];
		ptype = type;
		readptrs(type);
	}
	if (tok_grp() == 'a' && name)
		strcpy(name, tok_get());
	inner = readarrays(type);
	if (ptype && inner)
		ptype = inner;
	if (ptype)
		tok_req(")");
	if (tok_comes("(")) {
		struct type args[NARGS];
		char argnames[NARGS][NAMELEN];
		int varg = 0;
		int nargs = readargs(args, argnames, &varg);
		if (!ptype) {
			btype = type;
			type = &tpool[npool++];
			ptype = type;
		}
		ptype->flags = T_FUNC;
		ptype->bt = ULNG;
		ptype->id = func_create(btype, name, argnames, args, nargs, varg);
		if (!tok_comes(";"))
			while (!tok_comes("{") && !readdefs(krdef, &funcs[ptype->id]))
				tok_req(";");
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
 * readdef() reads a variable definition statement.  The definition
 * can appear in different contexts: global variables, function
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
	if (tok_comes(";") || tok_comes("{"))
		return 0;
	do {
		struct name name = {{""}};
		if (readname(&name.type, name.name, &base))
			break;
		def(data, &name, base_flags);
	} while (!tok_jmp(","));
	return 0;
}

/* just like readdefs, but default to int type; for handling K&R functions */
static int readdefs_int(void (*def)(void *data, struct name *name, unsigned flags),
			void *data)
{
	struct type base;
	unsigned flags = 0;
	if (basetype(&base, &flags)) {
		if (tok_grp() != 'a')
			return 1;
		memset(&base, 0, sizeof(base));
		base.bt = SINT;
	}
	if (!tok_comes(";")) {
		do {
			struct name name = {{""}};
			if (readname(&name.type, name.name, &base))
				break;
			def(data, &name, flags);
		} while (!tok_jmp(","));
	}
	return 0;
}


/* parsing initializer expressions */

static void jumpbrace(void)
{
	int depth = 0;
	while (!tok_comes("}") || depth--)
		if (!strcmp("{", tok_get()))
			depth++;
	tok_req("}");
}

/* compute the size of the initializer expression */
static int initsize(void)
{
	long addr = tok_addr();
	int n = 0;
	if (tok_jmp("="))
		return 0;
	if (tok_grp() == '"') {
		tok_get();
		n = tok_len() - 2 + 1;
		tok_jump(addr);
		return n;
	}
	tok_req("{");
	while (tok_jmp("}")) {
		long idx = n;
		if (!tok_jmp("[")) {
			readexpr();
			ts_pop_de(NULL);
			o_popnum(&idx);
			tok_req("]");
			tok_req("=");
		}
		if (n < idx + 1)
			n = idx + 1;
		while (!tok_comes("}") && !tok_comes(","))
			if (!strcmp("{", tok_get()))
				jumpbrace();
		tok_jmp(",");
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
	if (tok_jmp("{")) {
		set(obj, off, t);
		return;
	}
	if (!t->ptr && t->flags & T_STRUCT) {
		struct structinfo *si = &structs[t->id];
		int i;
		for (i = 0; i < si->nfields && !tok_comes("}"); i++) {
			struct name *field = &si->fields[i];
			if (!tok_jmp(".")) {
				field = struct_field(t->id, tok_get());
				tok_req("=");
			}
			initexpr(&field->type, off + field->addr, obj, set);
			if (tok_jmp(","))
				break;
		}
	} else if (t->flags & T_ARRAY) {
		struct type *t_de = &arrays[t->id].type;
		int i;
		/* handling extra braces as in: char s[] = {"sth"} */
		if (TYPE_SZ(t_de) == 1 && tok_grp() == '"') {
			set(obj, off, t);
			tok_req("}");
			return;
		}
		for (i = 0; !tok_comes("}"); i++) {
			long idx = i;
			struct type *it = t_de;
			if (!tok_jmp("[")) {
				readexpr();
				ts_pop_de(NULL);
				o_popnum(&idx);
				tok_req("]");
				tok_req("=");
			}
			if (!tok_comes("{") && (tok_grp() != '"' ||
						!(it->flags & T_ARRAY)))
				it = innertype(t_de);
			initexpr(it, off + type_totsz(it) * idx, obj, set);
			if (tok_jmp(","))
				break;
		}
	}
	tok_req("}");
}
