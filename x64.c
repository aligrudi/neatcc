/* architecture-dependent code generation for x86_64 */
#include "tok.h"
#include "gen.h"
#include "out.h"

/* registers */
#define R_RAX		0x00
#define R_RCX		0x01
#define R_RDX		0x02
#define R_RBX		0x03
#define R_RSP		0x04
#define R_RBP		0x05
#define R_RSI		0x06
#define R_RDI		0x07
/* x86_64 registers */
#define R_R8		0x08
#define R_R9		0x09
#define R_R10		0x0a
#define R_R11		0x0b
#define R_R12		0x0c
#define R_R13		0x0d
#define R_R14		0x0e
#define R_R15		0x0f

/* x86 opcodes */
#define I_MOV		0x89
#define I_MOVI		0xc7
#define I_MOVIR		0xb8
#define I_MOVR		0x8b
#define I_MOVSXD	0x63
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

#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))

int tmpregs[] = {0, 7, 6, 2, 1, 8, 9, 10, 11, 3, 12, 13, 14, 15};
int argregs[] = {7, 6, 2, 1, 8, 9};

#define OP2(o2, o1)		(0x010000 | ((o2) << 8) | (o1))
#define O2(op)			(((op) >> 8) & 0xff)
#define O1(op)			((op) & 0xff)
#define MODRM(m, r1, r2)	((m) << 6 | (r1) << 3 | (r2))
#define REX(r1, r2)		(0x48 | (((r1) & 8) >> 1) | (((r2) & 8) >> 3))

static void putint(char *s, long n, int l)
{
	while (l--) {
		*s++ = n;
		n >>= 8;
	}
}

