#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "gen.h"
#include "out.h"
#include "tok.h"

#define LOC_REG		0x01
#define LOC_MEM		0x02
#define LOC_NUM		0x04
#define LOC_SYM		0x08
#define LOC_LOCAL	0x10

#define NREGS		16

#define REG_PC		15	/* program counter */
#define REG_LR		14	/* link register */
#define REG_SP		13	/* stack pointer */
#define REG_TMP		12	/* temporary register */
#define REG_FP		11	/* frame pointer register */
#define REG_DP		10	/* data pointer register */
#define REG_RET		0	/* returned value register */

#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))

static char cs[SECSIZE];	/* code segment */
static int cslen;
static char ds[SECSIZE];	/* data segment */
static int dslen;
static long bsslen;		/* bss segment size */

static int nogen;		/* don't generate code */
static long sp;
static long func_beg;
static long maxsp;

#define TMP(i)		(((i) < ntmp) ? &tmps[ntmp - 1 - (i)] : NULL)

static struct tmp {
	long addr;
	char sym[NAMELEN];
	long off;	/* offset from a symbol or a local */
	unsigned loc;	/* variable location */
	unsigned bt;	/* type of address; zero when not a pointer */
} tmps[MAXTMP];
static int ntmp;

static int tmpsp;

static struct tmp *regs[NREGS];
static int tmpregs[] = {4, 5, 6, 7, 8, 9, 0, 1, 2, 3};
static int argregs[] = {0, 1, 2, 3};

#define I_AND		0x00
#define I_EOR		0x01
#define I_SUB		0x02
#define I_RSB		0x03
#define I_ADD		0x04
#define I_TST		0x08
#define I_CMP		0x0a
#define I_ORR		0x0c
#define I_MOV		0x0d
#define I_MVN		0x0f

#define MAXRET			(1 << 8)

static long ret[MAXRET];
static int nret;

/* output div/mod functions */
static int putdiv = 0;

/* for optimizing cmp + bcc */
static long last_cmp = -1;
static long last_set = -1;
static long last_cond;

static void os(void *s, int n)
{
	memcpy(cs + cslen, s, n);
	cslen += n;
}

static void oi(long n)
{
	if (nogen)
		return;
	*(int *) (cs + cslen) = n;
	cslen += 4;
}

#define MAXNUMS		1024

/* data pool */
static long num_offs[MAXNUMS];			/* data immediate value */
static char num_names[MAXNUMS][NAMELEN];	/* relocation data symbol name */
static int nums;

static int pool_find(char *name, int off)
{
	int i;
	for (i = 0; i < nums; i++)
		if (!strcmp(name, num_names[i]) && off == num_offs[i])
			return i;
	return -1;
}

static int pool_num(long num)
{
	int idx = pool_find("", num);
	if (idx < 0) {
		idx = nums++;
		num_offs[idx] = num;
		num_names[idx][0] = '\0';
	}
	return idx << 2;
}

static int pool_reloc(char *name, long off)
{
	int idx = pool_find(name, off);
	if (idx < 0) {
		idx = nums++;
		num_offs[idx] = off;
		strcpy(num_names[idx], name);
	}
	return idx << 2;
}

static void pool_write(void)
{
	int i;
	for (i = 0; i < nums; i++) {
		if (num_names[i])
			out_rel(num_names[i], OUT_CS, cslen);
		oi(num_offs[i]);
	}
}

/*
 * data processing:
 * +---------------------------------------+
 * |COND|00|I| op |S| Rn | Rd |  operand2  |
 * +---------------------------------------+
 *
 * S: set condition code
 * Rn: first operand
 * Rd: destination operand
 *
 * I=0 operand2=| shift  | Rm |
 * I=1 operand2=|rota|  imm   |
 */
#define ADD(op, rd, rn, s, i, cond)	\
	(((cond) << 28) | ((i) << 25) | ((s) << 20) | \
		((op) << 21) | ((rn) << 16) | ((rd) << 12))

static void i_add(int op, int rd, int rn, int rm)
{
	oi(ADD(op, rd, rn, 0, 0, 14) | rm);
}

