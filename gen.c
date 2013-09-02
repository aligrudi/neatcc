#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "gen.h"
#include "ncc.h"
#include "out.h"
#include "reg.h"
#include "tok.h"

/* variable location */
#define LOC_REG		0x01
#define LOC_MEM		0x02
#define LOC_NUM		0x04
#define LOC_SYM		0x08
#define LOC_LOCAL	0x10

#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))

char cs[SECLEN];		/* code segment */
int cslen;
static char ds[SECLEN];		/* data segment */
static int dslen;
static long bsslen;		/* bss segment size */

static long sp;			/* stack pointer offset from R_RBP */
static long sp_max;		/* maximum stack pointer offset */
static long sp_tmp;		/* sp for the first tmp on the stack */
static int localoff[NLOCALS];	/* the offset of locals on the stack */
static int nlocals;		/* number of locals */

/* function info */
static int func_beg;
static int func_argc;
static int func_varg;

/* function statistics */
int pass1;			/* collect statistics; 1st pass */
static int stat_calls;		/* # of function calls */
static int stat_tmps;		/* # of stack temporaries  */
static int stat_regs;		/* mask of used registers */

/* optimization info */
static int pass2;		/* use the collected statistics in the 1st pass */
static int tmp_mask;		/* registers that can be used for tmps */

/* register allocation for locals */
#define TMP_ISLREG(t)	(!(t)->bt && (t)->loc == LOC_LOCAL && r_regmap((t)->id) >= 0)
#define TMP_LREG(t)	(r_regmap((t)->id))

#define TMP(i)		(((i) < ntmp) ? &tmps[ntmp - 1 - (i)] : NULL)

static struct tmp {
	long addr;
	char sym[NAMELEN];
	long off;	/* offset from a symbol or a local */
	unsigned loc;	/* variable location */
	unsigned bt;	/* type of address; zero when not a pointer */
	int id;		/* local variable id */
} tmps[NTMPS];
static int ntmp;

static struct tmp *regs[N_REGS];

/* labels and jmps */
static long labels[NJMPS];
static int nlabels;
static long jmp_loc[NJMPS];
static int jmp_goal[NJMPS];
static int jmp_len[NJMPS];
static int njmps;

void o_label(int id)
{
	r_label(id);
	if (id > nlabels)
		nlabels = id + 1;
	labels[id] = cslen;
}

/* the number of bytes needed for holding jmp displacement */
static int jmp_sz(int id)
{
	long n = jmp_len[id] > 0 ? jmp_len[id] : -jmp_len[id];
	if (!pass2)
		return 4;
	if (n < 0x70)
		return n == 0 ? 0 : 1;
	return n < 0x7000 ? 2 : 4;
}

static void jmp_add(int id, int rn, int z)
{
	r_jmp(id);
	if (njmps >= NJMPS)
		err("nomem: NJMPS reached!\n");
	i_jmp(rn, z, jmp_sz(njmps));
	jmp_loc[njmps] = cslen;
	jmp_goal[njmps] = id;
	njmps++;
}

static void jmp_fill(void)
{
	int i;
	for (i = 0; i < njmps; i++)
		jmp_len[i] = i_fill(jmp_loc[i], labels[jmp_goal[i]], jmp_sz(i));
}

/* generating code */

void os(void *s, int n)
{
	while (n--)
		cs[cslen++] = *(char *) (s++);
}

void oi(long n, int l)
{
	while (l--) {
		cs[cslen++] = n;
		n >>= 8;
	}
}

static long sp_push(int sz)
{
	sp -= ALIGN(sz, LONGSZ);
	if (sp < sp_max)
		sp_max = sp;
	return sp;
}

