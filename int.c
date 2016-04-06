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
static int io_cmp(void);
static int io_jmp(void);
static int io_addr(void);
static int io_loc(void);
static int io_imm(void);
static int io_call(void);

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

static void ic_back(long pos)
{
	int i;
	for (i = pos; i < ic_n; i++)
		if (ic[i].op & O_CALL)
			free(ic[i].args);
	ic_n = pos;
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
	ic_put(O_MOV | O_NUM, iv_new(), n, 0);
}

void o_local(long id)
{
	ic_put(O_MOV | O_LOC, iv_new(), id, 0);
}

void o_sym(char *sym)
{
	ic_put(O_MOV | O_SYM, iv_new(), out_sym(sym), 0);
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
	io_num() && io_mul2() && io_imm() && io_addr();
}

void o_uop(long op)
{
	int r1 = iv_pop();
	ic_put(op, iv_new(), r1, 0);
	if (io_num())
		io_cmp();
}

void o_assign(long bt)
{
	ic_put(O_MK(O_ST | O_NUM, bt), iv_get(0), iv_get(1), 0);
	iv_swap(0, 1);
	iv_pop();
	io_loc();
}

void o_deref(long bt)
{
	int r1 = iv_pop();
	ic_put(O_MK(O_LD | O_NUM, bt), iv_new(), r1, 0);
	io_loc();
}