static int add_encimm(unsigned n)
{
	int i = 0;
	while (i < 12 && (n >> ((4 + i) << 1)))
		i++;
	return (n >> (i << 1)) | (((16 - i) & 0x0f) << 8);
}

static int add_decimm(unsigned n)
{
	int rot = (16 - ((n >> 8) & 0x0f)) & 0x0f;
	return (n & 0xff) << (rot << 1);
}

static int add_rndimm(unsigned n)
{
	int rot = (n >> 8) & 0x0f;
	int num = n & 0xff;
	if (rot == 0)
		return n;
	if (num == 0xff) {
		num = 0;
		rot = (rot + 12) & 0x0f;
	}
	return ((num + 1) & 0xff) | (rot << 8);
}

static void i_ldr(int l, int rd, int rn, int off, int bt);

static void i_num(int rd, long n)
{
	int neg = n < 0;
	int p = neg ? -n - 1 : n;
	int enc = add_encimm(p);
	if (p == add_decimm(enc)) {
		oi(ADD(neg ? I_MVN : I_MOV, rd, 0, 0, 1, 14) | enc);
	} else {
		if (!nogen) {
			int off = pool_num(n);
			i_ldr(1, rd, REG_DP, off, LONGSZ);
		}
	}
}

static void i_add_imm(int op, int rd, int rn, int n)
{
	int neg = n < 0;
	int imm = add_encimm(neg ? -n : n);
	if (imm == add_decimm(neg ? -n : n)) {
		oi(ADD(neg ? I_SUB : I_ADD, rd, rn, 0, 1, 14) | imm);
	} else {
		i_num(rd, n);
		i_add(I_ADD, rd, rd, rn);
	}
}

/*
 * multiply
 * +----------------------------------------+
 * |COND|000000|A|S| Rd | Rn | Rs |1001| Rm |
 * +----------------------------------------+
 *
 * Rd: destination
 * A: accumulate
 * C: set condition codes
 *
 * I=0 operand2=| shift  | Rm |
 * I=1 operand2=|rota|  imm   |
 */
#define MUL(rd, rn, rs)		\
	((14 << 28) | ((rd) << 16) | ((0) << 12) | ((rn) << 8) | ((9) << 4) | (rm))

static void i_mul(int rd, int rn, int rm)
{
	oi(MUL(rd, rn, rm));
}

static void i_cmp(int op, int rn, int rm)
{
	oi(ADD(op, 0, rn, 1, 0, 14) | rm);
	last_cmp = cslen;
}

static void i_set(int cond, int rd)
{
	last_set = cslen;
	last_cond = cond;
	oi(ADD(I_MOV, rd, 0, 0, 1, 14));
	oi(ADD(I_MOV, rd, 0, 0, 1, cond) | 1);
}

#define SM_LSL		0
#define SM_LSR		1
#define SM_ASR		2

static void i_shl(int sm, int rd, int rm, int rs)
{
	oi(ADD(I_MOV, rd, 0, 0, 0, 14) | (rs << 8) | (sm << 5) | (1 << 4) | rm);
}

static void i_shl_imm(int sm, int rd, int n)
{
	oi(ADD(I_MOV, rd, 0, 0, 0, 14) | (n << 7) | (sm << 5) | rd);
}

static void i_mov(int op, int rd, int rn)
{
	oi(ADD(op, rd, 0, 0, 0, 14) | rn);
}

/*
 * single data transfer:
 * +------------------------------------------+
 * |COND|01|I|P|U|B|W|L| Rn | Rd |   offset   |
 * +------------------------------------------+
 *
 * I: immediate/offset
 * P: post/pre indexing
 * U: down/up
 * B: byte/word
 * W: writeback
 * L: store/load
 * Rn: base register
 * Rd: source/destination register
 *
 * I=0 offset=| immediate |
 * I=1 offset=| shift  | Rm |
 *
 * halfword and signed data transfer
 * +----------------------------------------------+
 * |COND|000|P|U|0|W|L| Rn | Rd |0000|1|S|H|1| Rm |
 * +----------------------------------------------+
 *
 * +----------------------------------------------+
 * |COND|000|P|U|1|W|L| Rn | Rd |off1|1|S|H|1|off2|
 * +----------------------------------------------+
 *
 * S: singed
 * H: halfword
 */
