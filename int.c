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

static int io_num(void);
static int io_mul2(void);

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

static void iv_put(long n)
{
	iv[iv_n++] = n;
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
	if (io_num())
		io_mul2();
}

void o_uop(long op)
{
	int r1 = iv_pop();
	ic_put(op, iv_new(), r1, 0);
	io_num();
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
	if (T_SZ(bt) != ULNG) {
		int r1 = iv_pop();
		ic_put(O_MOV, iv_new(), r1, bt);
		io_num();
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

/* intermediate code queries */

static long cb(int op, long a, long b)
{
	switch (op) {
	case O_ADD:
		return a + b;
	case O_SUB:
		return a - b;
	case O_AND:
		return a & b;
	case O_OR:
		return a | b;
	case O_XOR:
		return a ^ b;
	case O_MUL | O_FSIGN:
	case O_MUL:
		return a * b;
	case O_DIV | O_FSIGN:
	case O_DIV:
		return a / b;
	case O_MOD | O_FSIGN:
	case O_MOD:
		return a % b;
	case O_SHL:
		return a << b;
	case O_SHR | O_FSIGN:
		return a >> b;
	case O_SHR:
		return (unsigned long) a >> b;
	case O_LT:
	case O_LT | O_FSIGN:
		return a < b;
	case O_GT:
	case O_GT | O_FSIGN:
		return a > b;
	case O_LE:
	case O_LE | O_FSIGN:
		return a <= b;
	case O_GE:
	case O_GE | O_FSIGN:
		return a >= b;
	case O_EQ:
		return a == b;
	case O_NE:
		return a != b;
	}
	return 0;
}

static long cu(int op, long i)
{
	switch (op) {
	case O_NEG:
		return -i;
	case O_NOT:
		return ~i;
	case O_LNOT:
		return !i;
	}
	return 0;
}

static long c_cast(long n, unsigned bt)
{
	if (!(bt & T_MSIGN) && T_SZ(bt) != ULNG)
		n &= ((1l << (long) (T_SZ(bt) * 8)) - 1);
	if (bt & T_MSIGN && T_SZ(bt) != ULNG &&
				n > (1l << (T_SZ(bt) * 8 - 1)))
		n = -((1l << (T_SZ(bt) * 8)) - n);
	return n;
}

int ic_num(struct ic *ic, long iv, long *n)
{
	long n1, n2;
	if (ic[iv].op == O_NUM) {
		*n = ic[iv].arg2;
		return 0;
	}
	if (ic[iv].op & O_MBOP) {
		if (ic_num(ic, ic[iv].arg1, &n1))
			return 1;
		if (ic_num(ic, ic[iv].arg2, &n2))
			return 1;
		*n = cb(ic[iv].op, n1, n2);
		return 0;
	}
	if (ic[iv].op & O_MUOP) {
		if (ic_num(ic, ic[iv].arg1, &n1))
			return 1;
		*n = cu(ic[iv].op, n1);
		return 0;
	}
	if (ic[iv].op == O_MOV) {
		if (ic_num(ic, ic[iv].arg1, &n1))
			return 1;
		*n = c_cast(n1, ic[iv].arg2);
		return 0;
	}
	return 1;
}

int ic_sym(struct ic *ic, long iv, long *sym, long *off)
{
	long n;
	if (ic[iv].op == O_SYM) {
		*sym = ic[iv].arg2;
		*off = 0;
		return 0;
	}
	if (ic[iv].op == O_ADD) {
		if ((ic_sym(ic, ic[iv].arg1, sym, off) ||
				ic_num(ic, ic[iv].arg2, &n)) &&
			(ic_sym(ic, ic[iv].arg2, sym, off) ||
				ic_num(ic, ic[iv].arg1, &n)))
			return 1;
		*off += n;
		return 0;
	}
	if (ic[iv].op == O_SUB) {
		if (ic_sym(ic, ic[iv].arg1, sym, off) ||
				ic_num(ic, ic[iv].arg2, &n))
			return 1;
		*off -= n;
		return 0;
	}
	return 1;
}

/* intermediate code optimizations */

static int io_num(void)
{
	long n;
	if (!ic_num(ic, iv_get(0), &n)) {
		iv_drop(1);
		o_num(n);
		return 0;
	}
	return 1;
}

static int log2a(unsigned long n)
{
	int i = 0;
	for (i = 0; i < LONGSZ * 8; i++)
		if (n & (1u << i))
			break;
	if (i == LONGSZ * 8 || !(n >> (i + 1)))
		return i;
	return -1;
}

static long iv_num(long n)
{
	o_num(n);
	return iv_pop();
}

/* optimized multiplication operations for powers of two */
static int io_mul2(void)
{
	struct ic *c = &ic[iv_get(0)];
	long n, p;
	long r1, r2;
	if (!(c->op & O_FMUL))
		return 1;
	if ((c->op & ~O_FSIGN) == O_MUL && ic_num(ic, c->arg1, &n)) {
		long t = c->arg1;
		c->arg1 = c->arg2;
		c->arg2 = t;
	}
	if (ic_num(ic, c->arg2, &n))
		return 1;
	p = log2a(n);
	if (n && p < 0)
		return 1;
	if ((c->op & ~O_FSIGN) == O_MUL) {
		iv_drop(1);
		if (n == 1) {
			iv_put(c->arg1);
			return 0;
		}
		if (n == 0) {
			o_num(0);
			return 0;
		}
		r2 = iv_num(p);
		ic_put(O_SHL, iv_new(), c->arg1, r2);
		return 0;
	}
	if (c->op == O_DIV) {
		iv_drop(1);
		if (n == 1) {
			iv_put(c->arg1);
			return 0;
		}
		r2 = iv_num(p);
		ic_put(O_SHR, iv_new(), c->arg1, r2);
		return 0;
	}
	if (c->op == O_MOD) {
		iv_drop(1);
		if (n == 1) {
			o_num(0);
			return 0;
		}
		r2 = iv_num(LONGSZ * 8 - p);
		ic_put(O_SHL, iv_new(), c->arg1, r2);
		r1 = iv_pop();
		r2 = iv_num(LONGSZ * 8 - p);
		ic_put(O_SHR, iv_new(), r1, r2);
		return 0;
	}
	return 1;
}
