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
static long lab_last;		/* the last label target */

static int io_num(void);
static int io_mul2(void);
static int io_cmp(void);
static int io_jmp(void);
static int io_addr(void);
static int io_loc(void);
static int io_imm(void);
static int io_call(void);
static void io_deadcode(void);

static void iv_put(long n);

static struct ic *ic_put(long op, long arg1, long arg2, long arg3)
{
	struct ic *c;
	if (ic_n == ic_sz) {
		ic_sz = MAX(128, ic_sz * 2);
		ic = mextend(ic, ic_n, ic_sz, sizeof(*ic));
	}
	c = &ic[ic_n++];
	c->op = op;
	c->a1 = arg1;
	c->a2 = arg2;
	c->a3 = arg3;
	if (op & O_OUT)
		iv_put(ic_n - 1);
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
	ic_put(O_MOV | O_NUM, n, 0, 0);
}

void o_local(long id)
{
	ic_put(O_MOV | O_LOC, id, 0, 0);
}

void o_sym(char *sym)
{
	ic_put(O_MOV | O_SYM, out_sym(sym), 0, 0);
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

/* return one if the given value is constant */
static int ic_const(long iv)
{
	long oc = O_C(ic[iv].op);
	return oc & O_MOV && oc & (O_NUM | O_SYM | O_LOC);
}

/* return one if the given value is a simple load */
static int ic_load(long iv)
{
	long oc = O_C(ic[iv].op);
	int i;
	if (oc & O_LD && oc & (O_NUM | O_SYM | O_LOC)) {
		for (i = iv + 1; i < ic_n; i++)
			if (ic[i].op & O_ST)
				return 0;
		return 1;
	}
	return 0;
}

void o_bop(long op)
{
	int r1 = iv_pop();
	int r2 = iv_pop();
	/* load constants as late as possible */
	if (opt(1) && ic_const(r2) && !ic_const(r1)) {
		ic_put(ic[r2].op, ic[r2].a1, ic[r2].a2, ic[r2].a3);
		r2 = iv_pop();
	}
	ic_put(op, r2, r1, 0);
	if (opt(1))
		io_num() && io_mul2() && io_addr() && io_imm();
}

void o_uop(long op)
{
	int r1 = iv_pop();
	ic_put(op, r1, 0, 0);
	if (opt(1))
		io_num() && io_cmp();
}

void o_assign(long bt)
{
	int rv = iv_pop();
	int lv = iv_pop();
	/* load constants as late as possible */
	if (opt(1) && (ic_const(lv) || ic_load(lv))) {
		ic_put(ic[lv].op, ic[lv].a1, ic[lv].a2, ic[lv].a3);
		lv = iv_pop();
	}
	ic_put(O_MK(O_ST | O_NUM, bt), rv, lv, 0);
	iv_put(rv);
	if (opt(1))
		io_loc();
}

void o_deref(long bt)
{
	int r1 = iv_pop();
	ic_put(O_MK(O_LD | O_NUM, bt), r1, 0, 0);
	if (opt(1))
		io_loc();
}

void o_cast(long bt)
{
	if (T_SZ(bt) != ULNG) {
		int r1 = iv_pop();
		ic_put(O_MK(O_MOV, bt), r1, 0, 0);
		if (opt(1))
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
	for (i = argc - 1; i >= 0; --i) {
		int iv = args[i];
		/* load constants as late as possible */
		if (opt(1) && (ic_const(iv) || ic_load(iv))) {
			ic_put(ic[iv].op, ic[iv].a1, ic[iv].a2, ic[iv].a3);
			args[i] = iv_pop();
		}
	}
	r1 = iv_pop();
	c = ic_put(O_CALL, r1, 0, argc);
	c->args = args;
	iv_drop(ret == 0);
	if (opt(1))
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
	lab_last = ic_n;
}

void o_jmp(long id)
{
	ic_put(O_JMP, 0, 0, id);
}

void o_jz(long id)
{
	ic_put(O_JZ, iv_pop(), 0, id);
	if (opt(1))
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
	if (!ic_n || ~ic[ic_n - 1].op & O_RET || lab_last == ic_n)
		o_ret(0);
	for (i = 0; i < ic_n; i++)	/* filling branch targets */
		if (ic[i].op & O_JXX)
			ic[i].a3 = lab_loc[ic[i].a3];
	io_deadcode();			/* removing dead code */
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
	lab_last = 0;
}

void ic_free(struct ic *ic)
{
	if (ic->op & O_CALL)
		free(ic->args);
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
		*n = ic[iv].a1;
		return 0;
	}
	if (oc & O_BOP) {
		if (ic_num(ic, ic[iv].a1, &n1))
			return 1;
		if (ic_num(ic, ic[iv].a2, &n2))
			return 1;
		*n = cb(ic[iv].op, n1, n2);
		return 0;
	}
	if (oc & O_UOP) {
		if (ic_num(ic, ic[iv].a1, &n1))
			return 1;
		*n = cu(ic[iv].op, n1);
		return 0;
	}
	if (oc & O_MOV && !(oc & (O_NUM | O_LOC | O_SYM))) {
		if (ic_num(ic, ic[iv].a1, &n1))
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
		*sym = ic[iv].a1;
		*off = ic[iv].a2;
		return 0;
	}
	if (oc == O_ADD) {
		if ((ic_sym(ic, ic[iv].a1, sym, off) ||
				ic_num(ic, ic[iv].a2, &n)) &&
			(ic_sym(ic, ic[iv].a2, sym, off) ||
				ic_num(ic, ic[iv].a1, &n)))
			return 1;
		*off += n;
		return 0;
	}
	if (oc == O_SUB) {
		if (ic_sym(ic, ic[iv].a1, sym, off) ||
				ic_num(ic, ic[iv].a2, &n))
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
	if (oc == (O_ADD | O_NUM)) {
		*base_iv = ic[iv].a1;
		*off = ic[iv].a2;
		return 0;
	}
	if (oc == (O_SUB | O_NUM)) {
		*base_iv = ic[iv].a1;
		*off = -ic[iv].a2;
		return 0;
	}
	if (oc == O_ADD) {
		if ((ic_off(ic, ic[iv].a1, base_iv, off) ||
				ic_num(ic, ic[iv].a2, &n)) &&
			(ic_off(ic, ic[iv].a2, base_iv, off) ||
				ic_num(ic, ic[iv].a1, &n)))
			return 1;
		*off += n;
		return 0;
	}
	if (oc == O_SUB) {
		if (ic_off(ic, ic[iv].a1, base_iv, off) ||
				ic_num(ic, ic[iv].a2, &n))
			return 1;
		*off -= n;
		return 0;
	}
	*base_iv = iv;
	*off = 0;
	return 0;
}

/* number of register arguments */
int ic_regcnt(struct ic *ic)
{
	long o = ic->op;
	if (o & O_BOP)
		return o & (O_NUM | O_SYM | O_LOC) ? 1 : 2;
	if (o & O_UOP)
		return o & (O_NUM | O_SYM | O_LOC) ? 0 : 1;
	if (o & O_CALL)
		return o & (O_NUM | O_SYM | O_LOC) ? 0 : 1;
	if (o & O_MOV)
		return o & (O_NUM | O_SYM | O_LOC) ? 0 : 1;
	if (o & O_MEM)
		return 3;
	if (o & O_JMP)
		return 0;
	if (o & O_JZ)
		return 1;
	if (o & O_JCC)
		return o & (O_NUM | O_SYM | O_LOC) ? 1 : 2;
	if (o & O_RET)
		return 1;
	if (o & O_LD)
		return ((o & O_NUM) != 0);
	if (o & O_ST)
		return 1 + ((o & O_NUM) != 0);
	return 0;
}

/*
 * The returned array indicates the last instruction in
 * which the value produced by each instruction is used.
 */
long *ic_lastuse(struct ic *ic, long ic_n)
{
	long *luse = calloc(ic_n, sizeof(luse[0]));
	int i, j;
	for (i = ic_n - 1; i >= 0; --i) {
		int n = ic_regcnt(ic + i);
		if (!luse[i])
			if (!(ic[i].op & O_OUT) || ic[i].op & O_CALL)
				luse[i] = -1;
		if (!luse[i])
			continue;
		if (n >= 1 && !luse[ic[i].a1])
			luse[ic[i].a1] = i;
		if (n >= 2 && !luse[ic[i].a2])
			luse[ic[i].a2] = i;
		if (n >= 3 && !luse[ic[i].a3])
			luse[ic[i].a3] = i;
		if (ic[i].op & O_CALL)
			for (j = 0; j < ic[i].a3; j++)
				if (!luse[ic[i].args[j]])
					luse[ic[i].args[j]] = i;
	}
	return luse;
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
	long iv = iv_get(0);
	long n, p;
	long r1, r2;
	long oc = O_C(ic[iv].op);
	long bt = O_T(ic[iv].op);
	if (!(oc & O_MUL))
		return 1;
	if (oc == O_MUL && !ic_num(ic, ic[iv].a1, &n)) {
		long t = ic[iv].a1;
		ic[iv].a1 = ic[iv].a2;
		ic[iv].a2 = t;
	}
	if (ic_num(ic, ic[iv].a2, &n))
		return 1;
	p = log2a(n);
	if (n && p < 0)
		return 1;
	if (oc == O_MUL) {
		iv_drop(1);
		if (n == 1) {
			iv_put(ic[iv].a1);
			return 0;
		}
		if (n == 0) {
			o_num(0);
			return 0;
		}
		r2 = iv_num(p);
		ic_put(O_MK(O_SHL, ULNG), ic[iv].a1, r2, 0);
		return 0;
	}
	if (oc == O_DIV && ~bt & T_MSIGN) {
		iv_drop(1);
		if (n == 1) {
			iv_put(ic[iv].a1);
			return 0;
		}
		r2 = iv_num(p);
		ic_put(O_MK(O_SHR, ULNG), ic[iv].a1, r2, 0);
		return 0;
	}
	if (oc == O_MOD && ~bt & T_MSIGN) {
		iv_drop(1);
		if (n == 1) {
			o_num(0);
			return 0;
		}
		r2 = iv_num(LONGSZ * 8 - p);
		ic_put(O_MK(O_SHL, ULNG), ic[iv].a1, r2, 0);
		r1 = iv_pop();
		r2 = iv_num(LONGSZ * 8 - p);
		ic_put(O_MK(O_SHR, ULNG), r1, r2, 0);
		return 0;
	}
	return 1;
}

/* optimise comparison */
static int io_cmp(void)
{
	long iv = iv_get(0);
	long cmp = ic[iv].a1;
	if (O_C(ic[iv].op) == O_LNOT && ic[cmp].op & O_CMP) {
		iv_drop(1);
		ic[cmp].op ^= 1;
		iv_put(cmp);
		return 0;
	}
	return 1;
}

/* optimise branch instructions after comparison */
static int io_jmp(void)
{
	struct ic *c = &ic[ic_n - 1];
	long oc = O_C(c->op);
	if (oc & O_JZ && O_C(ic[c->a1].op) == O_LNOT) {
		c->a1 = ic[c->a1].a1;
		c->op ^= 1;
		return 0;
	}
	if (oc & O_JZ && O_C(ic[c->a1].op) & O_CMP) {
		long cop = (ic[c->a1].op & ~O_CMP) | O_JCC;
		c->op = O_C(c->op) == O_JZ ? cop ^ 1 : cop;
		c->a2 = ic[c->a1].a2;
		c->a1 = ic[c->a1].a1;
		return 0;
	}
	return 1;
}

/* optimise accessing locals or symbols with an offset */
static int io_addr(void)
{
	long iv, off;
	if (ic_off(ic, iv_get(0), &iv, &off) || iv == iv_get(0))
		return 1;
	if (ic[iv].op & O_MOV && ic[iv].op & O_LOC) {
		iv_drop(1);
		ic_put(O_MOV | O_LOC, ic[iv].a1, ic[iv].a2 + off, 0);
		return 0;
	}
	if (ic[iv].op & O_MOV && ic[iv].op & O_SYM) {
		iv_drop(1);
		ic_put(O_MOV | O_SYM, ic[iv].a1, ic[iv].a2 + off, 0);
		return 0;
	}
	return 1;
}

static int imm_ok(long op, long n, int arg)
{
	long m0, m1, m2, m3, mt;
	if (i_reg(op | O_NUM, &m0, &m1, &m2, &m3, &mt))
		return 0;
	return i_imm(arg == 2 ? m2 : m1, n);
}

/* optimise loading and storing locals */
static int io_loc(void)
{
	struct ic *c = &ic[ic_n - 1];
	long iv, off;
	if (c->op & O_LD && c->op & O_NUM) {
		if (ic_off(ic, c->a1, &iv, &off))
			return 1;
		if (ic[iv].op & O_MOV && ic[iv].op & O_LOC) {
			c->op = (c->op & ~O_NUM) | O_LOC;
			c->a1 = ic[iv].a1;
			c->a2 += ic[iv].a2 + off;
			return 0;
		}
		if (imm_ok(c->op, 2, off)) {
			c->a1 = iv;
			c->a2 += off;
		}
		return 0;
	}
	if (c->op & O_ST && c->op & O_NUM) {
		if (ic_off(ic, c->a2, &iv, &off))
			return 1;
		if (ic[iv].op & O_MOV && ic[iv].op & O_LOC) {
			c->op = (c->op & ~O_NUM) | O_LOC;
			c->a2 = ic[iv].a1;
			c->a3 += ic[iv].a2 + off;
			return 0;
		}
		if (imm_ok(c->op, 2, off)) {
			c->a2 = iv;
			c->a3 += off;
		}
		return 0;
	}
	return 1;
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
		if (!ic_num(ic, c->a1, &n)) {
			long t = c->a1;
			c->a1 = c->a2;
			c->a2 = t;
		}
	}
	if (oc == O_LT || oc == O_GE || oc == O_LE || oc == O_GT) {
		if (!ic_num(ic, c->a1, &n)) {
			int t = c->a1;
			c->a1 = c->a2;
			c->a2 = t;
			c->op ^= 1;
		}
	}
	if (oc & O_JCC && !ic_num(ic, c->a1, &n)) {
		int t = c->a1;
		c->a1 = c->a2;
		c->a2 = t;
		c->op ^= 1;
	}
	if (oc & O_JCC && !ic_num(ic, c->a2, &n) && imm_ok(c->op, n, 1)) {
		c->op |= O_NUM;
		c->a2 = n;
		return 0;
	}
	if (!(oc & O_BOP) || ic_num(ic, c->a2, &n))
		return 1;
	if ((oc == O_ADD || oc == O_SUB || oc & O_SHL) && n == 0) {
		iv_drop(1);
		iv_put(c->a1);
		return 0;
	}
	if (imm_ok(c->op, n, 2)) {
		c->op |= O_NUM;
		c->a2 = n;
		return 0;
	}
	return 1;
}

/* calling symbols */
static int io_call(void)
{
	struct ic *c = &ic[ic_n - 1];
	long sym, off;
	if (c->op & O_CALL && !ic_sym(ic, c->a1, &sym, &off) && !off) {
		c->op |= O_SYM;
		c->a1 = sym;
		return 0;
	}
	return 1;
}

/* remove dead code */
static void io_deadcode(void)
{
	char *live;
	long *nidx;
	long src = 0, dst = 0;
	int i, j;
	/* liveness analysis */
	live = calloc(ic_n, sizeof(live[0]));
	for (i = ic_n - 1; i >= 0; i--) {
		int n = ic_regcnt(ic + i);
		if (!(ic[i].op & O_OUT) || ic[i].op & O_CALL)
			live[i] = 1;
		if (!live[i])
			continue;
		if (n >= 1)
			live[ic[i].a1] = 1;
		if (n >= 2)
			live[ic[i].a2] = 1;
		if (n >= 3)
			live[ic[i].a3] = 1;
		if (ic[i].op & O_CALL)
			for (j = 0; j < ic[i].a3; j++)
				live[ic[i].args[j]] = 1;
	}
	/* the new indices of intermediate instructions */
	nidx = calloc(ic_n, sizeof(nidx[0]));
	while (src < ic_n) {
		while (src < ic_n && !live[src]) {
			nidx[src] = dst;
			ic_free(&ic[src++]);
		}
		if (src < ic_n) {
			nidx[src] = dst;
			if (src != dst)
				memcpy(ic + dst, ic + src, sizeof(ic[src]));
			src++;
			dst++;
		}
	}
	ic_n = dst;
	/* adjusting arguments and branch targets */
	for (i = 0; i < ic_n; i++) {
		int n = ic_regcnt(ic + i);
		if (n >= 1)
			ic[i].a1 = nidx[ic[i].a1];
		if (n >= 2)
			ic[i].a2 = nidx[ic[i].a2];
		if (n >= 3)
			ic[i].a3 = nidx[ic[i].a3];
		if (ic[i].op & O_JXX)
			ic[i].a3 = nidx[ic[i].a3];
		if (ic[i].op & O_CALL)
			for (j = 0; j < ic[i].a3; j++)
				ic[i].args[j] = nidx[ic[i].args[j]];
	}
	free(live);
	free(nidx);
}
