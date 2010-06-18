#include <stdlib.h>
#include <string.h>
#include "gen.h"
#include "tok.h"

#define TMP_ADDR	0x0001
#define LOC_REG		0x0100
#define LOC_MEM		0x0200
#define LOC_NUM		0x0400
#define LOC_SYM		0x0800
#define LOC_LOCAL	0x1000
#define LOC_MASK	0xff00

#define R_RAX		0x00
#define R_RCX		0x01
#define R_RDX		0x02
#define R_RBX		0x03
#define R_RSP		0x04
#define R_RBP		0x05
#define R_RSI		0x06
#define R_RDI		0x07
#define R_R8		0x08
#define R_R9		0x09
#define R_R10		0x0a
#define R_R11		0x0b
#define R_R12		0x0c
#define R_R13		0x0d
#define R_R14		0x0e
#define R_R15		0x0f
#define NREGS		0x10

#define MOV_M2R		0x8b
#define MOV_R2X		0x89
#define MOV_I2X		0xc7
#define MOV_I2R		0xb8
#define ADD_R2X		0x03
#define SUB_R2X		0x2b
#define SHX_REG		0xd3
#define CMP_R2X		0x3b
#define LEA_M2R		0x8d
#define NEG_REG		0xf7
#define NOT_REG		0xf7
#define CALL_REG	0xff
#define MUL_A2X		0xf7
#define XOR_R2X		0x33
#define AND_R2X		0x23
#define OR_R2X		0x0b
#define TEST_R2R	0x85

#define MIN(a, b)		((a) < (b) ? (a) : (b))

#define R_BYTEMASK		(1 << R_RAX | 1 << R_RDX | 1 << R_RCX)
#define TMP_BT(t)		((t)->flags & TMP_ADDR ? 8 : (t)->bt)
#define TMP_REG(t)		((t)->flags & LOC_REG ? (t)->addr : reg_get(~0))
#define TMP_REG2(t, r)		((t)->flags & LOC_REG && (t)->addr != r ? \
					(t)->addr : reg_get(~(1 << r)))
#define TMP_BYTEREG(t)		((t)->flags & LOC_REG && \
				 (1 << (t)->addr) & R_BYTEMASK ? \
					(t)->addr : reg_get(R_BYTEMASK))
#define BT_TMPBT(bt)		(BT_SZ(bt) >= 4 ? (bt) : (bt) & BT_SIGNED | 4)

static char buf[SECSIZE];
static char *cur;
static int nogen;
static long sp;
static long spsub_addr;
static long maxsp;

#define TMP(i)		(&tmps[ntmp - 1 - (i)])

static struct tmp {
	long addr;
	long off;	/* used for LOC_SYM; offset from a symbol address */
	unsigned flags;
	unsigned bt;
} tmps[MAXTMP];
static int ntmp;

static int tmpsp;

static struct tmp *regs[NREGS];
static int tmpregs[] = {R_RAX, R_RDI, R_RSI, R_RDX, R_RCX, R_R8, R_R9};

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
		*cur++ = *s++;
}

static void oi(long n, int l)
{
	if (nogen)
		return;
	while (l--) {
		*cur++ = n;
		n >>= 8;
	}
}

static long codeaddr(void)
{
	return cur - buf;
}

static void o_op(int *op, int nop, int r1, int r2, unsigned bt)
{
	int rex = 0;
	int i;
	if (r1 & 0x8)
		rex |= 4;
	if (r2 & 0x8)
		rex |= 1;
	if (BT_SZ(bt) == 8)
		rex |= 8;
	if (rex || (bt & BT_SZMASK) == 8)
		oi(0x40 | rex, 1);
	if ((bt & BT_SZMASK) == 2)
		oi(0x66, 1);
	if ((bt & BT_SZMASK) == 1)
		op[nop - 1] &= ~0x1;
	for (i = 0; i < nop; i++)
		oi(op[i], 1);
}

static void memop(int *op, int nop, int src, int base, int off, unsigned bt)
{
	int dis = off == (char) off ? 1 : 4;
	int mod = dis == 4 ? 2 : 1;
	o_op(op, nop, src, base, bt);
	if (!off)
		mod = 0;
	oi((mod << 6) | ((src & 0x07) << 3) | (base & 0x07), 1);
	if (off)
		oi(off, dis);
}

static void memop1(int op, int src, int base, int off, unsigned bt)
{
	memop(&op, 1, src, base, off, bt);
}