static void tmp_mem(struct tmp *tmp)
{
	int src = tmp->addr;
	if (tmp->loc != LOC_REG || (1 << src) & r_lregs())
		return;
	if (sp_tmp == -1)
		sp_tmp = sp;
	tmp->addr = sp_push(LONGSZ);
	i_save(src, REG_FP, tmp->addr, LONGSZ);
	stat_tmps++;
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
			i_load(dst, tmp->addr, 0, bt);
		else if (dst != tmp->addr)
			i_mov(dst, tmp->addr);
		regs[tmp->addr] = NULL;
	}
	if (tmp->loc == LOC_LOCAL) {
		if (deref)
			r_read(tmp->id);
		else
			r_addr(tmp->id);
		if (deref)
			i_load(dst, REG_FP, tmp->addr + tmp->off, bt);
		else
			i_op_imm(O_ADD, dst, REG_FP, tmp->addr + tmp->off);
	}
	if (tmp->loc == LOC_MEM) {
		i_load(dst, REG_FP, tmp->addr, LONGSZ);
		if (deref)
			i_load(dst, dst, 0, bt);
	}
	tmp->addr = dst;
	stat_regs |= 1 << dst;
	regs[dst] = tmp;
	tmp->loc = LOC_REG;
}

/* empty the given register, but never touch the registers in rsrvd mask */
static void reg_free(int reg, int rsrvd)
{
	int i;
	if (!regs[reg])
		return;
	rsrvd |= ~tmp_mask;
	for (i = 0; i < N_TMPS; i++)
		if (!regs[tmpregs[i]] && ~rsrvd & (1 << tmpregs[i])) {
			tmp_reg(regs[reg], tmpregs[i], 0);
			return;
		}
	tmp_mem(regs[reg]);
}

static void reg_for(int reg, struct tmp *t)
{
	if (regs[reg] && regs[reg] != t)
		reg_free(reg, 0);
}

static void tmp_mv(struct tmp *t, int reg)
{
	reg_for(reg, t);
	tmp_reg(t, reg, 0);
}

static void tmp_to(struct tmp *t, int reg)
{
	reg_for(reg, t);
	if (t->loc == LOC_LOCAL && TMP_ISLREG(t)) {
		t->loc = LOC_REG;
		t->addr = TMP_LREG(t);
		t->bt = 0;
	}
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
	stat_regs |= 1 << reg;
	t->loc = LOC_REG;
	regs[reg] = t;
}

void o_local(long addr)
{
	struct tmp *t = tmp_new();
	t->addr = localoff[addr];
	t->id = addr;
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
	mask &= tmp_mask;
	for (i = 0; i < N_TMPS; i++)
		if ((1 << tmpregs[i]) & mask && !regs[tmpregs[i]]) {
			stat_regs |= 1 << tmpregs[i];
			return tmpregs[i];
		}
	for (i = 0; i < N_TMPS; i++)
		if ((1 << tmpregs[i]) & mask) {
			reg_free(tmpregs[i], 0);
			stat_regs |= 1 << tmpregs[i];
			return tmpregs[i];
		}
	die("reg_get: out of registers!\n");
	return 0;
}

static int reg_tmp(struct tmp *t, int mask, int readonly)
{
	if (t->loc == LOC_REG && (mask & (1 << t->addr)))
		if (!(r_lregs() & (1 << t->addr)) || (readonly && !t->bt))
			return t->addr;
	return reg_get(mask);
}

static int reg_tmpn(struct tmp *t, int notmask, int readonly)
{
	if (t->loc == LOC_REG && !(notmask & (1 << t->addr)))
		if (!(r_lregs() & (1 << t->addr)) || (readonly && !t->bt))
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
		tmp_mv(t2, reg_get(R_TMPS));
	} else if (t1->loc == LOC_REG) {
		t2->addr = reg_tmpn(t2, 1 << t1->addr, 0);
		i_mov(t2->addr, t1->addr);
		regs[t2->addr] = t2;
		stat_regs |= 1 << t2->addr;
	}
}

void o_tmpcopy(void)
{
	tmp_copy(TMP(0));
}

void o_deref(unsigned bt)
{
	struct tmp *t = TMP(0);
	if (TMP_ISLREG(t)) {
		t->loc = LOC_REG;
		t->addr = TMP_LREG(t);
	} else {
		if (t->bt)
			tmp_to(t, reg_tmp(t, R_TMPS, 0));
		t->bt = bt;
	}
}

