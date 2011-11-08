#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "gen.h"
#include "out.h"
#include "tok.h"

/* variable location */
#define LOC_REG		0x01
#define LOC_MEM		0x02
#define LOC_NUM		0x04
#define LOC_SYM		0x08
#define LOC_LOCAL	0x10

/* special registers */
#define REG_FP		R_RBP
#define REG_SP		R_RSP
#define REG_RET		R_RAX
#define REG_FORK	R_RAX

/* registers */
#define R_RAX		0x00
#define R_RCX		0x01
#define R_RDX		0x02
#define R_RBX		0x03
#define R_RSP		0x04
#define R_RBP		0x05
#define R_RSI		0x06
#define R_RDI		0x07

#define N_REGS		8
#define N_ARGS		0
#define N_TMPS		ARRAY_SIZE(tmpregs)
#define R_TMPS		0x00cf
#define R_ARGS		0x0000
#define R_SAVED		0x00c8
#define R_BYTEREGS	(1 << R_RAX | 1 << R_RDX | 1 << R_RCX)

#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))

static char cs[SECSIZE];	/* code segment */
static int cslen;
static char ds[SECSIZE];	/* data segment */
static int dslen;
static long bsslen;		/* bss segment size */

static long sp;			/* stack pointer offset from R_RBP */
static long sp_max;		/* maximum stack pointer offset */
static long sp_tmp;		/* sp for the first tmp on the stack */
static long func_fpsub;		/* stack pointer sub address in CS */

#define TMP(i)		(((i) < ntmp) ? &tmps[ntmp - 1 - (i)] : NULL)

static struct tmp {
	long addr;
	char sym[NAMELEN];
	long off;	/* offset from a symbol or a local */
	unsigned loc;	/* variable location */
	unsigned bt;	/* type of address; zero when not a pointer */
} tmps[MAXTMP];
static int ntmp;

/* arch-specific functions */
static void i_ldr(int l, int rd, int rn, int off, int bt);
static void i_mov(int rd, int rn, int bt);
static void i_add(int op, int rd, int rn, int rm);
static void i_shl(int op, int rd, int rm, int rs);
static void i_mul(int rd, int rn, int rm);
static void i_div(int op, int rd, int rn, int rm);
static void i_cmp(int rn, int rm);
static int i_decodeable(long imm);
static void i_add_imm(int op, int rd, int rn, long n);
static void i_shl_imm(int op, int rd, int rn, long n);
static void i_cmp_imm(int rn, long n);
static void i_add_anyimm(int rd, int rn, long n);
static void i_num(int rd, long n);
static void i_sym(int rd, char *sym, int off);
static void i_set(int op, int rd);
static void i_neg(int rd);
static void i_not(int rd);
static void i_lnot(int rd);
static void i_zx(int rd, int bits);
static void i_sx(int rd, int bits);
static void i_b(long addr);
static void i_b_fill(long *dst, int diff);
static void i_b_if(long addr, int rn, int z);
static void i_call(char *sym, int off);
static void i_call_reg(int rd);
static void i_prolog(void);
static void i_epilog(void);

static struct tmp *regs[N_REGS];
static int tmpregs[] = {R_RAX, R_RSI, R_RDI, R_RBX, R_RDX, R_RCX};

#define MAXRET			(1 << 8)

static long ret[MAXRET];
static int nret;

static void putint(char *s, long n, int l)
{
	while (l--) {
		*s++ = n;
		n >>= 8;
	}
}

static void os(void *s, int n)
{
	while (n--)
		cs[cslen++] = *(char *) (s++);
}

static void oi(long n, int l)
{
	while (l--) {
		cs[cslen++] = n;
		n >>= 8;
	}
}

static long sp_push(int size)
{
	sp += size;
	if (sp > sp_max)
		sp_max = sp;
	return sp;
}

static void tmp_mem(struct tmp *tmp)
{
	int src = tmp->addr;
	if (tmp->loc != LOC_REG)
		return;
	if (sp_tmp == -1)
		sp_tmp = sp;
	tmp->addr = -sp_push(LONGSZ);
	i_ldr(0, src, REG_FP, tmp->addr, LONGSZ);
	regs[src] = NULL;
	tmp->loc = LOC_MEM;
}