#define LDR(l, rd, rn, b, u, p, w)		\
	((14 << 28) | (1 << 26) | ((p) << 24) | ((b) << 22) | ((u) << 23) | \
	((w) << 21) | ((l) << 20) | ((rn) << 16) | ((rd) << 12))
#define LDRH(l, rd, rn, s, h, u, i)	\
	((14 << 28) | (1 << 24) | ((u) << 23) | ((i) << 22) | ((l) << 20) | \
	((rn) << 16) | ((rd) << 12) | ((s) << 6) | ((h) << 5) | (9 << 4))

static void i_ldr(int l, int rd, int rn, int off, int bt)
{
	int b = BT_SZ(bt) == 1;
	int h = BT_SZ(bt) == 2;
	int s = l && (bt & BT_SIGNED);
	int half = h || (b && s);
	int maximm = half ? 0x100 : 0x1000;
	int neg = off < 0;
	if (neg)
		off = -off;
	while (off >= maximm) {
		int imm = add_encimm(off);
		oi(ADD(neg ? I_SUB : I_ADD, REG_TMP, rn, 0, 1, 14) | imm);
		rn = REG_TMP;
		off -= add_decimm(imm);
	}
	if (!half)
		oi(LDR(l, rd, rn, b, !neg, 1, 0) | off);
	else
		oi(LDRH(l, rd, rn, s, h, !neg, 1) |
			((off & 0xf0) << 4) | (off & 0x0f));
}

static void i_sym(int rd, char *sym, int off)
{
	if (!nogen) {
		int doff = pool_reloc(sym, off);
		i_ldr(1, rd, REG_DP, doff, LONGSZ);
	}
}

static void i_neg(int rd)
{
	oi(ADD(I_RSB, rd, rd, 0, 1, 14));
}

static void i_not(int rd)
{
	oi(ADD(I_MVN, rd, 0, 0, 0, 14) | rd);
}

static void i_lnot(int rd)
{
	i_cmp(I_TST, rd, rd);
	oi(ADD(I_MOV, rd, 0, 0, 1, 14));
	oi(ADD(I_MOV, rd, 0, 0, 1, 0) | 1);
}

/* rd = rd & ((1 << bits) - 1) */
static void i_zx(int rd, int bits)
{
	if (bits <= 8) {
		oi(ADD(I_AND, rd, rd, 0, 1, 14) | add_encimm((1 << bits) - 1));
	} else {
		i_shl_imm(SM_LSL, rd, 32 - bits);
		i_shl_imm(SM_LSR, rd, 32 - bits);
	}
}

static void i_sx(int rd, int bits)
{
	i_shl_imm(SM_LSL, rd, 32 - bits);
	i_shl_imm(SM_ASR, rd, 32 - bits);
}

/*
 * branch:
 * +-----------------------------------+
 * |COND|101|L|         offset         |
 * +-----------------------------------+
 *
 * L: link
 */
#define BL(cond, l, o)	(((cond) << 28) | (5 << 25) | ((l) << 24) | \
				((((o) - 8) >> 2) & 0x00ffffff))

static void i_b(long addr)
{
	oi(BL(14, 0, addr - cslen));
}

static void i_b_if(long addr, int rn, int z)
{
	static int nots[] = {1, 0, 3, 2, -1, -1, -1, -1, 9, 8, 11, 10, 13, 12, -1};
	if (last_cmp + 8 != cslen || last_set != last_cmp) {
		i_cmp(I_TST, rn, rn);
		oi(BL(z ? 0 : 1, 0, addr - cslen));
		return;
	}
	cslen = last_cmp;
	oi(BL(z ? nots[last_cond] : last_cond, 0, addr - cslen));
}

static void i_b_fill(long *dst, int diff)
{
	*dst = (*dst & 0xff000000) | (((diff - 8) >> 2) & 0x00ffffff);
}

static void i_memcpy(int rd, int rs, int rn)
{
	oi(ADD(I_SUB, rn, rn, 1, 1, 14) | 1);
	oi(BL(4, 0, 16));
	oi(LDR(1, REG_TMP, rs, 1, 1, 0, 0) | 1);
	oi(LDR(0, REG_TMP, rd, 1, 1, 0, 0) | 1);
	oi(BL(14, 0, -16));
}