void o_load(void)
{
	struct tmp *t = TMP(0);
	tmp_to(t, reg_tmp(t, R_TMPS, 0));
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
	o_jmp(0);
}

long o_mklocal(int sz)
{
	r_mk(sz);
	localoff[nlocals] = sp_push(ALIGN(sz, LONGSZ));
	return nlocals++;
}

void o_rmlocal(long addr, int sz)
{
	r_rm(addr);
}

long o_arg2loc(int i)
{
	return i;
}

#define MOVXX(bt)	((BT_SZ(bt) == LONGSZ ? O_MOV : ((bt) & BT_SIGNED ? O_SX : O_ZX)))

void o_assign(unsigned bt)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	int r1 = reg_tmp(t1, BT_SZ(bt) > 1 ? R_TMPS : R_BYTE, 1);
	int r2 = reg_tmpn(t2, 1 << r1, 1);
	int off = 0;
	tmp_to(t1, r1);
	if (TMP_ISLREG(t2)) {
		i_op_imm(MOVXX(bt), TMP_LREG(t2), r1, BT_SZ(bt) * 8);
		goto done;
	}
	if (t2->bt)
		tmp_to(t2, r2);
	if (t2->loc == LOC_LOCAL) {
		r2 = REG_FP;
		off = t2->addr + t2->off;
		r_read(t2->id);
	} else {
		tmp_to(t2, r2);
	}
	i_save(r1, r2, off, bt);
done:
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
		if (t2->loc == LOC_LOCAL)
			r_addr(t2->id);
		t2->off = ret;
		tmp_drop(1);
	} else {
		long ret = cb(op, t2->addr, t1->addr);
		tmp_drop(2);
		o_num(ret);
	}
	return 0;
}

/* allocate registers for the given binary or unary instruction */
static void regs2(int op, int *rd, int *r1, int *r2)
{
	int md, m1, m2, mt;
	int all = 0;
	int i;
	i_reg(op, &md, &m1, &m2, &mt);
	if (m2) {
		struct tmp *t2 = TMP(0);
		*r2 = reg_tmp(t2, m2, 1);
		tmp_to(t2, *r2);
		all |= (1 << *r2);
	}
	if (m1) {
		struct tmp *t1 = TMP(m2 ? 1 : 0);
		*r1 = reg_tmp(t1, m1 & ~all, md ? 1 : 0);
		tmp_to(t1, *r1);
		all |= (1 << *r1);
	}
	if (md) {
		if (m2 && md & tmp_mask & (1 << *r2))
			*rd = *r2;
		else if (m1 && md & tmp_mask & (1 << *r1))
			*rd = *r1;
		else
			*rd = reg_get(md & ~all);
		all |= (1 << *rd);
	} else {
		*rd = *r1;
	}
	if (mt & ~all) {
		for (i = 0; i < N_TMPS; i++)
			if (mt & ~all & (1 << tmpregs[i]))
				reg_free(tmpregs[i], all | mt);
	}
	stat_regs |= mt;
	tmp_drop(m2 ? 2 : 1);
}

/* allocate registers for a 3 operand instruction */
static void regs3(int op, int *r0, int *r1, int *r2)
{
	int m0, m1, m2, mt;
	struct tmp *t0 = TMP(2);
	struct tmp *t1 = TMP(1);
	struct tmp *t2 = TMP(0);
	int all = 0;
	int i;
	i_reg(op, &m0, &m1, &m2, &mt);
	if (m2) {
		*r2 = reg_tmp(t2, m2, 1);
		tmp_to(t2, *r2);
		all |= (1 << *r2);
	}
	if (m1) {
		*r1 = reg_tmp(t1, m1 & ~(1 << *r2), 1);
		tmp_to(t1, *r1);
		all |= (1 << *r1);
	}
	if (m0) {
		*r0 = reg_tmp(t0, m0 & ~((1 << *r2) | (1 << *r1)), 1);
		tmp_to(t0, *r0);
		all |= (1 << *r0);
	}
	if (mt & ~all) {
		for (i = 0; i < N_TMPS; i++)
			if (mt & ~all & (1 << tmpregs[i]))
				reg_free(tmpregs[i], all | mt);
	}
	stat_regs |= mt;
	tmp_drop(3);
}