static void num_cast(struct tmp *t, unsigned bt)
{
	if (!(bt & BT_SIGNED) && BT_SZ(bt) != LONGSZ)
		t->addr &= ((1l << (long) (BT_SZ(bt) * 8)) - 1);
	if (bt & BT_SIGNED && BT_SZ(bt) != LONGSZ &&
				t->addr > (1l << (BT_SZ(bt) * 8 - 1)))
		t->addr = -((1l << (BT_SZ(bt) * 8)) - t->addr);
}

static void tmp_reg(struct tmp *tmp, int dst, int deref)
{
	int bt = tmp->bt;
	if (!tmp->bt)
		deref = 0;
	if (deref)
		tmp->bt = 0;
	if (tmp->loc == LOC_NUM) {
		i_num(dst, tmp->addr);
		tmp->addr = dst;
		regs[dst] = tmp;
		tmp->loc = LOC_REG;
	}
	if (tmp->loc == LOC_SYM) {
		i_sym(dst, tmp->sym, tmp->off);
		tmp->addr = dst;
		regs[dst] = tmp;
		tmp->loc = LOC_REG;
	}
	if (tmp->loc == LOC_REG) {
		if (deref)
			i_ldr(1, dst, tmp->addr, 0, bt);
		else if (dst != tmp->addr)
			i_mov(dst, tmp->addr, LONGSZ);
		regs[tmp->addr] = NULL;
	}
	if (tmp->loc == LOC_LOCAL) {
		if (deref)
			i_ldr(1, dst, REG_FP, tmp->addr + tmp->off, bt);
		else
			i_add_anyimm(dst, REG_FP, tmp->addr + tmp->off);
	}
	if (tmp->loc == LOC_MEM) {
		i_ldr(1, dst, REG_FP, tmp->addr, LONGSZ);
		if (deref)
			i_ldr(1, dst, dst, 0, bt);
	}
	tmp->addr = dst;
	regs[dst] = tmp;
	tmp->loc = LOC_REG;
}

static void reg_free(int reg)
{
	int i;
	if (!regs[reg])
		return;
	for (i = 0; i < N_TMPS; i++)
		if (!regs[tmpregs[i]]) {
			tmp_reg(regs[reg], tmpregs[i], 0);
			return;
		}
	tmp_mem(regs[reg]);
}

static void reg_for(int reg, struct tmp *t)
{
	if (regs[reg] && regs[reg] != t)
		reg_free(reg);
}

static void tmp_mv(struct tmp *t, int reg)
{
	reg_for(reg, t);
	tmp_reg(t, reg, 0);
}

static void tmp_to(struct tmp *t, int reg)
{
	reg_for(reg, t);
	tmp_reg(t, reg, 1);
}

static void tmp_drop(int n)
{
	int i;
	for (i = ntmp - n; i < ntmp; i++)
		if (tmps[i].loc == LOC_REG)
			regs[tmps[i].addr] = NULL;
	ntmp -= n;
}

static void tmp_pop(int reg)
{
	struct tmp *t = TMP(0);
	tmp_to(t, reg);
	tmp_drop(1);
}

static struct tmp *tmp_new(void)
{
	return &tmps[ntmp++];
}

static void tmp_push(int reg)
{
	struct tmp *t = tmp_new();
	t->addr = reg;
	t->bt = 0;
	t->loc = LOC_REG;
	regs[reg] = t;
}

void o_local(long addr)
{
	struct tmp *t = tmp_new();
	t->addr = -addr;
	t->loc = LOC_LOCAL;
	t->bt = 0;
	t->off = 0;
}

void o_num(long num)
{
	struct tmp *t = tmp_new();
	t->addr = num;
	t->bt = 0;
	t->loc = LOC_NUM;
}

void o_sym(char *name)
{
	struct tmp *t = tmp_new();
	strcpy(t->sym, name);
	t->loc = LOC_SYM;
	t->bt = 0;
	t->off = 0;
}

void o_tmpdrop(int n)
{
	if (n == -1 || n > ntmp)
		n = ntmp;
	tmp_drop(n);
	if (!ntmp) {
		if (sp_tmp != -1)
			sp = sp_tmp;
		sp_tmp = -1;
	}
}

/* make sure tmps remain intact after a conditional expression */
void o_fork(void)
{
	int i;
	for (i = 0; i < ntmp - 1; i++)
		tmp_mem(&tmps[i]);
}

void o_forkpush(void)
{
	tmp_pop(REG_FORK);
}

