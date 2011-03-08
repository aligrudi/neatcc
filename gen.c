#include <stdlib.h>
#include <string.h>
#include "gen.h"
#include "out.h"
#include "tok.h"

#define TMP_ADDR	0x0001
#define LOC_REG		0x0100
#define LOC_MEM		0x0200
#define LOC_NUM		0x0400
#define LOC_SYM		0x0800
#define LOC_LOCAL	0x1000
#define LOC_MASK	0xff00

#define R_EAX		0x00
#define R_ECX		0x01
#define R_EDX		0x02
#define R_EBX		0x03
#define R_ESP		0x04
#define R_EBP		0x05
#define R_ESI		0x06
#define R_EDI		0x07
#define NREGS		0x08

#define OP_XR(op)	(op | 03)
#define OP_B(op)	(op & ~01)

#define I_MOV		0x89
#define I_MOVI		0xc7
#define I_MOVIR		0xb8
#define I_MOVR		0x8b
#define I_SHX		0xd3
#define I_CMP		0x3b
#define I_LEA		0x8d
#define I_NOT		0xf7
#define I_CALL		0xff
#define I_MUL		0xf7
#define I_XOR		0x33
#define I_TEST		0x85
#define I_INC		0xff

#define MIN(a, b)		((a) < (b) ? (a) : (b))

#define TMP_BT(t)		((t)->flags & TMP_ADDR ? LONGSZ : (t)->bt)
#define TMP_REG(t)		((t)->flags & LOC_REG ? (t)->addr : reg_get(~0))
#define TMP_REG2(t, r)		((t)->flags & LOC_REG && (t)->addr != r ? \
					(t)->addr : reg_get(~(1 << r)))
#define TMPBT(bt)		(BT_SZ(bt) >= 4 ? (bt) : (bt) & BT_SIGNED | 4)

#define R_BYTEREGS		(1 << R_EAX | 1 << R_EDX | 1 << R_ECX)
#define TMP_BYTEREG(t)		((t)->flags & LOC_REG && \
					(1 << (t)->addr) & R_BYTEREGS ? \
					(t)->addr : reg_get(R_BYTEREGS))

static char cs[SECSIZE];	/* code segment */
static int cslen;
static char ds[SECSIZE];	/* data segment */
static int dslen;
static long bsslen;		/* bss segment size */

static int nogen;
static long sp;
static long spsub_addr;
static long maxsp;

#define TMP(i)		(&tmps[ntmp - 1 - (i)])

static struct tmp {
	long addr;
	char sym[NAMELEN];
	long off;	/* offset from a symbol or a local */
	unsigned flags;
	unsigned bt;
} tmps[MAXTMP];
static int ntmp;

static int tmpsp;

static struct tmp *regs[NREGS];
static int tmpregs[] = {R_EAX, R_ESI, R_EDI, R_EBX, R_EDX, R_ECX};

#define MAXRET			(1 << 8)

static long ret[MAXRET];
static int nret;

static long cmp_last;
static long cmp_setl;

static void putint(char *s, long n, int l)
{
	while (l--) {
		*s++ = n;
		n >>= 8;
	}
}

static void os(char *s, int n)
{
	if (nogen)
		return;
	while (n--)
		cs[cslen++] = *s++;
}

static void oi(long n, int l)
{
	if (nogen)
		return;
	while (l--) {
		cs[cslen++] = n;
		n >>= 8;
	}
}

#define OP2(o2, o1)		(0x010000 | ((o2) << 8) | (o1))
#define O2(op)			(((op) >> 8) & 0xff)
#define O1(op)			((op) & 0xff)
#define MODRM(m, r1, r2)	((m) << 6 | (r1) << 3 | (r2))

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
	if (!off && base != R_EBP)
		mod = 0;
	op_x(op, src, base, bt);
	oi(MODRM(mod, src & 0x07, base & 0x07), 1);
	if (base == R_ESP)
		oi(0x24, 1);
	if (mod)
		oi(off, dis);
}

static void op_rr(int op, int src, int dst, int bt)
{
	op_x(op, src, dst, bt);
	oi(MODRM(3, src & 0x07, dst & 0x07), 1);
}

static void op_rs(int op, int src, char *name, int off, int bt)
{
	op_x(op, src, 0, bt);
	oi(MODRM(0, src & 0x07, 5), 1);
	if (!nogen)
		out_rel(name, OUT_CS, cslen);
	oi(off, 4);
}