static void regop(int *op, int nop, int src, int dst, unsigned bt)
{
	o_op(op, nop, src, dst, bt);
	oi((3 << 6) | (src << 3) | (dst & 0x07), 1);
}

static void regop1(int op, int src, int dst, unsigned bt)
{
	regop(&op, 1, src, dst, bt);
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
	tmp->addr = -sp_push(8);
	memop1(MOV_R2X, src, R_RBP, tmp->addr, BT_TMPBT(TMP_BT(tmp)));
	regs[src] = NULL;
	tmp->flags = LOC_NEW(tmp->flags, LOC_MEM);
}

static int *movxx_x2r(int bt)
{
	static int movxx[2] = {0x0f};
	if (BT_SZ(bt) == 1)
		movxx[1] = bt & BT_SIGNED ? 0xbe : 0xb6;
	else
		movxx[1] = bt & BT_SIGNED ? 0xbf : 0xb7;
	return movxx;
}

#define MOVSXD		0x63

static void mov_r2r(int r1, int r2, unsigned bt1, unsigned bt2)
{
	int s1 = bt1 & BT_SIGNED;
	int s2 = bt2 & BT_SIGNED;
	int sz1 = BT_SZ(bt1);
	int sz2 = BT_SZ(bt2);
	if (sz2 < 4 && (sz1 > sz2 || s1 != s2)) {
		regop(movxx_x2r(bt2), 2, r1, r2, 4);
		return;
	}
	if (sz1 == 4 && sz2 == 8) {
		regop1(MOVSXD, r2, r1, sz2);
		return;
	}
	if (r1 != r2 || sz1 > sz2)
		regop1(MOV_R2X, r1, r2, BT_TMPBT(bt2));
}

static void mov_m2r(int dst, int base, int off, int bt1, int bt2)
{
	if (BT_SZ(bt1) < 4) {
		memop(movxx_x2r(bt1), 2, dst, base, off,
			bt1 & BT_SIGNED && BT_SZ(bt2) == 8 ? 8 : 4);
		mov_r2r(dst, dst, bt1, bt2);
	} else {
		memop1(MOV_M2R, dst, base, off, bt1);
		mov_r2r(dst, dst, bt1, bt2);
	}
}

static void num_cast(struct tmp *t, unsigned bt)
{
	if (!(bt & BT_SIGNED) && BT_SZ(bt) != 8)
		t->addr &= ((1l << (long) (BT_SZ(bt) * 8)) - 1);
	if (bt & BT_SIGNED && BT_SZ(bt) != 8 &&
				t->addr > (1l << (BT_SZ(bt) * 8 - 1)))
		t->addr = -((1l << (BT_SZ(bt) * 8)) - t->addr);
	t->bt = bt;
}

static void num_reg(int reg, unsigned bt, long num)
{
	int op = MOV_I2R + (reg & 7);
	if (BT_SZ(bt) == 8 && num >= 0 && num == (unsigned) num)
		bt = 4;
	o_op(&op, 1, 0, reg, bt);
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
		tmp->bt = BT_TMPBT(bt);
		num_reg(dst, tmp->bt, tmp->addr);
		tmp->addr = dst;
		regs[dst] = tmp;
		tmp->flags = LOC_NEW(tmp->flags, LOC_REG);
	}
	if (tmp->flags & LOC_SYM) {
		regop1(MOV_I2X, 0, dst, 4);
		if (!nogen)
			out_rela(tmp->addr, codeaddr(), 0);
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
				tmp->flags & TMP_ADDR ? 8 : bt);
		regs[tmp->addr] = NULL;
	}
	if (tmp->flags & LOC_LOCAL) {
		if (deref)
			mov_m2r(dst, R_RBP, tmp->addr, tmp->bt, bt);
		else
			memop1(LEA_M2R, dst, R_RBP, tmp->addr, 8);
	}
	if (tmp->flags & LOC_MEM) {
		int nbt = deref ? 8 : TMP_BT(tmp);
		mov_m2r(dst, R_RBP, tmp->addr, nbt, nbt);
		if (deref)
			mov_m2r(dst, dst, 0, tmp->bt, bt);
	}
	tmp->addr = dst;
	tmp->bt = tmp->flags & TMP_ADDR ? bt : BT_TMPBT(bt);
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
}

void o_num(long num, unsigned bt)
{
	struct tmp *t = tmp_new();
	t->addr = num;
	t->bt = bt;
	t->flags = LOC_NUM;
}