void o_forkjoin(void)
{
	tmp_push(REG_FORK);
}

void o_tmpswap(void)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	struct tmp t;
	memcpy(&t, t1, sizeof(t));
	memcpy(t1, t2, sizeof(t));
	memcpy(t2, &t, sizeof(t));
	if (t1->loc == LOC_REG)
		regs[t1->addr] = t1;
	if (t2->loc == LOC_REG)
		regs[t2->addr] = t2;
}

static int reg_get(int mask)
{
	int i;
	for (i = 0; i < N_TMPS; i++)
		if ((1 << tmpregs[i]) & mask && !regs[tmpregs[i]])
			return tmpregs[i];
	for (i = 0; i < N_TMPS; i++)
		if ((1 << tmpregs[i]) & mask) {
			reg_free(tmpregs[i]);
			return tmpregs[i];
		}
	return 0;
}

static int reg_fortmp(struct tmp *t, int notmask)
{
	if (t->loc == LOC_REG && !(notmask & (1 << t->addr)))
		return t->addr;
	return reg_get(~notmask);
}

static void tmp_copy(struct tmp *t1)
{
	struct tmp *t2 = tmp_new();
	memcpy(t2, t1, sizeof(*t1));
	if (!(t1->loc & (LOC_REG | LOC_MEM)))
		return;
	if (t1->loc == LOC_MEM) {
		tmp_mv(t2, reg_get(~0));
	} else if (t1->loc == LOC_REG) {
		t2->addr = reg_fortmp(t2, 1 << t1->addr);
		i_mov(t2->addr, t1->addr, LONGSZ);
		regs[t2->addr] = t2;
	}
}

void o_tmpcopy(void)
{
	tmp_copy(TMP(0));
}

void o_cast(unsigned bt)
{
	struct tmp *t = TMP(0);
	if (!t->bt && t->loc == LOC_NUM) {
		num_cast(t, bt);
		return;
	}
	if (BT_SZ(bt) != LONGSZ) {
		int reg = reg_fortmp(t, BT_SZ(bt) > 1 ? 0 : ~R_BYTEREGS);
		tmp_to(t, reg);
		if (bt & BT_SIGNED)
			i_sx(reg, BT_SZ(bt) * 8);
		else
			i_zx(reg, BT_SZ(bt) * 8);
	}
}

void o_func_beg(char *name, int argc, int global, int vararg)
{
	out_sym(name, (global ? OUT_GLOB : 0) | OUT_CS, cslen, 0);
	i_prolog();
	sp = 3 * LONGSZ;
	sp_max = sp;
	ntmp = 0;
	sp_tmp = -1;
	nret = 0;
	memset(regs, 0, sizeof(regs));
}

void o_deref(unsigned bt)
{
	struct tmp *t = TMP(0);
	if (t->bt)
		tmp_to(t, reg_fortmp(t, 0));
	t->bt = bt;
}

void o_load(void)
{
	struct tmp *t = TMP(0);
	tmp_to(t, reg_fortmp(t, 0));
}

#define TMP_NUM(t)	((t)->loc == LOC_NUM && !(t)->bt)
#define LOCAL_PTR(t)	((t)->loc == LOC_LOCAL && !(t)->bt)
#define SYM_PTR(t)	((t)->loc == LOC_SYM && !(t)->bt)

int o_popnum(long *c)
{
	struct tmp *t = TMP(0);
	if (!TMP_NUM(t))
		return 1;
	*c = t->addr;
	tmp_drop(1);
	return 0;
}

void o_ret(int rets)
{
	if (rets)
		tmp_pop(REG_RET);
	else
		i_num(REG_RET, 0);
	ret[nret++] = o_jmp(0);
}

void o_func_end(void)
{
	int i;
	for (i = 0; i < nret; i++)
		o_filljmp(ret[i]);
	i_epilog();
}

long o_mklocal(int sz)
{
	return sp_push(ALIGN(sz, LONGSZ));
}

void o_rmlocal(long addr, int sz)
{
	sp = addr - ALIGN(sz, LONGSZ);
}

long o_arg2loc(int i)
{
	return -LONGSZ * (i + 2);
}