static void op_ri(int op, int o3, int src, long num, int bt)
{
	op_x(op, src, 0, bt);
	oi(MODRM(3, o3, src & 0x07), 1);
	oi(num, MIN(4, BT_SZ(bt)));
}

static void op_sr(int op, int src, char *name, int off, int bt)
{
	op_x(op, src, 0, bt);
	oi(MODRM(0, src & 0x07, 5), 1);
	if (!nogen)
		out_rel(name, OUT_CS | OUT_REL, cslen);
	oi(off - 4, 4);
}

static void op_si(int op, int o3, char *name, int off, long num, int bt)
{
	int sz = MIN(4, BT_SZ(bt));
	op_x(op, 0, 0, bt);
	oi(MODRM(0, o3, 5), 1);
	if (!nogen)
		out_rel(name, OUT_CS | OUT_REL, cslen);
	oi(off - 4 - sz, 4);
	oi(num, sz);
}

static void op_mi(int op, int o3, int base, int off, long num, int bt)
{
	int dis = off == (char) off ? 1 : 4;
	int mod = dis == 4 ? 2 : 1;
	if (!off && base != R_EBP)
		mod = 0;
	op_x(op, 0, base, bt);
	oi(MODRM(mod, 0, base), 1);
	if (mod)
		oi(off, dis);
	oi(num, MIN(4, BT_SZ(bt)));
}

static long sp_push(int size)
{
	sp += size;
	if (sp > maxsp)
		maxsp = sp;
	return sp;
}

#define LOC_NEW(f, l)		(((f) & ~LOC_MASK) | (l))

static void tmp_mem(struct tmp *tmp)
{
	int src = tmp->addr;
	if (!(tmp->flags & LOC_REG))
		return;
	if (tmpsp == -1)
		tmpsp = sp;
	tmp->addr = -sp_push(LONGSZ);
	op_rm(I_MOV, src, R_EBP, tmp->addr, TMPBT(TMP_BT(tmp)));
	regs[src] = NULL;
	tmp->flags = LOC_NEW(tmp->flags, LOC_MEM);
}

static int movxx_x2r(int bt)
{
	int o2;
	if (BT_SZ(bt) == 1)
		o2 = bt & BT_SIGNED ? 0xbe : 0xb6;
	else
		o2 = bt & BT_SIGNED ? 0xbf : 0xb7;
	return OP2(0x0f, o2);
}

#define MOVSXD		0x63

static void mov_r2r(int r1, int r2, unsigned bt1, unsigned bt2)
{
	int s1 = bt1 & BT_SIGNED;
	int s2 = bt2 & BT_SIGNED;
	int sz1 = BT_SZ(bt1);
	int sz2 = BT_SZ(bt2);
	if (sz2 < 4 && (sz1 > sz2 || s1 != s2)) {
		op_rr(movxx_x2r(bt2), r1, r2, 4);
		return;
	}
	if (sz1 == 4 && sz2 == 8 && s1) {
		op_rr(MOVSXD, r2, r1, sz2);
		return;
	}
	if (r1 != r2 || sz1 > sz2)
		op_rr(I_MOV, r1, r2, TMPBT(bt2));
}

static void mov_m2r(int dst, int base, int off, int bt1, int bt2)
{
	if (BT_SZ(bt1) < 4) {
		op_rm(movxx_x2r(bt1), dst, base, off,
			bt1 & BT_SIGNED && BT_SZ(bt2) == 8 ? 8 : 4);
		mov_r2r(dst, dst, bt1, bt2);
	} else {
		op_rm(I_MOVR, dst, base, off, bt1);
		mov_r2r(dst, dst, bt1, bt2);
	}
}

static void num_cast(struct tmp *t, unsigned bt)
{
	if (!(bt & BT_SIGNED) && BT_SZ(bt) != LONGSZ)
		t->addr &= ((1l << (long) (BT_SZ(bt) * 8)) - 1);
	if (bt & BT_SIGNED && BT_SZ(bt) != LONGSZ &&
				t->addr > (1l << (BT_SZ(bt) * 8 - 1)))
		t->addr = -((1l << (BT_SZ(bt) * 8)) - t->addr);
	t->bt = bt;
}