void o_symaddr(long addr, unsigned bt)
{
	struct tmp *t = tmp_new();
	t->bt = bt;
	t->addr = addr;
	t->flags = LOC_SYM | TMP_ADDR;
	t->off = 0;
}

void o_tmpdrop(int n)
{
	int i;
	if (n == -1 || n > ntmp)
		n = ntmp;
	tmp_drop(n);
	if (!ntmp) {
		if (tmpsp != -1)
			sp = tmpsp;
		tmpsp = -1;
	}
}

#define FORK_REG		R_RAX

/* make sure tmps remain intact after a conditional expression */
void o_fork(void)
{
	int i;
	for (i = 0; i < ntmp - 1; i++)
		tmp_mem(&tmps[i]);
}

void o_forkpush(void)
{
	tmp_pop(R_RAX, 0);
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

void o_tmpcopy(void)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = tmp_new();
	memcpy(t2, t1, sizeof(*t1));
	if (!(t1->flags & (LOC_REG | LOC_MEM)))
		return;
	if (t1->flags & LOC_MEM) {
		tmp_reg(t2, reg_get(~0), t2->bt, 0);
	} else if (t1->flags & LOC_REG) {
		t2->addr = reg_get(~(1 << t1->addr));
		regop1(MOV_R2X, t1->addr, t2->addr, BT_TMPBT(TMP_BT(t1)));
		regs[t2->addr] = t2;
	}
	t2->flags = t1->flags;
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
	reg = BT_SZ(bt) == 1 ? TMP_BYTEREG(t) : TMP_REG(t);
	tmp_to(t, reg, bt);
}

long o_func_beg(char *name, int global)
{
	long addr = out_func_beg(name, global);
	cur = buf;
	os("\x55", 1);			/* push %rbp */
	os("\x48\x89\xe5", 3);		/* mov %rsp, %rbp */
	sp = 0;
	maxsp = 0;
	ntmp = 0;
	tmpsp = -1;
	nret = 0;
	cmp_last = -1;
	memset(regs, 0, sizeof(regs));
	os("\x48\x81\xec", 3);		/* sub $xxx, %rsp */
	spsub_addr = codeaddr();
	oi(0, 4);
	return addr;
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
	return BT_TMPBT(bt);
}

#define TMP_CONST(t)	((t)->flags & LOC_NUM && !((t)->flags & TMP_ADDR))
#define LOCAL_PTR(t)	((t)->flags & LOC_LOCAL && !((t)->flags & TMP_ADDR))
#define SYM_PTR(t)	((t)->flags & LOC_SYM && !((t)->flags & TMP_ADDR))

int o_popnum(long *c)
{
	struct tmp *t = TMP(0);
	if (!TMP_CONST(t))
		return 1;
	*c = t->addr;
	tmp_drop(1);
	return 0;
}

static int c_binop(long (*cop)(long a, long b, unsigned bt), unsigned bt)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	int locals = LOCAL_PTR(t1) + LOCAL_PTR(t2);
	int syms = SYM_PTR(t1) + SYM_PTR(t2);
	int nums = TMP_CONST(t1) + TMP_CONST(t2);
	if (syms == 2 || syms && locals || syms + nums + locals != 2)
		return 1;
	if (!bt) {
		if (!locals)
			bt = syms ? 8 : bt_op(t1->bt, t2->bt);
		if (locals == 1)
			bt = 8;
		if (locals == 2)
			bt = 4 | BT_SIGNED;
	}
	if (syms) {
		long o1 = SYM_PTR(t1) ? t1->off : t1->addr;
		long o2 = SYM_PTR(t2) ? t2->off : t2->addr;
		long ret = cop(o2, o1, bt);
		if (!SYM_PTR(t2))
			o_tmpswap();
		t2->off = ret;
		tmp_drop(1);
	} else {
		long ret = cop(t2->addr, t1->addr, locals ? 4 | BT_SIGNED : bt);
		tmp_drop(2);
		if (locals == 1) {
			o_local(-ret, bt);
			o_addr();
		} else {
			o_num(ret, bt);
		}
	}
	return 0;
}

static int c_op(long (*cop)(long a, unsigned bt), unsigned bt)
{
	struct tmp *t1 = TMP(0);
	long ret;
	if (!TMP_CONST(t1))
		return 1;
	if (!bt)
		bt = t1->bt;
	ret = cop(t1->addr, bt);
	tmp_drop(1);
	o_num(ret, bt);
	return 0;
}