void o_assign(unsigned bt)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	int r1 = reg_fortmp(t1, BT_SZ(bt) > 1 ? 0 : ~R_BYTEREGS);
	int r2 = reg_fortmp(t2, 1 << r1);
	int off = 0;
	tmp_to(t1, r1);
	if (t2->bt)
		tmp_to(t2, r2);
	if (t2->loc == LOC_LOCAL) {
		r2 = REG_FP;
		off = t2->addr + t2->off;
	} else {
		tmp_to(t2, r2);
	}
	i_ldr(0, r1, r2, off, bt);
	tmp_drop(2);
	tmp_push(r1);
}

static long cu(int op, long i)
{
	switch (op & 0xff) {
	case O_NEG:
		return -i;
	case O_NOT:
		return ~i;
	case O_LNOT:
		return !i;
	}
	return 0;
}

static int c_uop(int op)
{
	struct tmp *t1 = TMP(0);
	if (!TMP_NUM(t1))
		return 1;
	tmp_drop(1);
	o_num(cu(op, t1->addr));
	return 0;
}

static long cb(int op, long a, long b)
{
	switch (op & 0xff) {
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
		if (op & O_SIGNED)
			return a >> b;
		else
			return (unsigned long) a >> b;
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
	case O_NEQ:
		return a != b;
	}
	return 0;
}

static int c_bop(int op)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	int locs = LOCAL_PTR(t1) + LOCAL_PTR(t2);
	int syms = SYM_PTR(t1) + SYM_PTR(t2);
	int nums = TMP_NUM(t1) + TMP_NUM(t2);
	if (syms + locs == 2 || syms + nums + locs != 2)
		return 1;
	if (nums == 1)
		if ((op & 0xff) != O_ADD && ((op & 0xff) != O_SUB || TMP_NUM(t2)))
			return 1;
	if (nums == 1) {
		long o1 = TMP_NUM(t1) ? t1->addr : t1->off;
		long o2 = TMP_NUM(t2) ? t2->addr : t2->off;
		long ret = cb(op, o2, o1);
		if (!TMP_NUM(t1))
			o_tmpswap();
		t2->off = ret;
		tmp_drop(1);
	} else {
		long ret = cb(op, t2->addr, t1->addr);
		tmp_drop(2);
		o_num(ret);
	}
	return 0;
}

void o_uop(int op)
{
	int r1 = (op & 0xff) == O_LNOT ? R_RAX : reg_fortmp(TMP(0), 0);
	if (!c_uop(op))
		return;
	tmp_to(TMP(0), r1);
	switch (op & 0xff) {
	case O_NEG:
		i_neg(r1);
		break;
	case O_NOT:
		i_not(r1);
		break;
	case O_LNOT:
		i_lnot(r1);
		break;
	}
}

static void bin_regs(int *r1, int *r2, int mask1, int mask2)
{
	struct tmp *t2 = TMP(0);
	struct tmp *t1 = TMP(1);
	*r2 = reg_fortmp(t2, ~mask1);
	tmp_to(t2, *r2);
	*r1 = reg_fortmp(t1, ~mask2 | (1 << *r2));
	tmp_pop(*r2);
	tmp_pop(*r1);
}

static int bop_imm(int *r1, long *n, int swap)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	if (!TMP_NUM(t1) && (!swap || !TMP_NUM(t2)))
		return 1;
	*n = TMP_NUM(t1) ? t1->addr : t2->addr;
	if (!i_decodeable(*n))
		return 1;
	if (!TMP_NUM(t1))
		o_tmpswap();
	*r1 = reg_fortmp(t2, 0);
	tmp_drop(1);
	tmp_pop(*r1);
	return 0;
}

static void bin_add(int op)
{
	int r1, r2;
	long n;
	if (!bop_imm(&r1, &n, (op & 0xff) != O_SUB)) {
		i_add_imm(op, r1, r1, n);
	} else {
		bin_regs(&r1, &r2, R_TMPS, R_TMPS);
		i_add(op, r1, r1, r2);
	}
	tmp_push(r1);
}

