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
#define INC_X		0xff

#define MIN(a, b)		((a) < (b) ? (a) : (b))

#define TMP_BT(t)		((t)->flags & TMP_ADDR ? 8 : (t)->bt)
#define TMP_REG(t)		((t)->flags & LOC_REG ? (t)->addr : reg_get(~0))
#define TMP_REG2(t, r)		((t)->flags & LOC_REG && (t)->addr != r ? \
					(t)->addr : reg_get(~(1 << r)))
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

#define OP2(o2, o1)		(0x010000 | ((o2) << 8) | (o1))
#define O2(op)			(((op) >> 8) & 0xff)
#define O1(op)			((op) & 0xff)
#define MODRM(m, r1, r2)	((m) << 6 | (r1) << 3 | (r2))

static void op_x(int op, int r1, int r2, int bt)
{
	int rex = 0;
	int sz = BT_SZ(bt);
	if (r1 & 0x8)
		rex |= 4;
	if (r2 & 0x8)
		rex |= 1;
	if (sz == 8)
		rex |= 8;
	if (sz == 1 && (r1 == R_RSI || r1 == R_RDI ||
			r2 == R_RSI || r2 == R_RDI))
		rex |= 0x40;
	/* hack: for movxx ops, bt does not represent the second arg */
	if (op & 0x10000 && O2(op) == 0x0f && (O1(op) & 0xf7) == 0xb6 &&
			(r2 == R_RSI || r2 == R_RDI))
		rex |= 0x40;
	if (rex || sz == 8)
		oi(0x40 | rex, 1);
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
	if (!off)
		mod = 0;
	op_x(op, src, base, bt);
	oi(MODRM(mod, src & 0x07, base & 0x07), 1);
	if (off)
		oi(off, dis);
}

static void op_rr(int op, int src, int dst, int bt)
{
	op_x(op, src, dst, bt);
	oi(MODRM(3, src & 0x07, dst & 0x07), 1);
}

static void op_rs(int op, int src, long addr, int off, int bt)
{
	op_x(op, src, 0, bt);
	oi(MODRM(0, src & 0x07, 5), 1);
	if (!nogen)
		out_rela(addr, codeaddr(), 1);
	oi(off - 4, 4);
}

static void op_ri(int op, int o3, int src, long num, int bt)
{
	op_x(op, src, 0, bt);
	oi(MODRM(3, o3, src & 0x07), 1);
	oi(num, MIN(4, BT_SZ(bt)));
}

static void op_sr(int op, int src, long addr, int off, int bt)
{
	op_x(op, src, 0, bt);
	oi(MODRM(0, src & 0x07, 5), 1);
	if (!nogen)
		out_rela(addr, codeaddr(), 1);
	oi(off - 4, 4);
}

static void op_si(int op, int o3, long addr, int off, long num, int bt)
{
	int sz = MIN(4, BT_SZ(bt));
	op_x(op, 0, 0, bt);
	oi(MODRM(0, o3, 5), 1);
	if (!nogen)
		out_rela(addr, codeaddr(), 1);
	oi(off - 4 - sz, 4);
	oi(num, sz);
}

static void op_mi(int op, int o3, int base, int off, long num, int bt)
{
	int dis = off == (char) off ? 1 : 4;
	int mod = dis == 4 ? 2 : 1;
	if (!off)
		mod = 0;
	op_x(op, 0, base, bt);
	oi(MODRM(mod, 0, base), 1);
	if (off)
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
	tmp->addr = -sp_push(8);
	op_rm(MOV_R2X, src, R_RBP, tmp->addr, BT_TMPBT(TMP_BT(tmp)));
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
		op_rr(MOV_R2X, r1, r2, BT_TMPBT(bt2));
}