static void num_reg(int reg, unsigned bt, long num)
{
	int op = I_MOVIR + (reg & 7);
	op_x(op, 0, reg, bt);
	oi(num, BT_SZ(bt));
}

static void tmp_reg(struct tmp *tmp, int dst, unsigned bt, int deref)
{
	if (deref && tmp->flags & TMP_ADDR)
		tmp->flags &= ~TMP_ADDR;
	else
		deref = 0;
	if (tmp->flags & LOC_NUM) {
		num_cast(tmp, bt);
		tmp->bt = TMPBT(bt);
		num_reg(dst, tmp->bt, tmp->addr);
		tmp->addr = dst;
		regs[dst] = tmp;
		tmp->flags = LOC_NEW(tmp->flags, LOC_REG);
	}
	if (tmp->flags & LOC_SYM) {
		op_rr(I_MOVI, 0, dst, 4);
		if (!nogen)
			out_rel(tmp->sym, OUT_CS, cslen);
		oi(tmp->off, 4);
		tmp->addr = dst;
		regs[dst] = tmp;
		tmp->flags = LOC_NEW(tmp->flags, LOC_REG);
	}
	if (tmp->flags & LOC_REG) {
		if (deref)
			mov_m2r(dst, tmp->addr, 0, tmp->bt, bt);
		else
			mov_r2r(tmp->addr, dst, TMP_BT(tmp),
				tmp->flags & TMP_ADDR ? LONGSZ : bt);
		regs[tmp->addr] = NULL;
	}
	if (tmp->flags & LOC_LOCAL) {
		if (deref)
			mov_m2r(dst, R_EBP, tmp->addr + tmp->off, tmp->bt, bt);
		else
			op_rm(I_LEA, dst, R_EBP, tmp->addr + tmp->off, LONGSZ);
	}
	if (tmp->flags & LOC_MEM) {
		int nbt = deref ? LONGSZ : TMP_BT(tmp);
		mov_m2r(dst, R_EBP, tmp->addr, nbt, nbt);
		if (deref)
			mov_m2r(dst, dst, 0, tmp->bt, bt);
	}
	tmp->addr = dst;
	tmp->bt = tmp->flags & TMP_ADDR ? bt : TMPBT(bt);
	regs[dst] = tmp;
	tmp->flags = LOC_NEW(tmp->flags, LOC_REG);
}