static void bin_shx(int op)
{
	int r1, r2;
	long n;
	if (!bop_imm(&r1, &n, 0)) {
		i_shl_imm(op, r1, r1, n);
	} else {
		bin_regs(&r1, &r2, 1 << R_RCX, R_TMPS);
		i_shl(op, r1, r1, r2);
	}
	tmp_push(r1);
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

/* optimized version of mul/div/mod for powers of two */
static int mul_2(int op)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	long n;
	int r2;
	int p;
	if ((op & 0xff) == O_MUL && t2->loc == LOC_NUM && !t2->bt)
		o_tmpswap();
	if (t1->loc != LOC_NUM || t1->bt)
		return 1;
	n = t1->addr;
	p = log2a(n);
	if (n && p == -1)
		return 1;
	if ((op & 0xff) == O_MUL) {
		tmp_drop(1);
		if (n == 1)
			return 0;
		if (n == 0) {
			tmp_drop(1);
			o_num(0);
			return 0;
		}
		r2 = reg_fortmp(t2, 0);
		tmp_to(t2, r2);
		i_shl_imm(O_SHL, r2, r2, p);
		return 0;
	}
	if (op == O_DIV) {
		tmp_drop(1);
		if (n == 1)
			return 0;
		r2 = reg_fortmp(t2, 0);
		tmp_to(t2, r2);
		i_shl_imm((op & O_SIGNED) | O_SHR, r2, r2, p);
		return 0;
	}
	if (op == O_MOD) {
		tmp_drop(1);
		if (n == 1) {
			tmp_drop(1);
			o_num(0);
			return 0;
		}
		r2 = reg_fortmp(t2, 0);
		tmp_to(t2, r2);
		i_zx(r2, p);
		return 0;
	}
	return 1;
}

static void mulop(int *r1, int *r2, int rop)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	if (t1->loc & LOC_REG && t1->addr != R_RAX && t1->addr != R_RDX)
		rop = t1->addr;
	tmp_to(t1, rop);
	tmp_to(t2, R_RAX);
	if (rop != R_RDX)
		reg_free(R_RDX);
	tmp_drop(2);
	*r1 = rop;
	*r2 = R_RAX;
}

static void bin_mul(int op)
{
	int r1, r2;
	if (!mul_2(op))
		return;
	mulop(&r1, &r2, (op & 0xff) == O_MUL ? R_RDX : R_RCX);
	if ((op & 0xff) == O_MUL) {
		i_mul(R_RAX, r1, r2);
		tmp_push(R_RAX);
	}
	if ((op & 0xff) == O_DIV) {
		i_div(op, R_RAX, r1, r2);
		tmp_push(R_RAX);
	}
	if ((op & 0xff) == O_MOD) {
		i_div(op, R_RDX, r1, r2);
		tmp_push(R_RDX);
	}
}

static void bin_cmp(int op)
{
	int r1, r2;
	long n;
	if (!bop_imm(&r1, &n, (op & 0xff) == O_EQ || (op & 0xff) == O_NEQ)) {
		i_cmp_imm(r1, n);
	} else {
		bin_regs(&r1, &r2, R_TMPS, R_TMPS);
		i_cmp(r1, r2);
	}
	r1 = R_RAX;
	reg_free(r1);
	i_set(op, r1);
	tmp_push(r1);
}

void o_bop(int op)
{
	if (!c_bop(op))
		return;
	if ((op & 0xf0) == 0x00)
		bin_add(op);
	if ((op & 0xf0) == 0x10)
		bin_shx(op);
	if ((op & 0xf0) == 0x20)
		bin_mul(op);
	if ((op & 0xf0) == 0x30)
		bin_cmp(op);
}

void o_memcpy(void)
{
	struct tmp *t0 = TMP(0);
	struct tmp *t1 = TMP(1);
	struct tmp *t2 = TMP(2);
	tmp_to(t0, R_RCX);
	tmp_to(t1, R_RSI);
	tmp_to(t2, R_RDI);
	os("\xfc\xf3\xa4", 3);		/* cld; rep movs */
	tmp_drop(2);
}

void o_memset(void)
{
	struct tmp *t0 = TMP(0);
	struct tmp *t1 = TMP(1);
	struct tmp *t2 = TMP(2);
	tmp_to(t0, R_RCX);
	tmp_to(t1, R_RAX);
	tmp_to(t2, R_RDI);
	os("\xfc\xf3\xaa", 3);		/* cld; rep stosb */
	tmp_drop(2);
}

long o_mklabel(void)
{
	return cslen;
}

static long jxz(long addr, int z)
{
	int r = reg_fortmp(TMP(0), 0);
	tmp_pop(r);
	i_b_if(addr, r, z);
	return cslen - 4;
}

long o_jz(long addr)
{
	return jxz(addr, 1);
}

long o_jnz(long addr)
{
	return jxz(addr, 0);
}

