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
#define MOV_I2R		0xc7
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

#define TMP_BT(t)		((t)->flags & TMP_ADDR ? 8 : (t)->bt)

static char buf[SECSIZE];
static char *cur;
static long sp;
static long spsub_addr;
static long maxsp;

static struct tmp {
	long addr;
	unsigned flags;
	unsigned bt;
} tmp[MAXTMP];
static int ntmp;

static char names[MAXTMP][NAMELEN];
static int nnames;
static int tmpsp;

static struct tmp *regs[NREGS];
static int tmpregs[] = {R_RAX, R_RDI, R_RSI, R_RDX, R_RCX, R_R8, R_R9};

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

static void os(char *s, int n)
{
	while (n--)
		*cur++ = *s++;
}

static void oi(long n, int l)
{
	while (l--) {
		*cur++ = n;
		n >>= 8;
	}
}

static long codeaddr(void)
{
	return cur - buf;
}

static void o_op(int op, int r1, int r2, unsigned bt)
{
	int rex = 0;
	if (r1 & 0x8)
		rex |= 4;
	if (r2 & 0x8)
		rex |= 1;
	if (rex || (bt & BT_SZMASK) == 8)
		oi(0x48 | rex, 1);
	if ((bt & BT_SZMASK) == 2)
		oi(0x66, 1);
	if ((bt & BT_SZMASK) == 1)
		op &= ~0x1;
	oi(op, 1);
}

static void memop(int op, int src, int base, int off, unsigned bt)
{
	int dis = off == (char) off ? 1 : 4;
	int mod = dis == 4 ? 2 : 1;
	o_op(op, src, base, bt);
	if (!off)
		mod = 0;
	oi((mod << 6) | ((src & 0x07) << 3) | (base & 0x07), 1);
	if (off)
		oi(off, dis);
}

static void regop(int op, int src, int dst, unsigned bt)
{
	o_op(op, src, dst, bt);
	oi((3 << 6) | (src << 3) | (dst & 0x07), 1);
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
	tmp->addr = sp_push(8);
	memop(MOV_R2X, src, R_RBP, -tmp->addr, TMP_BT(tmp));
	regs[src] = NULL;
	tmp->flags = LOC_NEW(tmp->flags, LOC_MEM);
}