static void reg_free(int reg)
{
	int i;
	if (!regs[reg])
		return;
	for (i = 0; i < ARRAY_SIZE(tmpregs); i++)
		if (!regs[tmpregs[i]]) {
			tmp_reg(regs[reg], tmpregs[i], regs[reg]->bt, 0);
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
	tmp_reg(t, reg, t->bt, 0);
}

static void tmp_to(struct tmp *t, int reg, int bt)
{
	reg_for(reg, t);
	tmp_reg(t, reg, bt ? bt : t->bt, 1);
}

static void tmp_drop(int n)
{
	int i;
	for (i = ntmp - n; i < ntmp; i++)
		if (tmps[i].flags & LOC_REG)
			regs[tmps[i].addr] = NULL;
	cmp_last = -1;
	ntmp -= n;
}

static int tmp_pop(int reg, int bt)
{
	struct tmp *t = TMP(0);
	tmp_to(t, reg, bt);
	tmp_drop(1);
	return t->bt;
}

static struct tmp *tmp_new(void)
{
	cmp_last = -1;
	return &tmps[ntmp++];
}

static void tmp_push(int reg, unsigned bt)
{
	struct tmp *t = tmp_new();
	t->addr = reg;
	t->bt = bt;
	t->flags = LOC_REG;
	regs[reg] = t;
}

void o_local(long addr, unsigned bt)
{
	struct tmp *t = tmp_new();
	t->addr = -addr;
	t->bt = bt;
	t->flags = LOC_LOCAL | TMP_ADDR;
	t->off = 0;
}

void o_num(long num, unsigned bt)
{
	struct tmp *t = tmp_new();
	t->addr = num;
	t->bt = bt;
	t->flags = LOC_NUM;
}

void o_symaddr(char *name, unsigned bt)
{
	struct tmp *t = tmp_new();
	t->bt = bt;
	strcpy(t->sym, name);
	t->flags = LOC_SYM | TMP_ADDR;
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

#define FORK_REG		R_EAX

/* make sure tmps remain intact after a conditional expression */
void o_fork(void)
{
	int i;
	for (i = 0; i < ntmp - 1; i++)
		tmp_mem(&tmps[i]);
}

void o_forkpush(void)
{
	tmp_pop(R_EAX, 0);
}

void o_forkjoin(void)
{
	tmp_push(FORK_REG, 0);
}

void o_tmpswap(void)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	struct tmp t;
	memcpy(&t, t1, sizeof(t));
	memcpy(t1, t2, sizeof(t));
	memcpy(t2, &t, sizeof(t));
	if (t1->flags & LOC_REG)
		regs[t1->addr] = t1;
	if (t2->flags & LOC_REG)
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

static void tmp_copy(struct tmp *t1)
{
	struct tmp *t2 = tmp_new();
	memcpy(t2, t1, sizeof(*t1));
	if (!(t1->flags & (LOC_REG | LOC_MEM)))
		return;
	if (t1->flags & LOC_MEM) {
		tmp_reg(t2, reg_get(~0), t2->bt, 0);
	} else if (t1->flags & LOC_REG) {
		t2->addr = reg_get(~(1 << t1->addr));
		op_rr(I_MOV, t1->addr, t2->addr, TMPBT(TMP_BT(t1)));
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
	int reg;
	if (t->bt == bt)
		return;
	if (t->flags & LOC_NUM) {
		num_cast(t, bt);
		return;
	}
	reg = BT_SZ(bt) > 1 ? TMP_REG(t) : TMP_BYTEREG(t);
	tmp_to(t, reg, bt);
}

void o_func_beg(char *name, int global)
{
	out_sym(name, (global ? OUT_GLOB : 0) | OUT_CS, cslen, 0);
	os("\x55", 1);			/* push %rbp */
	os("\x89\xe5", 2);		/* mov %rsp, %rbp */
	os("\x53\x56\x57", 3);		/* push ebx; push esi; push edi */
	sp = 3 * LONGSZ;
	maxsp = sp;
	ntmp = 0;
	tmpsp = -1;
	nret = 0;
	cmp_last = -1;
	memset(regs, 0, sizeof(regs));
	os("\x81\xec", 2);		/* sub $xxx, %rsp */
	spsub_addr = cslen;
	oi(0, 4);
}

void o_deref(unsigned bt)
{
	struct tmp *t = TMP(0);
	if (t->flags & TMP_ADDR)
		tmp_to(t, TMP_REG(t), t->bt);
	t->bt = bt;
	t->flags |= TMP_ADDR;
}

void o_load(void)
{
	struct tmp *t = TMP(0);
	tmp_to(t, TMP_REG(t), t->bt);
}

static unsigned bt_op(unsigned bt1, unsigned bt2)
{
	unsigned s1 = BT_SZ(bt1);
	unsigned s2 = BT_SZ(bt2);
	unsigned bt = (bt1 | bt2) & BT_SIGNED | (s1 > s2 ? s1 : s2);
	return TMPBT(bt);
}

#define TMP_NUM(t)	((t)->flags & LOC_NUM && !((t)->flags & TMP_ADDR))
#define LOCAL_PTR(t)	((t)->flags & LOC_LOCAL && !((t)->flags & TMP_ADDR))
#define SYM_PTR(t)	((t)->flags & LOC_SYM && !((t)->flags & TMP_ADDR))

int o_popnum(long *c)
{
	struct tmp *t = TMP(0);
	if (!TMP_NUM(t))
		return 1;
	*c = t->addr;
	tmp_drop(1);
	return 0;
}

static void shx(int uop, int sop)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	int r2;
	int bt = t2->bt;
	tmp_to(t1, R_ECX, 0);
	r2 = TMP_REG2(t2, R_ECX);
	tmp_to(t2, r2, 0);
	tmp_drop(1);
	op_rr(I_SHX, bt & BT_SIGNED ? sop : uop, r2, TMPBT(bt));
}

#define CQO_REG		0x99

static int mulop(int uop, int sop, int reg)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	/* for div and mod, the sign of the second operand don't matter */
	int bt = uop == 4 ? bt_op(t1->bt, t2->bt) : TMPBT(t2->bt);
	if (t1->flags & LOC_REG && t1->addr != R_EAX && t1->addr != R_EDX)
		reg = t1->addr;
	tmp_to(t1, reg, bt);
	tmp_to(t2, R_EAX, bt);
	if (reg != R_EDX) {
		reg_free(R_EDX);
		if (bt & BT_SIGNED)
			op_x(CQO_REG, R_EAX, R_EDX, bt);
		else
			op_rr(I_XOR, R_EDX, R_EDX, bt);
	}
	tmp_drop(2);
	op_rr(I_MUL, bt & BT_SIGNED ? sop : uop, reg, TMPBT(t2->bt));
	return bt;
}

void o_addr(void)
{
	tmps[ntmp - 1].flags &= ~TMP_ADDR;
	tmps[ntmp - 1].bt = LONGSZ;
}

void o_ret(unsigned bt)
{
	if (bt)
		tmp_pop(R_EAX, bt);
	else
		os("\x31\xc0", 2);	/* xor %eax, %eax */
	ret[nret++] = o_jmp(0);
}

static void inc(int op)
{
	struct tmp *t = TMP(0);
	int reg;
	int off;
	if (t->flags & LOC_LOCAL) {
		reg = R_EBP;
		off = t->addr + t->off;
	} else {
		reg = TMP_REG(t);
		off = 0;
		tmp_mv(t, reg);
	}
	op_rm(I_INC, op, reg, off, t->bt);
}

static void o_lnot(void)
{
	if (cmp_last == cslen) {
		cs[cmp_setl + 1] ^= 0x01;
	} else {
		o_num(0, 4 | BT_SIGNED);
		o_bop(O_EQ);
	}
}

static void o_neg(int id)
{
	struct tmp *t = TMP(0);
	int reg;
	unsigned bt = TMPBT(t->bt);
	reg = TMP_REG(t);
	tmp_to(t, reg, bt);
	op_rr(I_NOT, id, reg, bt);
}

#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))

void o_func_end(void)
{
	int i;
	for (i = 0; i < nret; i++)
		o_filljmp(ret[i]);
	os("\x5f\x5e\x5b", 3);		/* pop edi; pop esi; pop ebx */
	os("\xc9\xc3", 2);		/* leave; ret; */
	putint(cs + spsub_addr, ALIGN(maxsp - 3 * LONGSZ, LONGSZ), 4);
}

long o_mklocal(int size)
{
	return sp_push(ALIGN(size, LONGSZ));
}

void o_rmlocal(long addr, int sz)
{
	sp = addr - sz;
}

long o_arg(int i, unsigned bt)
{
	return -LONGSZ * (i + 2);
}

void o_assign(unsigned bt)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	int r1 = BT_SZ(bt) > 1 ? TMP_REG(t1) : TMP_BYTEREG(t1);
	int reg;
	int off;
	tmp_to(t1, r1, TMPBT(bt));
	if (t2->flags & LOC_LOCAL) {
		reg = R_EBP;
		off = t2->addr + t2->off;
	} else {
		reg = TMP_REG2(t2, r1);
		off = 0;
		tmp_mv(t2, reg);
	}
	tmp_drop(2);
	op_rm(I_MOV, r1, reg, off, bt);
	tmp_push(r1, bt);
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
}