long o_jmp(long addr)
{
	i_b(addr);
	return cslen - 4;
}

void o_filljmp2(long addr, long jmpdst)
{
	i_b_fill((void *) cs + addr, jmpdst - addr);
}

void o_filljmp(long addr)
{
	o_filljmp2(addr, cslen);
}

void o_call(int argc, int rets)
{
	struct tmp *t;
	int i;
	for (i = 0; i < N_TMPS; i++)
		if (regs[tmpregs[i]] && regs[tmpregs[i]] - tmps < ntmp - argc)
			tmp_mem(regs[tmpregs[i]]);
	sp_push(LONGSZ * argc);
	for (i = argc - 1; i >= 0; --i) {
		int reg = reg_fortmp(TMP(0), 0);
		tmp_pop(reg);
		i_ldr(0, reg, REG_SP, i * LONGSZ, LONGSZ);
	}
	t = TMP(0);
	if (t->loc == LOC_SYM && !t->bt) {
		i_call(t->sym, t->off);
		tmp_drop(1);
	} else {
		int reg = reg_fortmp(t, 0);
		tmp_pop(reg);
		i_call_reg(reg);
	}
	if (rets)
		tmp_push(REG_RET);
}

void o_mkbss(char *name, int size, int global)
{
	out_sym(name, OUT_BSS | (global ? OUT_GLOB : 0), bsslen, size);
	bsslen += ALIGN(size, OUT_ALIGNMENT);
}

#define MAXDATS		(1 << 10)
static char dat_names[MAXDATS][NAMELEN];
static int dat_offs[MAXDATS];
static int ndats;

void err(char *msg);
void *o_mkdat(char *name, int size, int global)
{
	void *addr = ds + dslen;
	int idx = ndats++;
	if (idx >= MAXDATS)
		err("nomem: MAXDATS reached!\n");
	strcpy(dat_names[idx], name);
	dat_offs[idx] = dslen;
	out_sym(name, OUT_DS | (global ? OUT_GLOB : 0), dslen, size);
	dslen += ALIGN(size, OUT_ALIGNMENT);
	return addr;
}

static int dat_off(char *name)
{
	int i;
	for (i = 0; i < ndats; i++)
		if (!strcmp(name, dat_names[i]))
			return dat_offs[i];
	return 0;
}

void o_datset(char *name, int off, unsigned bt)
{
	struct tmp *t = TMP(0);
	int sym_off = dat_off(name) + off;
	if (t->loc == LOC_NUM && !t->bt) {
		num_cast(t, bt);
		memcpy(ds + sym_off, &t->addr, BT_SZ(bt));
	}
	if (t->loc == LOC_SYM && !t->bt) {
		out_rel(t->sym, OUT_DS, sym_off);
		memcpy(ds + sym_off, &t->off, BT_SZ(bt));
	}
	tmp_drop(1);
}

void o_write(int fd)
{
	out_write(fd, cs, cslen, ds, dslen);
}

/* X86 arch specific functions */

#define I_MOV		0x89
#define I_MOVI		0xc7
#define I_MOVIR		0xb8
#define I_MOVR		0x8b
#define I_SHX		0xd3
#define I_CMP		0x3b
#define I_TST		0x85
#define I_LEA		0x8d
#define I_NOT		0xf7
#define I_CALL		0xff
#define I_MUL		0xf7
#define I_XOR		0x33
#define I_TEST		0x85
#define I_CQO		0x99
#define I_PUSH		0x50
#define I_POP		0x58

#define OP2(o2, o1)		(0x010000 | ((o2) << 8) | (o1))
#define O2(op)			(((op) >> 8) & 0xff)
#define O1(op)			((op) & 0xff)
#define MODRM(m, r1, r2)	((m) << 6 | (r1) << 3 | (r2))

/* for optimizing cmp + jmp */
#define OPT_ISCMP()		(last_set + 6 == cslen)
#define OPT_CCOND()		(cs[last_set + 1])

static long last_set = -1;

static void op_x(int op, int r1, int r2, int bt)
{
	int sz = BT_SZ(bt);
	if (sz == 2)
		oi(0x66, 1);
	if (op & 0x10000)
		oi(O2(op), 1);
	oi(sz == 1 ? O1(op) & ~0x1 : O1(op), 1);
}

#define op_mr		op_rm