void o_cast(long bt)
{
	if (T_SZ(bt) != ULNG) {
		int r1 = iv_pop();
		ic_put(O_MK(O_MOV, bt), iv_new(), r1, 0);
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
	struct ic *c;
	long *args = malloc(argc * sizeof(c->args[0]));
	int r1, i;
	for (i = argc - 1; i >= 0; --i)
		args[i] = iv_pop();
	r1 = iv_pop();
	c = ic_put(O_CALL, iv_new(), r1, argc);
	c->args = args;
	iv_drop(ret == 0);
	io_call();
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
		lab_sz = MAX(128, lab_sz * 2);
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
	io_jmp();
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
	ic_back(mark);
}

void ic_get(struct ic **c, long *n)
{
	int i;
	if (!ic_n || ~ic[ic_n - 1].op & O_RET ||
			(lab_n && lab_loc[lab_n - 1] == ic_n))
		o_ret(0);
	/* filling jump destinations */
	for (i = 0; i < ic_n; i++)
		if (ic[i].op & O_JXX)
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

static long cb(long op, long a, long b)
{
	switch (O_C(op)) {
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
	case O_MUL:
		return a * b;
	case O_DIV:
		return a / b;
	case O_MOD:
		return a % b;
	case O_SHL:
		return a << b;
	case O_SHR:
		return O_T(op) & T_MSIGN ? a >> b : (unsigned long) a >> b;
	case O_LT:
		return a < b;
	case O_GT:
		return a > b;
	case O_LE:
		return a <= b;
	case O_GE:
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
	switch (O_C(op)) {
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
	long oc = O_C(ic[iv].op);
	long bt = O_T(ic[iv].op);
	if (oc & O_MOV && oc & O_NUM) {
		*n = ic[iv].arg1;
		return 0;
	}
	if (oc & O_BOP) {
		if (ic_num(ic, ic[iv].arg1, &n1))
			return 1;
		if (ic_num(ic, ic[iv].arg2, &n2))
			return 1;
		*n = cb(ic[iv].op, n1, n2);
		return 0;
	}
	if (oc & O_UOP) {
		if (ic_num(ic, ic[iv].arg1, &n1))
			return 1;
		*n = cu(ic[iv].op, n1);
		return 0;
	}
	if (oc & O_MOV && !(oc & (O_NUM | O_LOC | O_SYM))) {
		if (ic_num(ic, ic[iv].arg1, &n1))
			return 1;
		*n = c_cast(n1, bt);
		return 0;
	}
	return 1;
}

int ic_sym(struct ic *ic, long iv, long *sym, long *off)
{
	long n;
	long oc = O_C(ic[iv].op);
	if (oc & O_MOV && oc & O_SYM) {
		*sym = ic[iv].arg1;
		*off = ic[iv].arg2;
		return 0;
	}
	if (oc == O_ADD) {
		if ((ic_sym(ic, ic[iv].arg1, sym, off) ||
				ic_num(ic, ic[iv].arg2, &n)) &&
			(ic_sym(ic, ic[iv].arg2, sym, off) ||
				ic_num(ic, ic[iv].arg1, &n)))
			return 1;
		*off += n;
		return 0;
	}
	if (oc == O_SUB) {
		if (ic_sym(ic, ic[iv].arg1, sym, off) ||
				ic_num(ic, ic[iv].arg2, &n))
			return 1;
		*off -= n;
		return 0;
	}
	return 1;
}

static int ic_off(struct ic *ic, long iv, long *base_iv, long *off)
{
	long n;
	long oc = O_C(ic[iv].op);
	if (oc != O_ADD && oc != O_SUB) {
		*base_iv = iv;
		*off = 0;
		return 0;
	}
	if (oc == O_ADD) {
		if ((ic_off(ic, ic[iv].arg1, base_iv, off) ||
				ic_num(ic, ic[iv].arg2, &n)) &&
			(ic_off(ic, ic[iv].arg2, base_iv, off) ||
				ic_num(ic, ic[iv].arg1, &n)))
			return 1;
		*off += n;
		return 0;
	}
	if (oc == O_SUB) {
		if (ic_off(ic, ic[iv].arg1, base_iv, off) ||
				ic_num(ic, ic[iv].arg2, &n))
			return 1;
		*off -= n;
		return 0;
	}
	return 1;
}

/* intermediate code optimisations */

/* constant folding */
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

/* optimised multiplication operations for powers of two */
static int io_mul2(void)
{
	struct ic *c = &ic[iv_get(0)];
	long n, p;
	long r1, r2;
	long oc = O_C(c->op);
	long bt = O_T(c->op);
	if (!(oc & O_MUL))
		return 1;
	if (oc == O_MUL && !ic_num(ic, c->arg1, &n)) {
		long t = c->arg1;
		c->arg1 = c->arg2;
		c->arg2 = t;
	}
	if (ic_num(ic, c->arg2, &n))
		return 1;
	p = log2a(n);
	if (n && p < 0)
		return 1;
	if (oc == O_MUL) {
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
		ic_put(O_MK(O_SHL, ULNG), iv_new(), c->arg1, r2);
		return 0;
	}
	if (oc == O_DIV && ~bt & T_MSIGN) {
		iv_drop(1);
		if (n == 1) {
			iv_put(c->arg1);
			return 0;
		}
		r2 = iv_num(p);
		ic_put(O_MK(O_SHR, ULNG), iv_new(), c->arg1, r2);
		return 0;
	}
	if (oc == O_MOD && ~bt & T_MSIGN) {
		iv_drop(1);
		if (n == 1) {
			o_num(0);
			return 0;
		}
		r2 = iv_num(LONGSZ * 8 - p);
		ic_put(O_MK(O_SHL, ULNG), iv_new(), c->arg1, r2);
		r1 = iv_pop();
		r2 = iv_num(LONGSZ * 8 - p);
		ic_put(O_MK(O_SHR, ULNG), iv_new(), r1, r2);
		return 0;
	}
	return 1;
}

/* optimise comparison */
static int io_cmp(void)
{
	struct ic *c = &ic[iv_get(0)];
	long cmp = c->arg1;
	if (O_C(c->op) == O_LNOT && ic[cmp].op & O_CMP) {
		iv_drop(1);
		ic[cmp].op ^= 1;
		iv_put(ic[cmp].arg0);
		return 0;
	}
	return 1;
}

/* optimise branch instructions after comparison */
static int io_jmp(void)
{
	struct ic *c = &ic[ic_n - 1];
	long oc = O_C(c->op);
	if (oc & O_JZ && O_C(ic[c->arg0].op) == O_LNOT) {
		c->arg0 = ic[c->arg0].arg1;
		c->op ^= 1;
		return 0;
	}
	if (oc & O_JZ && O_C(ic[c->arg0].op) & O_CMP) {
		long cop = (ic[c->arg0].op & ~O_CMP) | O_JCC;
		c->op = O_C(c->op) == O_JZ ? cop ^ 1 : cop;
		c->arg1 = ic[c->arg0].arg2;
		c->arg0 = ic[c->arg0].arg1;
		return 0;
	}
	return 1;
}

/* optimise accessing locals or symbols with an offset */
static int io_addr(void)
{
	long iv, off;
	if (ic_off(ic, ic_n - 1, &iv, &off) || iv != ic_n - 1)
		return 1;
	if (ic[iv].op & O_MOV && ic[iv].op & O_LOC) {
		iv_drop(1);
		ic_put(O_MOV | O_LOC, iv_new(), ic[iv].arg1, ic[iv].arg2 + off);
		return 0;
	}
	if (ic[iv].op & O_MOV && ic[iv].op & O_SYM) {
		iv_drop(1);
		ic_put(O_MOV | O_SYM, iv_new(), ic[iv].arg1, ic[iv].arg2 + off);
		return 0;
	}
	return 1;
}

/* optimise loading and storing locals */
static int io_loc(void)
{
	struct ic *c = &ic[ic_n - 1];
	long iv, off;
	if (!(c->op & (O_LD | O_ST)) || !(c->op & O_NUM))
		return 1;
	if (ic_off(ic, c->arg1, &iv, &off))
		return 1;
	if (ic[iv].op & O_MOV && ic[iv].op & O_LOC) {
		c->op = (c->op & ~O_NUM) | O_LOC;
		c->arg1 = ic[iv].arg1;
		c->arg2 += ic[iv].arg2 + off;
		return 0;
	}
	c->arg1 = iv;
	c->arg2 += off;
	return 0;
}

static int imm_ok(long op, long n, int arg)
{
	long m0, m1, m2, mt, bits;
	long max;
	if (i_reg(op | O_NUM, &m0, &m1, &m2, &mt))
		return 0;
	bits = arg == 2 ? m2 : m1;
	max = (1 << (bits - 1)) - 1;
	return n <= max && n + 1 >= -max;
}

/* use instruction immediates */
static int io_imm(void)
{
	struct ic *c = &ic[ic_n - 1];
	long oc = O_C(c->op);
	long n;
	if (oc & (O_NUM | O_LOC | O_SYM))
		return 1;
	if (oc == O_ADD || oc == O_MUL || oc == O_AND || oc == O_OR ||
			oc == O_XOR || oc == O_EQ || oc == O_NE) {
		if (!ic_num(ic, c->arg1, &n)) {
			long t = c->arg1;
			c->arg1 = c->arg2;
			c->arg2 = t;
		}
	}
	if (oc == O_LT || oc == O_GE || oc == O_LE || oc == O_GT) {
		if (!ic_num(ic, c->arg1, &n)) {
			int t = c->arg1;
			c->arg1 = c->arg2;
			c->arg2 = t;
			c->op ^= 1;
		}
	}
	if (oc & O_JCC && !ic_num(ic, c->arg0, &n)) {
		int t = c->arg0;
		c->arg0 = c->arg1;
		c->arg1 = t;
		c->op ^= 1;
	}
	if (oc & O_JCC && !ic_num(ic, c->arg1, &n) && imm_ok(c->op, n, 1)) {
		c->op |= O_NUM;
		c->arg1 = n;
		return 0;
	}
	if (!(oc & O_BOP) || ic_num(ic, c->arg2, &n))
		return 1;
	if ((oc == O_ADD || oc == O_SUB) && n == 0) {
		iv_drop(1);
		iv_put(c->arg1);
		return 0;
	}
	if (imm_ok(c->op, n, 2)) {
		c->op |= O_NUM;
		c->arg2 = n;
		return 0;
	}
	return 1;
}

/* calling symbols */
static int io_call(void)
{
	int iv = iv_get(0);
	struct ic *c = &ic[iv];
	long sym, off;
	if (c->op & O_CALL && !ic_sym(ic, c->arg1, &sym, &off) && !off) {
		c->op |= O_SYM;
		c->arg1 = sym;
		return 0;
	}
	return 1;
}