static void op_imm(int op, long n)
{
	int rd, r1, r2;
	regs2(op | O_IMM, &rd, &r1, &r2);
	i_op_imm(op | O_IMM, rd, r1, n);
	tmp_push(rd);
}

void o_uop(int op)
{
	int rd, r1, r2;
	if (!c_uop(op))
		return;
	regs2(op, &rd, &r1, &r2);
	i_op(op, rd, r1, r2);
	tmp_push(rd);
}

static int bop_imm(int op, long *n, int swap)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	if (!TMP_NUM(t1) && (!swap || !TMP_NUM(t2)))
		return 1;
	*n = TMP_NUM(t1) ? t1->addr : t2->addr;
	if (!i_imm(op, *n))
		return 1;
	if (!TMP_NUM(t1))
		o_tmpswap();
	tmp_drop(1);
	return 0;
}

static void bin_op(int op, int swap)
{
	int rd, r1, r2;
	long n;
	if (!bop_imm(op, &n, swap)) {
		regs2(op | O_IMM, &rd, &r1, &r2);
		i_op_imm(op, rd, r1, n);
	} else {
		regs2(op, &rd, &r1, &r2);
		i_op(op, rd, r1, r2);
	}
	tmp_push(rd);
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
		op_imm(O_SHL, p);
		return 0;
	}
	if (op == O_DIV) {
		tmp_drop(1);
		if (n == 1)
			return 0;
		op_imm((op & O_SIGNED) | O_SHR, p);
		return 0;
	}
	if (op == O_MOD) {
		tmp_drop(1);
		if (n == 1) {
			tmp_drop(1);
			o_num(0);
			return 0;
		}
		op_imm(O_ZX, p);
		return 0;
	}
	return 1;
}

void o_bop(int op)
{
	if (!c_bop(op))
		return;
	if ((op & 0xf0) == 0x00)	/* add */
		bin_op(op, (op & 0xff) != O_SUB);
	if ((op & 0xf0) == 0x10)	/* shx */
		bin_op(op, 0);
	if ((op & 0xf0) == 0x20) {	/* mul */
		if (!mul_2(op))
			return;
		bin_op(op, (op & 0xff) == O_MUL);
	}
	if ((op & 0xf0) == 0x30)
		bin_op(op, (op & 0xff) == O_EQ || (op & 0xff) == O_NEQ);
}

void o_memcpy(void)
{
	int r0, r1, r2;
	regs3(O_MCPY, &r0, &r1, &r2);
	i_memcpy(r0, r1, r2);
}

void o_memset(void)
{
	int r0, r1, r2;
	regs3(O_MSET, &r0, &r1, &r2);
	i_memset(r0, r1, r2);
}

void o_cast(unsigned bt)
{
	struct tmp *t = TMP(0);
	if (!t->bt && t->loc == LOC_NUM) {
		num_cast(t, bt);
		return;
	}
	if (BT_SZ(bt) != LONGSZ)
		op_imm(MOVXX(bt), BT_SZ(bt) * 8);
}

static void jxz(int id, int z)
{
	int r = reg_tmp(TMP(0), R_TMPS, 1);
	tmp_pop(r);
	jmp_add(id, r, z);
}

void o_jz(int id)
{
	jxz(id, 1);
}

void o_jnz(int id)
{
	jxz(id, 0);
}

void o_jmp(int id)
{
	jmp_add(id, -1, 0);
}