static int c_uop(int op)
{
	struct tmp *t1 = TMP(0);
	if (!TMP_NUM(t1))
		return 1;
	tmp_drop(1);
	o_num(cu(op, t1->addr), t1->bt);
	return 0;
}

static long cb(int op, long a, long b, int *bt, int bt1, int bt2)
{
	*bt = bt_op(bt1, bt2);
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
	case O_MUL:
		return a * b;
	case O_DIV:
		return a / b;
	case O_MOD:
		return a % b;
	case O_SHL:
		*bt = bt1;
		return a << b;
	case O_SHR:
		*bt = bt1;
		if (bt1 & BT_SIGNED)
			return a >> b;
		else
			return (unsigned long) a >> b;
	case O_LT:
		*bt = 4;
		return a < b;
	case O_GT:
		*bt = 4;
		return a > b;
	case O_LE:
		*bt = 4;
		return a <= b;
	case O_GE:
		*bt = 4;
		return a >= b;
	case O_EQ:
		*bt = 4;
		return a == b;
	case O_NEQ:
		*bt = 4;
		return a != b;
	}
}

static int c_bop(int op)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	int locals = LOCAL_PTR(t1) + LOCAL_PTR(t2);
	int syms = SYM_PTR(t1) + SYM_PTR(t2);
	int nums = TMP_NUM(t1) + TMP_NUM(t2);
	int bt;
	if (syms + locals == 2 || syms + nums + locals != 2)
		return 1;
	if (nums == 1)
		if (op != O_ADD && op != O_SUB || op == O_SUB && TMP_NUM(t2))
			return 1;
	bt = BT_SIGNED | LONGSZ;
	if (nums == 2)
		bt = bt_op(t1->bt, t2->bt);
	if (nums == 1) {
		long o1 = TMP_NUM(t1) ? t1->addr : t1->off;
		long o2 = TMP_NUM(t2) ? t2->addr : t2->off;
		long ret = cb(op, o2, o1, &bt, t2->bt, t1->bt);
		if (!TMP_NUM(t1))
			o_tmpswap();
		t2->off = ret;
		tmp_drop(1);
	} else {
		long ret = cb(op, t2->addr, t1->addr, &bt, t2->bt, t1->bt);
		tmp_drop(2);
		o_num(ret, bt);
	}
	return 0;
}