static void tmp_reg(struct tmp *tmp, unsigned dst, int deref)
{
	if (!(tmp->flags & TMP_ADDR))
		deref = 0;
	if (deref)
		tmp->flags &= ~TMP_ADDR;
	if (tmp->flags & LOC_NUM) {
		regop(MOV_I2R, 0, dst, TMP_BT(tmp));
		oi(tmp->addr, BT_SZ(tmp->bt));
		tmp->addr = dst;
		regs[dst] = tmp;
		tmp->flags = LOC_NEW(tmp->flags, LOC_REG);
	}
	if (tmp->flags & LOC_SYM) {
		regop(MOV_I2R, 0, dst, TMP_BT(tmp));
		out_rela(names[tmp->addr], codeaddr(), 0);
		oi(0, 4);
		tmp->addr = dst;
		regs[dst] = tmp;
		tmp->flags = LOC_NEW(tmp->flags, LOC_REG);
	}
	if (tmp->flags & LOC_REG) {
		if (deref) {
			memop(MOV_M2R, dst, tmp->addr, 0, tmp->bt);
		} else {
			if (dst == tmp->addr)
				return;
			regop(MOV_R2X, tmp->addr, dst, TMP_BT(tmp));
		}
		regs[tmp->addr] = NULL;
		tmp->addr = dst;
		regs[dst] = tmp;
		return;
	}
	if (tmp->flags & LOC_LOCAL) {
		if (deref)
			memop(MOV_M2R, dst, R_RBP, -tmp->addr, TMP_BT(tmp));
		else
			memop(LEA_M2R, dst, R_RBP, -tmp->addr, 8);
	}
	if (tmp->flags & LOC_MEM) {
		memop(MOV_M2R, dst, R_RBP, -tmp->addr, TMP_BT(tmp));
		if (deref)
			memop(MOV_M2R, dst, dst, 0, TMP_BT(tmp));
	}
	tmp->addr = dst;
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

static unsigned tmp_pop(int deref, int reg)
{
	struct tmp *t = &tmp[--ntmp];
	reg_for(reg, t);
	tmp_reg(t, reg, deref);
	regs[reg] = NULL;
	return t->bt;
}

static void tmp_push_reg(unsigned bt, unsigned reg)
{
	struct tmp *t = &tmp[ntmp++];
	t->addr = reg;
	t->bt = bt;
	t->flags = LOC_REG;
	regs[reg] = t;
}

void o_local(long addr, unsigned bt)
{
	struct tmp *t = &tmp[ntmp++];
	t->addr = addr;
	t->bt = bt;
	t->flags = LOC_LOCAL | TMP_ADDR;
}

void o_num(long num, unsigned bt)
{
	struct tmp *t = &tmp[ntmp++];
	t->addr = num;
	t->bt = bt;
	t->flags = LOC_NUM;
}

void o_symaddr(char *name, unsigned bt)
{
	int id = nnames++;
	struct tmp *t = &tmp[ntmp++];
	t->bt = bt;
	t->addr = id;
	t->flags = LOC_SYM | TMP_ADDR;
	strcpy(names[id], name);
}

void o_tmpdrop(int n)
{
	int i;
	if (n == -1 || n > ntmp)
		n = ntmp;
	ntmp -= n;
	for (i = ntmp; i < ntmp + n; i++)
		if (tmp[i].flags & LOC_REG)
			regs[tmp[i].addr] = NULL;
	if (!ntmp) {
		nnames = 0;
		if (tmpsp != -1)
			sp = tmpsp;
		tmpsp = -1;
	}
}

#define FORK_REG		R_RAX

void o_tmpfork(void)
{
	struct tmp *t = &tmp[ntmp - 1];
	reg_for(FORK_REG, t);
	tmp_reg(t, FORK_REG, 0);
	o_tmpdrop(1);
}

void o_tmpjoin(void)
{
	struct tmp *t = &tmp[ntmp - 1];
	reg_for(FORK_REG, t);
	tmp_reg(t, FORK_REG, 0);
}

void o_tmpswap(void)
{
	struct tmp *t1 = &tmp[ntmp - 1];
	struct tmp *t2 = &tmp[ntmp - 2];
	struct tmp t;
	memcpy(&t, t1, sizeof(t));
	memcpy(t1, t2, sizeof(t));
	memcpy(t2, &t, sizeof(t));
}

static int reg_other(int not)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(tmpregs); i++)
		if (tmpregs[i] != not && !regs[tmpregs[i]])
			return tmpregs[i];
	for (i = 0; i < ARRAY_SIZE(tmpregs); i++)
		if (tmpregs[i] != not) {
			reg_free(tmpregs[i]);
			return tmpregs[i];
		}
	return 0;
}

static int reg_get(void)
{
	return reg_other(-1);
}

void o_tmpcopy(void)
{
	struct tmp *t1 = &tmp[ntmp - 1];
	struct tmp *t2 = &tmp[ntmp++];
	if (!(t1->flags & (LOC_REG | LOC_MEM))) {
		memcpy(t2, t1, sizeof(*t2));
		return;
	}
	memcpy(t2, t1, sizeof(*t1));
	if (t1->flags & LOC_MEM)
		tmp_reg(t2, reg_get(), 0);
	else if (t1->flags & LOC_REG) {
		t2->addr = reg_get();
		regop(MOV_R2X, t1->addr, t2->addr, TMP_BT(tmp));
	}
	t2->flags = t1->flags;
}

void o_func_beg(char *name)
{
	out_func_beg(name);
	cur = buf;
	os("\x55", 1);			/* push %rbp */
	os("\x48\x89\xe5", 3);		/* mov %rsp, %rbp */
	sp = 0;
	maxsp = 0;
	ntmp = 0;
	nnames = 0;
	tmpsp = -1;
	nret = 0;
	memset(regs, 0, sizeof(regs));
	os("\x48\x81\xec", 3);		/* sub $xxx, %rsp */
	spsub_addr = codeaddr();
	oi(0, 4);
}