static void shx(int uop, int sop)
{
	struct tmp *t2 = TMP(1);
	int r2 = TMP_REG2(t2, R_RCX);
	int bt;
	tmp_pop(R_RCX, 0);
	bt = tmp_pop(r2, 0);
	regop1(SHX_REG, bt & BT_SIGNED ? sop : uop, r2, BT_TMPBT(bt));
	tmp_push(r2, bt);
}

static long c_shl(long a, long b, unsigned bt)
{
	return a << b;
}

void o_shl(void)
{
	if (!c_binop(c_shl, 0))
		return;
	shx(4, 4);
}

static long c_shr(long a, long b, unsigned bt)
{
	if (bt & BT_SIGNED)
		return a >> b;
	else
		return (unsigned long) a >> b;
}

void o_shr(void)
{
	if (!c_binop(c_shr, 0))
		return;
	shx(5, 7);
}

static int mulop(int uop, int sop, int reg)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	int bt1 = TMP_BT(t1);
	int bt2 = TMP_BT(t2);
	int bt = bt_op(bt1, bt2);
	if (t1->flags & LOC_REG && t1->addr != R_RAX && t1->addr != R_RDX)
		reg = t1->addr;
	tmp_to(t1, reg, bt);
	tmp_to(t2, R_RAX, bt);
	if (reg != R_RDX) {
		int cqo = 0x99;
		reg_free(R_RDX);
		if (bt & BT_SIGNED)
			o_op(&cqo, 1, R_RAX, R_RDX, bt);
		else
			regop1(XOR_R2X, R_RDX, R_RDX, bt);
	}
	tmp_drop(2);
	regop1(MUL_A2X, bt & BT_SIGNED ? sop : uop, reg, BT_TMPBT(bt2));
	return bt;
}

static long c_mul(long a, long b, unsigned bt)
{
	return a * b;
}

void o_mul(void)
{
	int bt;
	if (!c_binop(c_mul, 0))
		return;
	bt = mulop(4, 5, R_RDX);
	tmp_push(R_RAX, bt);
}

static long c_div(long a, long b, unsigned bt)
{
	return a / b;
}

void o_div(void)
{
	int bt;
	if (!c_binop(c_div, 0))
		return;
	bt = mulop(6, 7, R_RCX);
	tmp_push(R_RAX, bt);
}

static long c_mod(long a, long b, unsigned bt)
{
	return a % b;
}

void o_mod(void)
{
	int bt;
	if (!c_binop(c_mod, 0))
		return;
	bt = mulop(6, 7, R_RCX);
	tmp_push(R_RDX, bt);
}

void o_addr(void)
{
	tmps[ntmp - 1].flags &= ~TMP_ADDR;
	tmps[ntmp - 1].bt = 8;
}

void o_ret(unsigned bt)
{
	if (bt)
		tmp_pop(R_RAX, bt);
	else
		os("\x31\xc0", 2);	/* xor %eax, %eax */
	ret[nret++] = o_jmp(0);
}

static int binop(int op, int *reg)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	int r1, r2;
	unsigned bt = bt_op(t1->bt, t2->bt);
	r1 = TMP_REG(t1);
	r2 = TMP_REG2(t2, r1);
	tmp_pop(r1, bt);
	tmp_pop(r2, bt);
	regop1(op, r2, r1, bt);
	*reg = r2;
	return bt;
}

static long c_add(long a, long b, unsigned bt)
{
	return a + b;
}

void o_add(void)
{
	int reg;
	int bt;
	if (!c_binop(c_add, 0))
		return;
	bt = binop(ADD_R2X, &reg);
	tmp_push(reg, bt);
}

static long c_xor(long a, long b, unsigned bt)
{
	return a ^ b;
}

void o_xor(void)
{
	int reg;
	int bt;
	if (!c_binop(c_xor, 0))
		return;
	bt = binop(XOR_R2X, &reg);
	tmp_push(reg, bt);
}

static long c_and(long a, long b, unsigned bt)
{
	return a & b;
}

void o_and(void)
{
	int reg;
	int bt;
	if (!c_binop(c_and, 0))
		return;
	bt = binop(AND_R2X, &reg);
	tmp_push(reg, bt);
}

static long c_or(long a, long b, unsigned bt)
{
	return a | b;
}