void o_uop(int op)
{
	if (!c_uop(op))
		return;
	switch (op) {
	case O_NEG:
	case O_NOT:
		o_neg(op == O_NEG ? 3 : 2);
		break;
	case O_LNOT:
		o_lnot();
		break;
	case O_INC:
	case O_DEC:
		inc(op == O_DEC);
		break;
	}
}

static int binop(int op, int *reg)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	int r1, r2;
	unsigned bt = bt_op(t1->bt, t2->bt);
	r1 = TMP_REG(t1);
	tmp_to(t1, r1, bt);
	r2 = TMP_REG2(t2, r1);
	tmp_pop(r1, bt);
	tmp_pop(r2, bt);
	op_rr(op, r2, r1, bt);
	*reg = r2;
	return bt;
}

static void bin_add(int op)
{
	/* opcode for O_ADD, O_SUB, O_AND, O_OR, O_XOR */
	static int rx[] = {0003, 0053, 0043, 0013, 0063};
	int reg;
	int bt = binop(rx[op & 0x0f], &reg);
	tmp_push(reg, bt);
}

static void bin_shx(int op)
{
	if ((op & 0xff) == O_SHL)
		shx(4, 4);
	else
		shx(5, 7);
}

static void bin_mul(int op)
{
	if ((op & 0xff) == O_MUL)
		tmp_push(R_EAX, mulop(4, 5, R_EDX));
	if ((op & 0xff) == O_DIV)
		tmp_push(R_EAX, mulop(6, 7, R_ECX));
	if ((op & 0xff) == O_MOD)
		tmp_push(R_EDX, mulop(6, 7, R_ECX));
}

static void o_cmp(int uop, int sop)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	char set[] = "\x0f\x00\xc0";
	int reg;
	int bt;
	if (regs[R_EAX] && regs[R_EAX] != t1 && regs[R_EAX] != t2)
		reg_free(R_EAX);
	bt = binop(I_CMP, &reg);
	set[1] = bt & BT_SIGNED ? sop : uop;
	cmp_setl = cslen;
	os(set, 3);			/* setl %al */
	os("\x0f\xb6\xc0", 3);		/* movzbl %al, %eax */
	tmp_push(R_EAX, 4 | BT_SIGNED);
	cmp_last = cslen;
}

static void bin_cmp(int op)
{
	switch (op & 0xff) {
	case O_LT:
		o_cmp(0x92, 0x9c);
		break;
	case O_GT:
		o_cmp(0x97, 0x9f);
		break;
	case O_LE:
		o_cmp(0x96, 0x9e);
		break;
	case O_GE:
		o_cmp(0x93, 0x9d);
		break;
	case O_EQ:
		o_cmp(0x94, 0x94);
		break;
	case O_NEQ:
		o_cmp(0x95, 0x95);
		break;
	}
}

static void o_bopset(int op)
{
	tmp_copy(TMP(1));
	o_tmpswap();
	o_bop(op & ~O_SET);
	o_assign(TMP(1)->bt);
}

void o_bop(int op)
{
	if (!(op & O_SET) && !c_bop(op))
		return;
	if (op & O_SET) {
		o_bopset(op);
		return;
	}
	if ((op & 0xf0) == 0x00)
		bin_add(op);
	if ((op & 0xf0) == 0x10)
		bin_shx(op);
	if ((op & 0xf0) == 0x20)
		bin_mul(op);
	if ((op & 0xf0) == 0x30)
		bin_cmp(op);
}