void o_call(int argc, int rets)
{
	struct tmp *t;
	int i;
	int aregs = MIN(N_ARGS, argc);
	for (i = 0; i < N_TMPS; i++)
		if (regs[tmpregs[i]] && regs[tmpregs[i]] - tmps < ntmp - argc)
			tmp_mem(regs[tmpregs[i]]);
	if (argc > aregs) {
		sp_push(LONGSZ * (argc - aregs));
		for (i = argc - 1; i >= aregs; --i) {
			int reg = reg_tmp(TMP(0), R_TMPS, 1);
			tmp_pop(reg);
			i_save(reg, REG_SP, (i - aregs) * LONGSZ, LONGSZ);
		}
	}
	for (i = aregs - 1; i >= 0; --i)
		tmp_to(TMP(aregs - i - 1), argregs[i]);
	tmp_drop(aregs);
	t = TMP(0);
	if (t->loc == LOC_SYM && !t->bt) {
		i_call(t->sym, t->off);
		tmp_drop(1);
	} else {
		int reg = reg_tmp(t, R_TMPS, 1);
		tmp_pop(reg);
		i_call_reg(reg);
	}
	if (rets)
		tmp_push(REG_RET);
	stat_calls++;
}

void o_mkbss(char *name, int size, int global)
{
	if (pass1)
		return;
	out_sym(name, OUT_BSS | (global ? OUT_GLOB : 0), bsslen, size);
	bsslen += ALIGN(size, OUT_ALIGNMENT);
}

static char dat_names[NDATS][NAMELEN];
static int dat_offs[NDATS];
static int ndats;

void *o_mkdat(char *name, int size, int global)
{
	void *addr = ds + dslen;
	int idx;
	if (pass1)
		return addr;
	idx = ndats++;
	if (idx >= NDATS)
		err("nomem: NDATS reached!\n");
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
	if (pass1) {
		tmp_drop(1);
		return;
	}
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
	i_done();
	out_write(fd, cs, cslen, ds, dslen);
}

static void func_reset(void)
{
	int i;
	int argaddr = 0;
	memset(regs, 0, sizeof(regs));
	sp = i_sp();
	sp_max = sp;
	ntmp = 0;
	sp_tmp = -1;
	nlabels = 0;
	njmps = 0;
	nlocals = 0;
	stat_calls = 0;
	stat_tmps = 0;
	stat_regs = 1 << REG_RET;
	for (i = 0; i < func_argc; i++) {
		localoff[nlocals++] = i_args() + argaddr;
		if (i >= N_ARGS || r_sargs() & (1 << argregs[i]))
			argaddr += LONGSZ;
	}
}

void o_func_beg(char *name, int argc, int global, int varg)
{
	func_argc = argc;
	func_varg = varg;
	func_beg = cslen;
	pass1 = 0;
	pass2 = 0;
	tmp_mask = N_TMPS > 6 ? R_TMPS & ~R_SAVED : R_TMPS;
	r_func(argc, varg);
	out_sym(name, (global ? OUT_GLOB : 0) | OUT_CS, cslen, 0);
	i_prolog(argc, varg, r_sargs(), tmp_mask & R_SAVED, 1, 1);
	func_reset();
}

void o_pass1(void)
{
	pass1 = 1;
}

void o_pass2(void)
{
	int locregs, leaf;
	int initfp, subsp, sregs;
	int i;
	o_label(0);
	jmp_fill();
	leaf = !stat_calls;
	cslen = func_beg;
	locregs = r_alloc(leaf, stat_regs);
	subsp = nlocals > locregs || !leaf;
	initfp = subsp || stat_tmps || func_argc > N_ARGS;
	sregs = (r_lregs() | stat_regs) & R_SAVED;
	tmp_mask = stat_regs;
	pass1 = 0;
	pass2 = 1;
	i_prolog(func_argc, func_varg, r_sargs(), sregs, initfp, subsp);
	func_reset();
	for (i = 0; i < MIN(func_argc, N_ARGS); i++)
		if (r_regmap(i) >= 0 && r_regmap(i) != argregs[i])
			i_mov(r_regmap(i), argregs[i]);
	for (i = N_ARGS; i < func_argc; i++)
		if (r_regmap(i) >= 0)
			i_load(r_regmap(i), REG_FP, localoff[i], LONGSZ);
}

void o_func_end(void)
{
	o_label(0);
	jmp_fill();
	i_epilog(sp_max);
}