void o_or(void)
{
	int reg;
	int bt;
	if (!c_binop(c_or, 0))
		return;
	bt = binop(OR_R2X, &reg);
	tmp_push(reg, bt);
}

static long c_sub(long a, long b, unsigned bt)
{
	return a - b;
}

void o_sub(void)
{
	int reg;
	int bt;
	if (!c_binop(c_sub, 0))
		return;
	bt = binop(SUB_R2X, &reg);
	tmp_push(reg, bt);
}

static void o_cmp(int uop, int sop)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	char set[] = "\x0f\x00\xc0";
	int reg;
	int bt;
	if (regs[R_RAX] && regs[R_RAX] != t1 && regs[R_RAX] != t2)
		reg_free(R_RAX);
	bt = binop(CMP_R2X, &reg);
	set[1] = bt & BT_SIGNED ? sop : uop;
	cmp_setl = codeaddr();
	os(set, 3);			/* setl %al */
	os("\x0f\xb6\xc0", 3);		/* movzbl %al, %eax */
	tmp_push(R_RAX, 4 | BT_SIGNED);
	cmp_last = codeaddr();
}

static long c_lt(long a, long b, unsigned bt)
{
	return a < b;
}

void o_lt(void)
{
	if (!c_binop(c_lt, 4))
		return;
	o_cmp(0x92, 0x9c);
}

static long c_gt(long a, long b, unsigned bt)
{
	return a > b;
}

void o_gt(void)
{
	if (!c_binop(c_gt, 4))
		return;
	o_cmp(0x97, 0x9f);
}

static long c_le(long a, long b, unsigned bt)
{
	return a <= b;
}

void o_le(void)
{
	if (!c_binop(c_le, 4))
		return;
	o_cmp(0x96, 0x9e);
}

static long c_ge(long a, long b, unsigned bt)
{
	return a >= b;
}

void o_ge(void)
{
	if (!c_binop(c_ge, 4))
		return;
	o_cmp(0x93, 0x9d);
}

static long c_eq(long a, long b, unsigned bt)
{
	return a == b;
}

void o_eq(void)
{
	if (!c_binop(c_eq, 4))
		return;
	o_cmp(0x94, 0x94);
}

static long c_neq(long a, long b, unsigned bt)
{
	return a != b;
}

void o_neq(void)
{
	if (!c_binop(c_neq, 4))
		return;
	o_cmp(0x95, 0x95);
}

static long c_lnot(long a, unsigned bt)
{
	return !a;
}

void o_lnot(void)
{
	if (!c_op(c_lnot, 4))
		return;
	if (cmp_last == codeaddr()) {
		buf[cmp_setl + 1] ^= 0x01;
	} else {
		o_num(0, 4 | BT_SIGNED);
		o_eq();
	}
}

static long c_neg(long a, unsigned bt)
{
	return -a;
}

void o_neg(void)
{
	struct tmp *t = TMP(0);
	int reg;
	unsigned bt = BT_TMPBT(t->bt);
	if (!c_op(c_neg, t->bt | BT_SIGNED))
		return;
	reg = TMP_REG(t);
	tmp_to(t, reg, bt);
	regop1(NEG_REG, 3, reg, bt);
}

static long c_not(long a, unsigned bt)
{
	return ~a;
}

void o_not(void)
{
	struct tmp *t = TMP(0);
	int reg;
	unsigned bt = BT_TMPBT(t->bt);
	if (!c_op(c_not, 0))
		return;
	reg = TMP_REG(t);
	tmp_to(t, reg, bt);
	regop1(NOT_REG, 2, reg, bt);
}

void o_func_end(void)
{
	int i;
	for (i = 0; i < nret; i++)
		o_filljmp(ret[i]);
	os("\xc9\xc3", 2);		/* leave; ret; */
	putint(buf + spsub_addr, (maxsp + 7) & ~0x07, 4);
	out_func_end(buf, cur - buf);
}

long o_mklocal(int size)
{
	return sp_push((size + 7) & ~0x07);
}

void o_rmlocal(long addr, int sz)
{
	sp = addr - sz;
}

static int arg_regs[] = {R_RDI, R_RSI, R_RDX, R_RCX, R_R8, R_R9};

#define R_NARGS			ARRAY_SIZE(arg_regs)

long o_arg(int i, unsigned bt)
{
	long addr;
	if (i < R_NARGS) {
		addr = o_mklocal(BT_SZ(bt));
		memop1(MOV_R2X, arg_regs[i], R_RBP, -addr, bt);
	} else {
		addr = -8 * (i - R_NARGS + 2);
	}
	return addr;
}