static void mov_m2r(int dst, int base, int off, int bt1, int bt2)
{
	if (BT_SZ(bt1) < 4) {
		op_rm(movxx_x2r(bt1), dst, base, off,
			bt1 & BT_SIGNED && BT_SZ(bt2) == 8 ? 8 : 4);
		mov_r2r(dst, dst, bt1, bt2);
	} else {
		op_rm(MOV_M2R, dst, base, off, bt1);
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
		tmp->bt = BT_TMPBT(bt);
		num_reg(dst, tmp->bt, tmp->addr);
		tmp->addr = dst;
		regs[dst] = tmp;
		tmp->flags = LOC_NEW(tmp->flags, LOC_REG);
	}
	if (tmp->flags & LOC_SYM) {
		op_rr(MOV_I2X, 0, dst, 4);
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
			op_rm(LEA_M2R, dst, R_RBP, tmp->addr, 8);
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

void tmp_copy(struct tmp *t1)
{
	struct tmp *t2 = tmp_new();
	memcpy(t2, t1, sizeof(*t1));
	if (!(t1->flags & (LOC_REG | LOC_MEM)))
		return;
	if (t1->flags & LOC_MEM) {
		tmp_reg(t2, reg_get(~0), t2->bt, 0);
	} else if (t1->flags & LOC_REG) {
		t2->addr = reg_get(~(1 << t1->addr));
		op_rr(MOV_R2X, t1->addr, t2->addr, BT_TMPBT(TMP_BT(t1)));
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
	reg = TMP_REG(t);
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

#define TMP_NUM(t)	((t)->flags & LOC_NUM && !((t)->flags & TMP_ADDR))
#define TMP_SYM(t)	((t)->flags & LOC_SYM && (t)->flags & TMP_ADDR)
#define TMP_LOCAL(t)	((t)->flags & LOC_LOCAL && (t)->flags & TMP_ADDR)
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

static int c_op(long (*cop)(long a, unsigned bt), unsigned bt)
{
	struct tmp *t1 = TMP(0);
	long ret;
	if (!TMP_NUM(t1))
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
	op_rr(SHX_REG, bt & BT_SIGNED ? sop : uop, r2, BT_TMPBT(bt));
	tmp_push(r2, bt);
}

#define CQO_REG		0x99

static int mulop(int uop, int sop, int reg)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	int bt = bt_op(t1->bt, t2->bt);
	if (t1->flags & LOC_REG && t1->addr != R_RAX && t1->addr != R_RDX)
		reg = t1->addr;
	tmp_to(t1, reg, bt);
	tmp_to(t2, R_RAX, bt);
	if (reg != R_RDX) {
		reg_free(R_RDX);
		if (bt & BT_SIGNED)
			op_x(CQO_REG, R_RAX, R_RDX, bt);
		else
			op_rr(XOR_R2X, R_RDX, R_RDX, bt);
	}
	tmp_drop(2);
	op_rr(MUL_A2X, bt & BT_SIGNED ? sop : uop, reg, BT_TMPBT(t2->bt));
	return bt;
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

static void inc(int op)
{
	struct tmp *t = TMP(0);
	int reg;
	int off;
	if (t->flags & LOC_LOCAL) {
		reg = R_RBP;
		off = t->addr;
	} else {
		reg = TMP_REG(t);
		off = 0;
		tmp_mv(t, reg);
	}
	op_rm(INC_X, op, reg, off, t->bt);
}

void o_lnot(void)
{
	if (cmp_last == codeaddr()) {
		buf[cmp_setl + 1] ^= 0x01;
	} else {
		o_num(0, 4 | BT_SIGNED);
		o_bop(O_EQ);
	}
}

void o_neg(int id)
{
	struct tmp *t = TMP(0);
	int reg;
	unsigned bt = BT_TMPBT(t->bt);
	reg = TMP_REG(t);
	tmp_to(t, reg, bt);
	op_rr(NOT_REG, id, reg, bt);
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
		op_rm(MOV_R2X, arg_regs[i], R_RBP, -addr, BT_TMPBT(bt));
	} else {
		addr = -8 * (i - R_NARGS + 2);
	}
	return addr;
}

void o_assign(unsigned bt)
{
	struct tmp *t1 = TMP(0);
	struct tmp *t2 = TMP(1);
	int r1 = TMP_REG(t1);
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
	op_rm(MOV_R2X, r1, reg, off, bt);
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

static long cb(int op, long a, long b, int *bt)
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
	case O_MUL:
		return a * b;
	case O_DIV:
		return a / b;
	case O_MOD:
		return a % b;
	case O_SHL:
		return a << b;
	case O_SHR:
		if (*bt & BT_SIGNED)
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
	if (syms == 2 || syms && locals || syms + nums + locals != 2)
		return 1;
	if (!locals)
		bt = syms ? 8 : bt_op(t1->bt, t2->bt);
	if (locals == 1)
		bt = 8;
	if (locals == 2)
		bt = 4 | BT_SIGNED;
	if (syms) {
		long o1 = SYM_PTR(t1) ? t1->off : t1->addr;
		long o2 = SYM_PTR(t2) ? t2->off : t2->addr;
		long ret = cb(op, o2, o1, &bt);
		if (!SYM_PTR(t2))
			o_tmpswap();
		t2->off = ret;
		tmp_drop(1);
	} else {
		long ret = cb(op, t2->addr, t1->addr, &bt);
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
	static int rx[] = {0x03, 0x2b, 0x23, 0x0b, 0x33};
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
		tmp_push(R_RAX, mulop(4, 5, R_RDX));
	if ((op & 0xff) == O_DIV)
		tmp_push(R_RAX, mulop(6, 7, R_RCX));
	if ((op & 0xff) == O_MOD)
		tmp_push(R_RDX, mulop(6, 7, R_RCX));
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
	op_rr(TEST_R2R, R_RAX, R_RAX, bt);
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
			op_rm(MOV_R2X, reg, R_RBP,
				-(addr - (i - R_NARGS) * 8), BT_TMPBT(bt[i]));
		}
	}
	for (i = MIN(argc, R_NARGS) - 1; i >= 0; i--)
		tmp_pop(arg_regs[i], BT_TMPBT(bt[i]));
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
		op_rr(CALL_REG, 2, R_RAX, 4);
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