static void i_memset(int rd, int rs, int rn)
{
	oi(ADD(I_SUB, rn, rn, 1, 1, 14) | 1);
	oi(BL(4, 0, 12));
	oi(LDR(0, rs, rd, 1, 1, 0, 0) | 1);
	oi(BL(14, 0, -12));
}

static void i_call_reg(int rd)
{
	i_mov(I_MOV, REG_LR, REG_PC);
	i_mov(I_MOV, REG_PC, rd);
}

static void i_call(char *sym)
{
	if (!nogen)
		out_rel(sym, OUT_CS | OUT_REL24, cslen);
	oi(BL(14, 1, 0));
}

static void i_prolog(void)
{
	func_beg = cslen;
	nums = 0;
	oi(0xe1a0c00d);		/* mov   r12, sp */
	oi(0xe92d000f);		/* stmfd sp!, {r0-r3} */
	oi(0xe92d5ff0);		/* stmfd sp!, {r0-r11, r12, lr} */
	oi(0xe1a0b00d);		/* mov   fp, sp */
	oi(0xe24dd000);		/* sub   sp, sp, xx */
	oi(0xe28fa000);		/* add   dp, pc, xx */
}

static void i_epilog(void)
{
	int dpoff;
	oi(0xe89baff0);		/* ldmfd fp, {r4-r11, sp, pc} */
	dpoff = add_decimm(add_rndimm(add_encimm(cslen - func_beg - 28)));
	cslen = func_beg + dpoff + 28;
	maxsp = ALIGN(maxsp, 8);
	maxsp = add_decimm(add_rndimm(add_encimm(maxsp)));
	/* fill stack sub: sp = sp - xx */
	*(long *) (cs + func_beg + 16) |= add_encimm(maxsp);
	/* fill data ptr addition: dp = pc + xx */
	*(long *) (cs + func_beg + 20) |= add_encimm(dpoff);
	pool_write();
}

static long sp_push(int size)
{
	sp += size;
	if (sp > maxsp)
		maxsp = sp;
	return sp;
}

static void tmp_mem(struct tmp *tmp)
{
	int src = tmp->addr;
	if (!(tmp->loc == LOC_REG))
		return;
	if (tmpsp == -1)
		tmpsp = sp;
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
			i_mov(I_MOV, dst, tmp->addr);
		regs[tmp->addr] = NULL;
	}
	if (tmp->loc == LOC_LOCAL) {
		if (deref)
			i_ldr(1, dst, REG_FP, tmp->addr + tmp->off, bt);
		else
			i_add_imm(I_ADD, dst, REG_FP, tmp->addr + tmp->off);
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
	for (i = 0; i < ARRAY_SIZE(tmpregs); i++)
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
		if (tmpsp != -1)
			sp = tmpsp;
		tmpsp = -1;
	}
}

#define FORK_REG		0x00

/* make sure tmps remain intact after a conditional expression */
void o_fork(void)
{
	int i;
	for (i = 0; i < ntmp - 1; i++)
		tmp_mem(&tmps[i]);
}

void o_forkpush(void)
{
	tmp_pop(FORK_REG);
}

void o_forkjoin(void)
{
	tmp_push(FORK_REG);
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
	for (i = 0; i < ARRAY_SIZE(tmpregs); i++)
		if ((1 << tmpregs[i]) & mask && !regs[tmpregs[i]])
			return tmpregs[i];
	for (i = 0; i < ARRAY_SIZE(tmpregs); i++)
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
		i_mov(I_MOV, t2->addr, t1->addr);
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
		int reg = reg_fortmp(t, 0);
		tmp_to(t, reg);
		if (bt & BT_SIGNED)
			i_sx(reg, BT_SZ(bt) * 8);
		else
			i_zx(reg, BT_SZ(bt) * 8);
	}
}