void o_assign(unsigned bt)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	int r1 = BT_SZ(bt) > 1 ? TMP_REG(t1) : TMP_BYTEREG(t1);
	int reg;
	int off;
	tmp_to(t1, r1, BT_TMPBT(bt));
	if (t2->flags & LOC_LOCAL) {
		reg = R_RBP;
		off = t2->addr;
	} else {
		reg = TMP_REG2(t2, r1);
		off = 0;
		tmp_mv(t2, reg);
	}
	tmp_drop(2);
	memop1(MOV_R2X, r1, reg, off, bt);
	tmp_push(r1, bt);
}

void o_memcpy(int sz)
{
	struct tmp *t0 = TMP(-1);
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	o_num(sz, 4);
	tmp_to(t0, R_RCX, 0);
	tmp_mv(t1, R_RSI);
	tmp_mv(t2, R_RDI);
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
	tmp_to(t0, R_RAX, 0);
	tmp_to(t1, R_RCX, 0);
	tmp_mv(t2, R_RDI);
	os("\xf3\xaa", 2);		/* rep stosb */
	tmp_drop(2);
}

long o_mklabel(void)
{
	return codeaddr();
}

static long jx(int x, long addr)
{
	char op[2] = {0x0f};
	op[1] = x;
	os(op, 2);		/* jx $addr */
	oi(addr - codeaddr() - 4, 4);
	return codeaddr() - 4;
}

static long jxtest(int x, long addr)
{
	int bt = tmp_pop(R_RAX, 0);
	regop1(TEST_R2R, R_RAX, R_RAX, bt);
	return jx(x, addr);
}

static long jxcmp(long addr, int inv)
{
	int x;
	if (codeaddr() != cmp_last)
		return -1;
	tmp_drop(1);
	cur = buf + cmp_setl;
	x = (unsigned char) buf[cmp_setl + 1];
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
	oi(addr - codeaddr() - 4, 4);
	return codeaddr() - 4;
}

void o_filljmp2(long addr, long jmpdst)
{
	putint(buf + addr, jmpdst - addr - 4, 4);
}

void o_filljmp(long addr)
{
	o_filljmp2(addr, codeaddr());
}

void o_call(int argc, unsigned *bt, unsigned ret_bt)
{
	int i;
	struct tmp *t;
	for (i = 0; i < ARRAY_SIZE(tmpregs); i++)
		if (regs[tmpregs[i]] && regs[tmpregs[i]] - tmps < ntmp - argc)
			tmp_mem(regs[tmpregs[i]]);
	if (argc > R_NARGS) {
		long addr = sp_push(8 * (argc - R_NARGS));
		for (i = argc - 1; i >= R_NARGS; --i) {
			int reg = TMP_REG(TMP(0));
			tmp_pop(reg, bt[i]);
			memop1(MOV_R2X, reg, R_RBP,
				-(addr - (i - R_NARGS) * 8), BT_TMPBT(bt[i]));
		}
	}
	for (i = MIN(argc, R_NARGS) - 1; i >= 0; i--)
		tmp_pop(arg_regs[i], bt[i]);
	t = TMP(0);
	if (t->flags & LOC_SYM) {
		os("\x31\xc0", 2);	/* xor %eax, %eax */
		os("\xe8", 1);		/* call $x */
		if (!nogen)
			out_rela(t->addr, codeaddr(), 1);
		oi(-4 + t->off, 4);
		tmp_drop(1);
	} else {
		tmp_mv(TMP(0), R_RAX);
		tmp_drop(1);
		regop1(CALL_REG, 2, R_RAX, 4);
	}
	if (ret_bt)
		tmp_push(R_RAX, ret_bt);
}

int o_nogen(void)
{
	return nogen++;
}

void o_dogen(void)
{
	nogen = 0;
}

void o_datset(long addr, int off, unsigned bt)
{
	struct tmp *t = TMP(0);
	if (t->flags & LOC_NUM && !(t->flags & TMP_ADDR)) {
		num_cast(t, bt);
		out_datcpy(addr, off, (void *) &t->addr, BT_SZ(bt));
	}
	if (t->flags & LOC_SYM && !(t->flags & TMP_ADDR)) {
		out_datrela(t->addr, addr, off);
		out_datcpy(addr, off, (void *) &t->off, BT_SZ(bt));
	}
	tmp_drop(1);
}
