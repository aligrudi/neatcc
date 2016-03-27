/* neatcc intermediate code generation */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ncc.h"

static struct ic *ic;		/* intermediate code */
static long ic_n, ic_sz;	/* number of instructions in ic[] */
static long iv[NTMPS];		/* operand stack */
static long iv_n;		/* number of values in iv[] */
static long *lab_loc;		/* label locations */
static long lab_n, lab_sz;	/* number of labels in lab_loc[] */

static struct ic *ic_new(void)
{
	if (ic_n == ic_sz) {
		ic_sz = MAX(128, ic_sz * 2);
		ic = mextend(ic, ic_n, ic_sz, sizeof(*ic));
	}
	return &ic[ic_n++];
}

static struct ic *ic_put(long op, long arg0, long arg1, long arg2)
{
	struct ic *c = ic_new();
	c->op = op;
	c->arg0 = arg0;
	c->arg1 = arg1;
	c->arg2 = arg2;
	return c;
}

static long iv_pop(void)
{
	return iv[--iv_n];
}

static long iv_get(int n)
{
	return iv[iv_n - n - 1];
}

static long iv_new(void)
{
	iv[iv_n] = ic_n;
	return iv[iv_n++];
}

static void iv_drop(int n)
{
	iv_n = MAX(0, iv_n - n);
}

static void iv_swap(int x, int y)
{
	long v = iv[iv_n - x - 1];
	iv[iv_n - x - 1] = iv[iv_n - y - 1];
	iv[iv_n - y - 1] = v;
}

static void iv_dup(void)
{
	iv[iv_n] = iv[iv_n - 1];
	iv_n++;
}

void o_num(long n)
{
	ic_put(O_NUM, iv_new(), 0, n);
}

void o_local(long id)
{
	ic_put(O_LOC, iv_new(), 0, id);
}

void o_sym(char *sym)
{
	ic_put(O_SYM, iv_new(), 0, out_sym(sym));
}

void o_tmpdrop(int n)
{
	iv_drop(n >= 0 ? n : iv_n);
}

void o_tmpswap(void)
{
	iv_swap(0, 1);
}

void o_tmpcopy(void)
{
	iv_dup();
}

void o_bop(long op)
{
	int r1 = iv_pop();
	int r2 = iv_pop();
	ic_put(op, iv_new(), r2, r1);
}

void o_uop(long op)
{
	int r1 = iv_pop();
	ic_put(op, iv_new(), r1, 0);
}

void o_assign(long bt)
{
	ic_put(O_SAVE, iv_get(1), iv_get(0), bt);
	iv_swap(0, 1);
	iv_pop();
}

void o_deref(long bt)
{
	int r1 = iv_pop();
	ic_put(O_LOAD, iv_new(), r1, bt);
}

void o_cast(long bt)
{
	if (BT_SZ(bt) != LONGSZ) {
		int r1 = iv_pop();
		ic_put(O_MOV, iv_new(), r1, bt);
	}
}

void o_memcpy(void)
{
	int r2 = iv_pop();
	int r1 = iv_pop();
	int r0 = iv_pop();
	ic_put(O_MCPY, r0, r1, r2);
}

void o_memset(void)
{
	int r2 = iv_pop();
	int r1 = iv_pop();
	int r0 = iv_pop();
	ic_put(O_MSET, r0, r1, r2);
}

void o_call(int argc, int ret)
{
	struct ic *ic;
	long *args = malloc(argc * sizeof(ic->args[0]));
	int r1, i;
	for (i = argc - 1; i >= 0; --i)
		args[i] = iv_pop();
	r1 = iv_pop();
	ic = ic_put(O_CALL, iv_new(), r1, argc);
	ic->args = args;
	iv_drop(ret == 0);
}

void o_ret(int ret)
{
	if (!ret)
		o_num(0);
	ic_put(O_RET, iv_pop(), 0, 0);
}

void o_label(long id)
{
	while (id >= lab_sz) {
		lab_sz = MAX(128, ic_sz * 2);
		lab_loc = mextend(lab_loc, lab_n, lab_sz, sizeof(*lab_loc));
	}
	while (lab_n <= id)
		lab_loc[lab_n++] = -1;
	lab_loc[id] = ic_n;
}

void o_jmp(long id)
{
	ic_put(O_JMP, 0, 0, id);
}

void o_jz(long id)
{
	ic_put(O_JZ, iv_pop(), 0, id);
}

int o_popnum(long *n)
{
	if (ic_num(ic, iv_get(0), n))
		return 1;
	iv_drop(1);
	return 0;
}

int o_popsym(long *sym, long *off)
{
	if (ic_sym(ic, iv_get(0), sym, off))
		return 1;
	iv_drop(1);
	return 0;
}

long o_mark(void)
{
	return ic_n;
}

void o_back(long mark)
{
	int i;
	for (i = mark; i < ic_n; i++)
		if (ic[i].op == O_CALL)
			free(ic[i].args);
	ic_n = mark;
}

void ic_get(struct ic **c, long *n)
{
	int i;
	if (!ic_n || ic[ic_n - 1].op != O_RET)
		o_ret(0);
	/* filling jump destinations */
	for (i = 0; i < ic_n; i++)
		if (ic[i].op == O_JMP || ic[i].op == O_JZ)
			ic[i].arg2 = lab_loc[ic[i].arg2];
	*c = ic;
	*n = ic_n;
	ic = NULL;
	ic_n = 0;
	ic_sz = 0;
	iv_n = 0;
	free(lab_loc);
	lab_loc = NULL;
	lab_n = 0;
	lab_sz = 0;
}