void o_func_beg(char *name, int global)
{
	out_sym(name, (global ? OUT_GLOB : 0) | OUT_CS, cslen, 0);
	i_prolog();
	sp = 0;
	maxsp = sp;
	ntmp = 0;
	tmpsp = -1;
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

long o_mklocal(int size)
{
	return sp_push(ALIGN(size, LONGSZ));
}

void o_rmlocal(long addr, int sz)
{
	sp = addr - sz;
}

long o_arg(int i)
{
	return -(10 + i) << 2;
}

void o_assign(unsigned bt)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	int r1 = reg_fortmp(t1, 0);
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
	tmp_drop(2);
	i_ldr(0, r1, r2, off, bt);
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
	int locals = LOCAL_PTR(t1) + LOCAL_PTR(t2);
	int syms = SYM_PTR(t1) + SYM_PTR(t2);
	int nums = TMP_NUM(t1) + TMP_NUM(t2);
	if (syms + locals == 2 || syms + nums + locals != 2)
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
	int r1 = reg_fortmp(TMP(0), 0);
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

static void bin_regs(int *r1, int *r2)
{
	struct tmp *t2 = TMP(0);
	struct tmp *t1 = TMP(1);
	*r2 = reg_fortmp(t2, 0);
	tmp_to(t2, *r2);
	*r1 = reg_fortmp(t1, 1 << *r2);
	tmp_pop(*r2);
	tmp_pop(*r1);
}

static void bin_add(int op)
{
	/* opcode for O_ADD, O_SUB, O_AND, O_OR, O_XOR */
	static int rx[] = {I_ADD, I_SUB, I_AND, I_ORR, I_EOR};
	int r1, r2;
	bin_regs(&r1, &r2);
	i_add(rx[op & 0x0f], r1, r1, r2);
	tmp_push(r1);
}

static void bin_shx(int op)
{
	int sm = SM_LSL;
	int r1, r2;
	bin_regs(&r1, &r2);
	if ((op & 0x0f) == 1)
		sm = op & O_SIGNED ? SM_ASR : SM_LSR;
	i_shl(sm, r1, r1, r2);
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
		i_shl_imm(SM_LSL, r2, p);
		return 0;
	}
	if (op == O_DIV) {
		tmp_drop(1);
		if (n == 1)
			return 0;
		r2 = reg_fortmp(t2, 0);
		tmp_to(t2, r2);
		i_shl_imm(op & O_SIGNED ? SM_ASR : SM_LSR, r2, p);
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

static void bin_div(int op)
{
	struct tmp *t2 = TMP(0);
	struct tmp *t1 = TMP(1);
	char *func;
	int i;
	putdiv = 1;
	if ((op & 0xff) == O_DIV)
		func = op & O_SIGNED ? "__divdi3" : "__udivdi3";
	else
		func = op & O_SIGNED ? "__moddi3" : "__umoddi3";
	for (i = 0; i < ARRAY_SIZE(argregs); i++)
		if (regs[argregs[i]] && regs[argregs[i]] - tmps < ntmp - 2)
			tmp_mem(regs[argregs[i]]);
	tmp_to(t1, argregs[0]);
	tmp_to(t2, argregs[1]);
	tmp_drop(2);
	i_call(func);
	tmp_push(REG_RET);
}

static void bin_mul(int op)
{
	int r1, r2;
	if (!mul_2(op))
		return;
	if ((op & 0xff) == O_DIV || (op & 0xff) == O_MOD) {
		bin_div(op);
	} else {
		bin_regs(&r1, &r2);
		i_mul(r1, r1, r2);
		tmp_push(r1);
	}
}

static void bin_cmp(int op)
{
	/* lt, gt, le, ge, eq, neq */
	static int ucond[] = {3, 8, 9, 2, 0, 1};
	static int scond[] = {11, 12, 13, 10, 0, 1};
	int r1, r2;
	bin_regs(&r1, &r2);
	i_cmp(I_CMP, r1, r2);
	i_set(op & O_SIGNED ? scond[op & 0x0f] : ucond[op & 0x0f], r1);
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

static void load_regs2(int *r0, int *r1, int *r2)
{
	struct tmp *t0 = TMP(0);
	struct tmp *t1 = TMP(1);
	struct tmp *t2 = TMP(2);
	*r0 = reg_fortmp(t0, 0);
	*r1 = reg_fortmp(t1, 1 << *r0);
	*r2 = reg_fortmp(t2, (1 << *r0) | (1 << *r1));
	tmp_to(t0, *r0);
	tmp_to(t1, *r1);
	tmp_to(t2, *r2);
}

void o_memcpy(void)
{
	int rd, rs, rn;
	load_regs2(&rn, &rs, &rd);
	i_memcpy(rd, rs, rn);
	tmp_drop(2);
}

void o_memset(void)
{
	int rd, rs, rn;
	load_regs2(&rn, &rs, &rd);
	i_memset(rd, rs, rn);
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
	int aregs = MIN(ARRAY_SIZE(argregs), argc);
	for (i = 0; i < ARRAY_SIZE(argregs); i++)
		if (regs[argregs[i]] && regs[argregs[i]] - tmps < ntmp - argc)
			tmp_mem(regs[argregs[i]]);
	if (argc > aregs) {
		sp_push(LONGSZ * (argc - aregs));
		for (i = argc - 1; i >= aregs; --i) {
			int reg = reg_fortmp(TMP(0), 0);
			tmp_pop(reg);
			i_ldr(0, reg, REG_SP, (i - aregs) * LONGSZ, LONGSZ);
		}
	}
	for (i = aregs - 1; i >= 0; --i)
		tmp_to(TMP(aregs - i - 1), argregs[i]);
	tmp_drop(aregs);
	t = TMP(0);
	if (t->loc == LOC_SYM && !t->bt) {
		i_call(t->sym);
		tmp_drop(1);
	} else {
		int reg = t->loc == LOC_REG ? t->addr : REG_TMP;
		tmp_pop(reg);
		i_call_reg(reg);
	}
	if (rets)
		tmp_push(REG_RET);
}

void o_nogen(void)
{
	nogen++;
}

void o_dogen(void)
{
	nogen--;
}

void dat_bss(char *name, int size, int global)
{
	out_sym(name, OUT_BSS | (global ? OUT_GLOB : 0), bsslen, size);
	bsslen += ALIGN(size, LONGSZ);
}

#define MAXDATS		(1 << 10)
static char dat_names[MAXDATS][NAMELEN];
static int dat_offs[MAXDATS];
static int ndats;

void err(char *msg);
void *dat_dat(char *name, int size, int global)
{
	void *addr = ds + dslen;
	int idx = ndats++;
	if (idx >= MAXDATS)
		err("nomem: MAXDATS reached!\n");
	strcpy(dat_names[idx], name);
	dat_offs[idx] = dslen;
	out_sym(name, OUT_DS | (global ? OUT_GLOB : 0), dslen, size);
	dslen += ALIGN(size, LONGSZ);
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

/* compiled division functions; div.s contains the source */
static int udivdi3[] = {
	0xe3a02000, 0xe3a03000, 0xe1110001, 0x0a00000a,
	0xe1b0c211, 0xe2822001, 0x5afffffc, 0xe3a0c001,
	0xe2522001, 0x4a000004, 0xe1500211, 0x3afffffb,
	0xe0400211, 0xe083321c, 0xeafffff8, 0xe1a01000,
	0xe1a00003, 0xe1a0f00e,
};
static int umoddi3[] = {
	0xe92d4000, 0xebffffeb, 0xe1a00001, 0xe8bd8000,
};
static int divdi3[] = {
	0xe92d4030, 0xe1a04000, 0xe1a05001, 0xe1100000,
	0x42600000, 0xe1110001, 0x42611000, 0xebffffe1,
	0xe1340005, 0x42600000, 0xe1140004, 0x42611000,
	0xe8bd8030,
};
static int moddi3[] = {
	0xe92d4000, 0xebfffff0, 0xe1a00001, 0xe8bd8000,
};

void o_write(int fd)
{
	if (putdiv) {
		out_sym("__udivdi3", OUT_CS, cslen, 0);
		os(udivdi3, sizeof(udivdi3));
		out_sym("__umoddi3", OUT_CS, cslen, 0);
		os(umoddi3, sizeof(umoddi3));
		out_sym("__divdi3", OUT_CS, cslen, 0);
		os(divdi3, sizeof(divdi3));
		out_sym("__moddi3", OUT_CS, cslen, 0);
		os(moddi3, sizeof(moddi3));
	}
	out_write(fd, cs, cslen, ds, dslen);
}