/* op_*(): r=reg, m=mem, i=imm, s=sym */
static void op_rm(int op, int src, int base, int off, int bt)
{
	int dis = off == (char) off ? 1 : 4;
	int mod = dis == 4 ? 2 : 1;
	if (!off && (base & 7) != R_RBP)
		mod = 0;
	op_x(op, src, base, bt);
	oi(MODRM(mod, src & 0x07, base & 0x07), 1);
	if ((base & 7) == R_RSP)
		oi(0x24, 1);
	if (mod)
		oi(off, dis);
}

static void op_rr(int op, int src, int dst, int bt)
{
	op_x(op, src, dst, bt);
	oi(MODRM(3, src & 0x07, dst & 0x07), 1);
}

#define movrx_bt(bt)		(LONGSZ)

static int movrx_op(int bt, int mov)
{
	int sz = BT_SZ(bt);
	if (sz == 2)
		return OP2(0x0f, bt & BT_SIGNED ? 0xbf : 0xb7);
	if (sz == 1)
		return OP2(0x0f, bt & BT_SIGNED ? 0xbe : 0xb6);
	return mov;
}

static void mov_r2r(int r1, int r2, unsigned bt)
{
	if (r1 != r2 || BT_SZ(bt) != LONGSZ)
		op_rr(movrx_op(bt, I_MOV), r1, r2, movrx_bt(bt));
}

static void mov_m2r(int dst, int base, int off, int bt)
{
	op_rm(movrx_op(bt, I_MOVR), dst, base, off, movrx_bt(bt));
}

static void i_zx(int rd, int bits)
{
	if (bits & 0x07) {
		i_shl_imm(O_SHL, rd, rd, LONGSZ * 8 - bits);
		i_shl_imm(O_SHR, rd, rd, LONGSZ * 8 - bits);
	} else {
		mov_r2r(rd, rd, bits >> 3);
	}
}

static void i_sx(int rd, int bits)
{
	mov_r2r(rd, rd, BT_SIGNED | (bits >> 3));
}

static void i_add(int op, int rd, int rn, int rm)
{
	/* opcode for O_ADD, O_SUB, O_AND, O_OR, O_XOR */
	static int rx[] = {0003, 0053, 0043, 0013, 0063};
	if (rn != rd)
		die("this is cisc!\n");
	op_rr(rx[op & 0x0f], rd, rm, LONGSZ);
}

static void i_add_imm(int op, int rd, int rn, long n)
{
	/* opcode for O_ADD, O_SUB, O_AND, O_OR, O_XOR */
	static int rx[] = {0xc0, 0xe8, 0xe0, 0xc8, 0xf0};
	unsigned char s[3] = {0x83, rx[op & 0x0f] | rd, n & 0xff};
	if (rn != rd)
		die("this is cisc!\n");
	os((void *) s, 3);
}

static int i_decodeable(long imm)
{
	return imm <= 127 && imm >= -128;
}

static void i_num(int rd, long n)
{
	if (!n) {
		op_rr(I_XOR, rd, rd, 4);
	} else {
		op_x(I_MOVIR + (rd & 7), 0, rd, LONGSZ);
		oi(n, LONGSZ);
	}
}

static void i_add_anyimm(int rd, int rn, long n)
{
	op_rm(I_LEA, rd, rn, n, LONGSZ);
}

static void i_mul(int rd, int rn, int rm)
{
	if (rn != R_RDX)
		i_num(R_RDX, 0);
	op_rr(I_MUL, 4, rn, LONGSZ);
}

static void i_div(int op, int rd, int rn, int rm)
{
	if (rn != R_RDX) {
		if (op & O_SIGNED)
			op_x(I_CQO, R_RAX, R_RDX, LONGSZ);
		else
			i_num(R_RDX, 0);
	}
	op_rr(I_MUL, op & O_SIGNED ? 7 : 6, rn, LONGSZ);
}

static void i_tst(int rn, int rm)
{
	op_rr(I_TST, rn, rm, LONGSZ);
}

static void i_cmp(int rn, int rm)
{
	op_rr(I_CMP, rn, rm, LONGSZ);
}

static void i_cmp_imm(int rn, long n)
{
	unsigned char s[3] = {0x83, 0xf8 | rn, n & 0xff};
	os(s, 3);
}