void o_deref(unsigned bt)
{
	struct tmp *t = &tmp[ntmp - 1];
	if (t->flags & TMP_ADDR) {
		int reg = t->flags & LOC_REG ? t->addr : reg_get();
		tmp_reg(t, reg, 1);
	}
	t->flags |= TMP_ADDR;
}

void o_load(void)
{
	struct tmp *t = &tmp[ntmp - 1];
	int reg = t->flags & LOC_REG ? t->addr : reg_get();
	tmp_reg(t, reg, 1);
}

static void shx(int uop, int sop)
{
	struct tmp *t = &tmp[ntmp - 2];
	unsigned bt;
	unsigned reg = t->addr;
	if (!(t->flags & LOC_REG) || t->addr == R_RCX)
		reg = reg_other(R_RCX);
	tmp_pop(1, R_RCX);
	bt = tmp_pop(1, reg);
	regop(SHX_REG, bt & BT_SIGNED ? sop : uop, reg, bt);
	tmp_push_reg(bt, reg);
}

void o_shl(void)
{
	shx(4, 4);
}

void o_shr(void)
{
	shx(7, 5);
}

static unsigned bt_op(unsigned bt1, unsigned bt2)
{
	unsigned s1 = BT_SZ(bt1);
	unsigned s2 = BT_SZ(bt2);
	return (bt1 | bt2) & BT_SIGNED | (s1 > s2 ? s1 : s2);
}

void mulop(int uop, int sop, int reg)
{
	struct tmp *t1 = &tmp[ntmp - 1];
	struct tmp *t2 = &tmp[ntmp - 2];
	int bt1 = TMP_BT(t1);
	int bt2 = TMP_BT(t2);
	if (t1->flags & LOC_REG && t1->addr != R_RAX && t1->addr != R_RDX)
		reg = t1->addr;
	reg_for(reg, t1);
	tmp_reg(t1, reg, 1);
	reg_for(R_RAX, t2);
	tmp_reg(t2, R_RAX, 1);
	if (reg != R_RDX)
		reg_free(R_RDX);
	o_tmpdrop(2);
	regop(MUL_A2X, bt2 & BT_SIGNED ? sop : uop, reg, bt2);
	return bt_op(bt1, bt2);
}

void o_mul(void)
{
	int bt = mulop(4, 5, R_RDX);
	tmp_push_reg(bt, R_RAX);
}

void o_div(void)
{
	int bt = mulop(6, 7, R_RCX);
	tmp_push_reg(bt, R_RAX);
}

void o_mod(void)
{
	int bt = mulop(6, 7, R_RCX);
	tmp_push_reg(bt, R_RDX);
}

void o_addr(void)
{
	tmp[ntmp - 1].flags &= ~TMP_ADDR;
	tmp[ntmp - 1].bt = 8;
}

void o_ret(unsigned bt)
{
	if (bt)
		tmp_pop(1, R_RAX);
	else
		os("\x31\xc0", 2);	/* xor %eax, %eax */
	ret[nret++] = o_jmp(0);
}

static int binop(int op, int *reg)
{
	struct tmp *t1 = &tmp[ntmp - 1];
	struct tmp *t2 = &tmp[ntmp - 2];
	int r1;
	unsigned bt1, bt2, bt;
	r1 = t1->flags & LOC_REG ? t1->addr : reg_get();
	*reg = t2->flags & LOC_REG ? t2->addr : reg_other(r1);
	bt1 = tmp_pop(1, r1);
	bt2 = tmp_pop(1, *reg);
	bt = bt_op(bt1, bt2);
	regop(op, *reg, r1, bt);
	return bt;
}

void o_add(void)
{
	int reg;
	int bt = binop(ADD_R2X, &reg);
	tmp_push_reg(bt, reg);
}

void o_xor(void)
{
	int reg;
	int bt = binop(XOR_R2X, &reg);
	tmp_push_reg(bt, reg);
}

void o_and(void)
{
	int reg;
	int bt = binop(AND_R2X, &reg);
	tmp_push_reg(bt, reg);
}

void o_or(void)
{
	int reg;
	int bt = binop(OR_R2X, &reg);
	tmp_push_reg(bt, reg);
}