static void op_x(int op, int r1, int r2, int bt)
{
	int sz = BT_SZ(bt);
	int rex = 0;
	if (sz == 8)
		rex |= 8;
	if (sz == 1)
		rex |= 0x40;
	if (r1 & 0x8)
		rex |= 4;
	if (r2 & 0x8)
		rex |= 1;
	if (sz == 2)
		oi(0x66, 1);
	if (rex)
		oi(rex | 0x40, 1);
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

#define movrx_bt(bt)		(((bt) == 4) ? 4 : LONGSZ)

static int movrx_op(int bt, int mov)
{
	int sz = BT_SZ(bt);
	if (sz == 4)
		return bt & BT_SIGNED ? I_MOVSXD : mov;
	if (sz == 2)
		return OP2(0x0f, bt & BT_SIGNED ? 0xbf : 0xb7);
	if (sz == 1)
		return OP2(0x0f, bt & BT_SIGNED ? 0xbe : 0xb6);
	return mov;
}

static void mov_r2r(int rd, int r1, unsigned bt)
{
	if (rd != r1 || BT_SZ(bt) != LONGSZ)
		op_rr(movrx_op(bt, I_MOVR), rd, r1, movrx_bt(bt));
}

int i_imm(int op, long imm)
{
	if ((op & 0xf0) == 0x20)
		return 0;
	return imm <= 127 && imm >= -128;
}

static void i_push(int reg)
{
	op_x(I_PUSH | (reg & 0x7), 0, reg, LONGSZ);
}

static void i_pop(int reg)
{
	op_x(I_POP | (reg & 0x7), 0, reg, LONGSZ);
}

void i_mov(int rd, int rn)
{
	op_rr(movrx_op(LONGSZ, I_MOVR), rd, rn, movrx_bt(LONGSZ));
}

void i_load(int rd, int rn, int off, int bt)
{
	op_rm(movrx_op(bt, I_MOVR), rd, rn, off, movrx_bt(bt));
}

void i_save(int rd, int rn, int off, int bt)
{
	op_rm(I_MOV, rd, rn, off, bt);
}

void i_reg(int op, int *rd, int *r1, int *r2, int *tmp)
{
	*rd = 0;
	*r1 = R_TMPS;
	*r2 = op & O_IMM ? 0 : R_TMPS;
	*tmp = 0;
	if ((op & 0xf0) == 0x00)	/* add */
		return;
	if ((op & 0xf0) == 0x10) {	/* shl */
		if (~op & O_IMM) {
			*r2 = 1 << R_RCX;
			*r1 = R_TMPS & ~*r2;
		}
		return;
	}
	if ((op & 0xf0) == 0x20) {	/* mul */
		*rd = (op & 0xff) == O_MOD ? (1 << R_RDX) : (1 << R_RAX);
		*r1 = (1 << R_RAX);
		*r2 = R_TMPS & ~*rd & ~*r1;
		if ((op & 0xff) == O_DIV)
			*r2 &= ~(1 << R_RDX);
		*tmp = (1 << R_RDX) | (1 << R_RAX);
		return;
	}
	if ((op & 0xf0) == 0x30) {	/* cmp */
		*rd = 1 << R_RAX;
		return;
	}
	if ((op & 0xf0) == 0x40) {	/* uop */
		*r2 = 0;
		if ((op & 0xff) == O_LNOT)
			*r1 = 1 << R_RAX;
		return;
	}
	if ((op & 0xf0) == 0x50) {	/* etc */
		if (op == O_MSET) {
			*rd = 1 << R_RDI;
			*r1 = 1 << R_RAX;
			*r2 = 1 << R_RCX;
		}
		if (op == O_MCPY) {
			*rd = 1 << R_RDI;
			*r1 = 1 << R_RSI;
			*r2 = 1 << R_RCX;
		}
		if (op == O_SX || op == O_ZX || op == O_MOV) {
			*rd = R_TMPS;
			*r2 = 0;
		}
		return;
	}
}

static void i_add(int op, int rd, int r1, int r2)
{
	/* opcode for O_ADD, O_SUB, O_AND, O_OR, O_XOR */
	static int rx[] = {0003, 0053, 0043, 0013, 0063};
	op_rr(rx[op & 0x0f], rd, r2, LONGSZ);
}

static void i_add_imm(int op, int rd, int rn, long n)
{
	/* opcode for O_ADD, O_SUB, O_AND, O_OR, O_XOR */
	static int rx[] = {0xc0, 0xe8, 0xe0, 0xc8, 0xf0};
	unsigned char s[4] = {REX(0, rd), 0x83, rx[op & 0x0f] | (rd & 7), n & 0xff};
	os((void *) s, 4);
}

void i_num(int rd, long n)
{
	if (!n) {
		op_rr(I_XOR, rd, rd, 4);
		return;
	}
	if (n < 0 && -n <= 0xffffffff) {
		op_rr(I_MOVI, 0, rd, LONGSZ);
		oi(n, 4);
	} else {
		int len = 8;
		if (n > 0 && n <= 0xffffffff)
			len = 4;
		op_x(I_MOVIR + (rd & 7), 0, rd, len);
		oi(n, len);
	}
}

static void i_mul(int rd, int r1, int r2)
{
	if (r2 != R_RDX)
		i_num(R_RDX, 0);
	op_rr(I_MUL, 4, r2, LONGSZ);
}

static void i_div(int op, int rd, int r1, int r2)
{
	if (r2 != R_RDX) {
		if (op & O_SIGNED)
			op_x(I_CQO, R_RAX, R_RDX, LONGSZ);
		else
			i_num(R_RDX, 0);
	}
	op_rr(I_MUL, op & O_SIGNED ? 7 : 6, r2, LONGSZ);
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
	unsigned char s[4] = {REX(0, rn), 0x83, 0xf8 | rn, n & 0xff};
	os(s, 4);
}

static void i_shl(int op, int rd, int r1, int rs)
{
	int sm = 4;
	if ((op & 0x0f) == 1)
		sm = op & O_SIGNED ? 7 : 5;
	op_rr(I_SHX, sm, rd, LONGSZ);
}

static void i_shl_imm(int op, int rd, int rn, long n)
{
	int sm = (op & 0x1) ? (op & O_SIGNED ? 0xf8 : 0xe8) : 0xe0 ;
	char s[4] = {REX(0, rn), 0xc1, sm | (rn & 7), n & 0xff};
	os(s, 4);
}

void i_sym(int rd, char *sym, int off)
{
	int sz = X64_ABS_RL & OUT_RL32 ? 4 : LONGSZ;
	if (X64_ABS_RL & OUT_RLSX)
		op_rr(I_MOVI, 0, rd, sz);
	else
		op_x(I_MOVIR + (rd & 7), 0, rd, sz);
	if (!pass1)
		out_rel(sym, OUT_CS | X64_ABS_RL, cslen);
	oi(off, sz);
}

static void i_neg(int rd)
{
	op_rr(I_NOT, 3, rd, LONGSZ);
}

static void i_not(int rd)
{
	op_rr(I_NOT, 2, rd, LONGSZ);
}

/* for optimizing cmp + tst + jmp to cmp + jmp */
#define OPT_ISCMP()		(last_set >= 0 && last_set + 7 == cslen)
#define OPT_CCOND()		(cs[last_set + 1])

static long last_set = -1;

static void i_set(int op, int rd)
{
	/* lt, gt, le, ge, eq, neq */
	static int ucond[] = {0x92, 0x97, 0x96, 0x93, 0x94, 0x95};
	static int scond[] = {0x9c, 0x9f, 0x9e, 0x9d, 0x94, 0x95};
	int cond = op & O_SIGNED ? scond[op & 0x0f] : ucond[op & 0x0f];
	char set[] = "\x0f\x00\xc0";
	set[1] = cond;
	last_set = cslen;
	os(set, 3);			/* setl al */
	os("\x48\x0f\xb6\xc0", 4);	/* movzx rax, al */
}

static void i_lnot(int rd)
{
	if (OPT_ISCMP()) {
		cs[last_set + 1] ^= 0x01;
	} else {
		char cmp[] = "\x00\x83\xf8\x00";
		cmp[0] = REX(0, rd);
		cmp[2] |= rd & 7;
		os(cmp, 4);		/* cmp rax, 0 */
		i_set(O_EQ, rd);
	}
}

static void jx(int x, int nbytes)
{
	char op[2] = {0x0f};
	if (nbytes == 1) {
		op[0] = 0x70 | (x & 0x0f);
		os(op, 1);		/* jx $addr */
	} else {
		op[1] = x;
		os(op, 2);		/* jx $addr */
	}
	oi(0, nbytes);
}

void i_jmp(int rn, int z, int nbytes)
{
	if (!nbytes)
		return;
	if (nbytes > 1)
		nbytes = 4;
	if (rn >= 0) {
		if (OPT_ISCMP()) {
			int cond = OPT_CCOND();
			cslen = last_set;
			jx((!z ? cond : cond ^ 0x01) & ~0x10, nbytes);
			last_set = -1;
		} else {
			i_tst(rn, rn);
			jx(z ? 0x84 : 0x85, nbytes);
		}
	} else {
		os(nbytes == 1 ? "\xeb" : "\xe9", 1);		/* jmp $addr */
		oi(0, nbytes);
	}
}

long i_fill(long src, long dst, int nbytes)
{
	if (!nbytes)
		return 0;
	if (nbytes > 1)
		nbytes = 4;
	putint((void *) (cs + src - nbytes), dst - src, nbytes);
	return dst - src;
}

static void i_zx(int rd, int r1, int bits)
{
	if (bits & 0x07) {
		i_shl_imm(O_SHL, rd, rd, LONGSZ * 8 - bits);
		i_shl_imm(O_SHR, rd, rd, LONGSZ * 8 - bits);
	} else {
		mov_r2r(rd, r1, bits >> 3);
	}
}

static void i_sx(int rd, int r1, int bits)
{
	mov_r2r(rd, r1, BT_SIGNED | (bits >> 3));
}

void i_op(int op, int rd, int r1, int r2)
{
	if ((op & 0xf0) == 0x00)
		i_add(op, r1, r1, r2);
	if ((op & 0xf0) == 0x10)
		i_shl(op, r1, r1, r2);
	if ((op & 0xf0) == 0x20) {
		if ((op & 0xff) == O_MUL)
			i_mul(R_RAX, r1, r2);
		if ((op & 0xff) == O_DIV)
			i_div(op, R_RAX, r1, r2);
		if ((op & 0xff) == O_MOD)
			i_div(op, R_RDX, r1, r2);
		return;
	}
	if ((op & 0xf0) == 0x30) {
		i_cmp(r1, r2);
		i_set(op, rd);
		return;
	}
	if ((op & 0xf0) == 0x40) {	/* uop */
		if ((op & 0xff) == O_NEG)
			i_neg(r1);
		if ((op & 0xff) == O_NOT)
			i_not(r1);
		if ((op & 0xff) == O_LNOT)
			i_lnot(r1);
		return;
	}
}

static void i_add_anyimm(int rd, int rn, long n)
{
	op_rm(I_LEA, rd, rn, n, LONGSZ);
}

void i_op_imm(int op, int rd, int r1, long n)
{
	if ((op & 0xf0) == 0x00) {	/* add */
		if (rd == r1 && i_imm(O_ADD, n))
			i_add_imm(op, rd, r1, n);
		else
			i_add_anyimm(rd, r1, n);
	}
	if ((op & 0xf0) == 0x10)	/* shl */
		i_shl_imm(op, rd, r1, n);
	if ((op & 0xf0) == 0x20)	/* mul */
		die("mul/imm not implemented");
	if ((op & 0xf0) == 0x30) {	/* cmp */
		i_cmp_imm(r1, n);
		i_set(op, rd);
	}
	if ((op & 0xf0) == 0x50) {	/* etc */
		if ((op & 0xff) == O_ZX)
			i_zx(rd, r1, n);
		if ((op & 0xff) == O_SX)
			i_sx(rd, r1, n);
		if ((op & 0xff) == O_MOV)
			i_mov(rd, r1);
	}
}

void i_memcpy(int r0, int r1, int r2)
{
	os("\xfc\xf3\xa4", 3);		/* cld; rep movs */
}

void i_memset(int r0, int r1, int r2)
{
	os("\xfc\xf3\xaa", 3);		/* cld; rep stosb */
}

void i_call_reg(int rd)
{
	op_rr(I_CALL, 2, rd, LONGSZ);
}

void i_call(char *sym, int off)
{
	os("\xe8", 1);		/* call $x */
	if (!pass1)
		out_rel(sym, OUT_CS | OUT_RLREL, cslen);
	oi(-4 + off, 4);
}

static int func_argc;
static int func_varg;
static int func_spsub;
static int func_sargs;
static int func_sregs;
static int func_initfp;
static int spsub_addr;

int i_args(void)
{
	return 16;
}

int i_sp(void)
{
	int i;
	int n = 0;
	for (i = 0; i < N_TMPS; i++)
		if ((1 << tmpregs[i]) & func_sregs)
			n += 8;
	return -n;
}

static void i_saveargs(void)
{
	int i;
	os("\x58", 1);		/* pop rax */
	for (i = N_ARGS - 1; i >= 0; i--)
		if ((1 << argregs[i]) & func_sargs)
			i_push(argregs[i]);
	os("\x50", 1);		/* push rax */
}

void i_prolog(int argc, int varg, int sargs, int sregs, int initfp, int subsp)
{
	int i;
	last_set = -1;
	func_argc = argc;
	func_varg = varg;
	func_sargs = sargs;
	func_sregs = sregs;
	func_initfp = initfp;
	func_spsub = subsp;
	if (func_sargs)
		i_saveargs();
	if (initfp) {
		os("\x55", 1);			/* push rbp */
		os("\x48\x89\xe5", 3);		/* mov rbp, rsp */
	}
	if (func_sregs) {
		for (i = N_TMPS - 1; i >= 0; i--)
			if ((1 << tmpregs[i]) & func_sregs)
				i_push(tmpregs[i]);
	}
	if (func_spsub) {
		os("\x48\x81\xec", 3);		/* sub rsp, $xxx */
		spsub_addr = cslen;
		oi(0, 4);
	}
}

void i_epilog(int sp_max)
{
	int diff;
	int nsregs = 0;
	int nsargs = 0;
	int i;
	for (i = 0; i < N_TMPS; i++)
		if ((1 << tmpregs[i]) & func_sregs)
			nsregs++;
	for (i = 0; i < N_ARGS; i++)
		if ((1 << argregs[i]) & func_sargs)
			nsargs++;
	diff = ALIGN(-sp_max - nsregs * LONGSZ, 16);
	/* forcing 16-byte alignment */
	diff = (nsregs + nsargs) & 1 ? diff + LONGSZ : diff;
	if (func_spsub && diff) {
		i_add_anyimm(R_RSP, R_RBP, -nsregs * LONGSZ);
		putint(cs + spsub_addr, diff, 4);
	}
	if (func_sregs) {
		for (i = 0; i < N_TMPS; i++)
			if ((1 << tmpregs[i]) & func_sregs)
				i_pop(tmpregs[i]);
	}
	if (func_initfp)
		os("\xc9", 1);		/* leave */
	if (func_sargs) {
		os("\xc2", 1);		/* ret n */
		oi(nsargs * LONGSZ, 2);
	} else {
		os("\xc3", 1);		/* ret */
	}
}

void i_done(void)
{
}