static void i_set(int op, int rd)
{
	/* lt, gt, le, ge, eq, neq */
	static int ucond[] = {0x92, 0x97, 0x96, 0x93, 0x94, 0x95};
	static int scond[] = {0x9c, 0x9f, 0x9e, 0x9d, 0x94, 0x95};
	int cond = op & O_SIGNED ? scond[op & 0x0f] : ucond[op & 0x0f];
	char set[] = "\x0f\x00\xc0";
	if (rd != R_RAX)
		die("set works only with R_RAX\n");
	set[1] = cond;
	last_set = cslen;
	os(set, 3);			/* setl al */
	os("\x0f\xb6\xc0", 3);		/* movzbl eax, al */
}

static void i_shl(int op, int rd, int rm, int rs)
{
	int sm = 4;
	if ((op & 0x0f) == 1)
		sm = op & O_SIGNED ? 7 : 5;
	if (rd != rm)
		die("this is cisc!\n");
	op_rr(I_SHX, sm, rd, LONGSZ);
}

static void i_shl_imm(int op, int rd, int rn, long n)
{
	int sm = (op & 0x1) ? (op & O_SIGNED ? 0xf8 : 0xe8) : 0xe0 ;
	char s[3] = {0xc1, sm | rn, n & 0xff};
	if (rd != rn)
		die("this is cisc!\n");
	os(s, 3);
}

static void i_mov(int rd, int rn, int bt)
{
	op_rr(movrx_op(bt, I_MOVR), rd, rn, movrx_bt(bt));
}

static void i_ldr(int l, int rd, int rn, int off, int bt)
{
	if (l)
		mov_m2r(rd, rn, off, bt);
	else
		op_rm(I_MOV, rd, rn, off, bt);
}

static void i_sym(int rd, char *sym, int off)
{
	op_x(I_MOVIR + (rd & 7), 0, rd, LONGSZ);
	out_rel(sym, OUT_CS, cslen);
	oi(off, LONGSZ);
}

static void i_neg(int rd)
{
	op_rr(I_NOT, 3, rd, LONGSZ);
}

static void i_not(int rd)
{
	op_rr(I_NOT, 2, rd, LONGSZ);
}

static void i_lnot(int rd)
{
	if (OPT_ISCMP()) {
		cs[last_set + 1] ^= 0x01;
	} else {
		char cmp[] = "\x83\xf8\x00";
		cmp[1] |= rd;
		os(cmp, 3);		/* cmp eax, 0 */
		i_set(O_EQ, rd);
	}
}

static void jx(int x, long addr)
{
	char op[2] = {0x0f};
	op[1] = x;
	os(op, 2);		/* jx $addr */
	oi(addr - cslen - 4, 4);
}

static void i_b_if(long addr, int rn, int z)
{
	if (OPT_ISCMP()) {
		int cond = OPT_CCOND();
		cslen = last_set;
		jx((!z ? cond : cond ^ 0x01) & ~0x10, addr);
		last_set = -1;
	} else {
		i_tst(rn, rn);
		jx(z ? 0x84 : 0x85, addr);
	}
}

static void i_b(long addr)
{
	os("\xe9", 1);			/* jmp $addr */
	oi(addr - cslen - 4, 4);
}

static void i_b_fill(long *dst, int diff)
{
	putint((void *) dst, diff - 4, 4);
}

static void i_call_reg(int rd)
{
	op_rr(I_CALL, 2, rd, LONGSZ);
}

static void i_call(char *sym, int off)
{
	os("\xe8", 1);		/* call $x */
	out_rel(sym, OUT_CS | OUT_REL, cslen);
	oi(-4 + off, 4);
}

static void i_prolog(void)
{
	last_set = -1;
	os("\x55", 1);			/* push rbp */
	os("\x89\xe5", 2);		/* mov rbp, rsp */
	os("\x53\x56\x57", 3);		/* push rbx; push rsi; push rdi */
	os("\x81\xec", 2);		/* sub rsp, $xxx */
	func_fpsub = cslen;
	oi(0, 4);
}

static void i_epilog(void)
{
	int diff = ALIGN(sp_max - 3 * LONGSZ, LONGSZ);
	if (diff) {
		os("\x81\xc4", 2);		/* add $xxx, %esp */
		oi(diff, 4);
		putint(cs + func_fpsub, diff, 4);
	}
	os("\x5f\x5e\x5b", 3);		/* pop edi; pop esi; pop ebx */
	os("\xc9\xc3", 2);		/* leave; ret; */
}