void o_sub(void)
{
	int reg;
	int bt = binop(SUB_R2X, &reg);
	tmp_push_reg(bt, reg);
}

static void o_cmp(int uop, int sop)
{
	char set[] = "\x0f\x00\xc0";
	int reg;
	int bt = binop(CMP_R2X, &reg);
	set[1] = bt & BT_SIGNED ? sop : uop;
	reg_free(R_RAX);
	os(set, 3);			/* setl %al */
	os("\x0f\xb6\xc0", 3);		/* movzbl %al, %eax */
	tmp_push_reg(4 | BT_SIGNED, R_RAX);
}

void o_lt(void)
{
	o_cmp(0x92, 0x9c);
}

void o_gt(void)
{
	o_cmp(0x97, 0x9f);
}

void o_le(void)
{
	o_cmp(0x96, 0x9e);
}

void o_ge(void)
{
	o_cmp(0x93, 0x9d);
}

void o_eq(void)
{
	o_cmp(0x94, 0x94);
}

void o_neq(void)
{
	o_cmp(0x95, 0x95);
}

void o_neg(void)
{
	struct tmp *t = &tmp[ntmp - 1];
	int reg = t->flags & LOC_REG ? t->addr : reg_get();
	tmp_pop(1, reg);
	regop(NEG_REG, 3, reg, t->bt);
	tmp_push_reg(t->bt, reg);
}

void o_not(void)
{
	struct tmp *t = &tmp[ntmp - 1];
	int reg = t->flags & LOC_REG ? t->addr : reg_get();
	tmp_pop(1, reg);
	regop(NOT_REG, 2, reg, t->bt);
	tmp_push_reg(t->bt, reg);
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

static int arg_regs[] = {R_RDI, R_RSI, R_RDX, R_RCX, R_R8, R_R9};

long o_arg(int i, unsigned bt)
{
	long addr = o_mklocal(BT_SZ(bt));
	memop(MOV_R2X, arg_regs[i], R_RBP, -addr, bt);
	return addr;
}

void o_assign(unsigned bt)
{
	struct tmp *t1 = &tmp[ntmp - 1];
	struct tmp *t2 = &tmp[ntmp - 2];
	int r1 = t1->flags & LOC_REG ? t1->addr : reg_get();
	int reg;
	int off;
	tmp_pop(1, r1);
	if (t2->flags & LOC_LOCAL) {
		reg = R_RBP;
		off = -t2->addr;
		o_tmpdrop(1);
	} else {
		reg = t2->flags & LOC_REG ? t2->addr : reg_other(r1);
		off = 0;
		tmp_pop(0, reg);
	}
	memop(MOV_R2X, r1, reg, off, bt);
	tmp_push_reg(bt, r1);
}

long o_mklabel(void)
{
	return codeaddr();
}

long o_jz(long addr)
{
	tmp_pop(1, R_RAX);
	os("\x48\x85\xc0", 3);		/* test %rax, %rax */
	os("\x0f\x84", 2);		/* jz $addr */
	oi(addr - codeaddr() - 4, 4);
	return codeaddr() - 4;
}

long o_jmp(long addr)
{
	os("\xe9", 1);			/* jmp $addr */
	oi(addr - codeaddr() - 4, 4);
	return codeaddr() - 4;
}

void o_filljmp(long addr)
{
	putint(buf + addr, codeaddr() - addr - 4, 4);
}

void o_call(int argc, unsigned *bt, unsigned ret_bt)
{
	int i;
	struct tmp *t;
	for (i = 0; i < argc; i++)
		tmp_pop(1, arg_regs[i]);
	t = &tmp[ntmp - 1];
	if (t->flags & LOC_SYM) {
		os("\xe8", 1);		/* call $x */
		out_rela(names[tmp->addr], codeaddr(), 1);
		oi(-4, 4);
		o_tmpdrop(1);
	} else {
		tmp_pop(0, R_RAX);
		regop(CALL_REG, 2, R_RAX, 4);
	}
	for (i = 0; i < ARRAY_SIZE(tmpregs); i++)
		if (regs[i])
			tmp_mem(regs[i]);
	if (ret_bt)
		tmp_push_reg(ret_bt, R_RAX);
}