void o_memcpy(int sz)
{
	struct tmp *t0 = TMP(-1);
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	o_num(sz, 4);
	tmp_to(t0, R_ECX, 0);
	tmp_mv(t1, R_ESI);
	tmp_mv(t2, R_EDI);
	os("\xf3\xa4", 2);		/* rep movs */
	tmp_drop(2);
}

void o_memset(int x, int sz)
{
	struct tmp *t0 = TMP(-2);
	struct tmp *t1 = TMP(-1);
	struct tmp *t2 = TMP(0);
	o_num(sz, 4);
	o_num(x, 4);
	tmp_to(t0, R_EAX, 0);
	tmp_to(t1, R_ECX, 0);
	tmp_mv(t2, R_EDI);
	os("\xf3\xaa", 2);		/* rep stosb */
	tmp_drop(2);
}

long o_mklabel(void)
{
	return cslen;
}

static long jx(int x, long addr)
{
	char op[2] = {0x0f};
	op[1] = x;
	os(op, 2);		/* jx $addr */
	oi(addr - cslen - 4, 4);
	return cslen - 4;
}

static long jxtest(int x, long addr)
{
	int bt = tmp_pop(R_EAX, 0);
	op_rr(I_TEST, R_EAX, R_EAX, bt);
	return jx(x, addr);
}

static long jxcmp(long addr, int inv)
{
	int x;
	if (cslen != cmp_last)
		return -1;
	tmp_drop(1);
	cslen = cmp_setl;
	x = (unsigned char) cs[cmp_setl + 1];
	return jx((inv ? x : x ^ 0x01) & ~0x10, addr);
}

long o_jz(long addr)
{
	long ret = jxcmp(addr, 0);
	return ret != -1 ? ret : jxtest(0x84, addr);
}

long o_jnz(long addr)
{
	long ret = jxcmp(addr, 1);
	return ret != -1 ? ret : jxtest(0x85, addr);
}

long o_jmp(long addr)
{
	os("\xe9", 1);			/* jmp $addr */
	oi(addr - cslen - 4, 4);
	return cslen - 4;
}

void o_filljmp2(long addr, long jmpdst)
{
	putint(cs + addr, jmpdst - addr - 4, 4);
}

void o_filljmp(long addr)
{
	o_filljmp2(addr, cslen);
}

void o_call(int argc, unsigned *bt, unsigned ret_bt)
{
	struct tmp *t;
	int i;
	for (i = 0; i < ARRAY_SIZE(tmpregs); i++)
		if (regs[tmpregs[i]] && regs[tmpregs[i]] - tmps < ntmp - argc)
			tmp_mem(regs[tmpregs[i]]);
	sp_push(LONGSZ * argc);
	for (i = argc - 1; i >= 0; --i) {
		int reg = TMP_REG(TMP(0));
		tmp_pop(reg, TMPBT(bt[i]));
		op_rm(I_MOV, reg, R_ESP, i * LONGSZ, TMPBT(bt[i]));
	}
	t = TMP(0);
	if (t->flags & LOC_SYM) {
		os("\xe8", 1);		/* call $x */
		if (!nogen)
			out_rel(t->sym, OUT_CS | OUT_REL, cslen);
		oi(-4 + t->off, 4);
		tmp_drop(1);
	} else {
		tmp_mv(t, R_EAX);
		tmp_drop(1);
		op_rr(I_CALL, 2, R_EAX, 4);
	}
	if (ret_bt)
		tmp_push(R_EAX, ret_bt);
}

int o_nogen(void)
{
	return nogen++;
}

void o_dogen(void)
{
	nogen = 0;
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
	if (t->flags & LOC_NUM && !(t->flags & TMP_ADDR)) {
		num_cast(t, bt);
		memcpy(ds + sym_off, &t->addr, BT_SZ(bt));
	}
	if (t->flags & LOC_SYM && !(t->flags & TMP_ADDR)) {
		out_rel(t->sym, OUT_DS, sym_off);
		memcpy(ds + sym_off, &t->off, BT_SZ(bt));
	}
	tmp_drop(1);
}

void o_write(int fd)
{
	out_write(fd, cs, cslen, ds, dslen);
}
