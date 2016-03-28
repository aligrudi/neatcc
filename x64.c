/* architecture-dependent code generation for x86_64 */
#include "ncc.h"

/* x86-64 registers, without r8-r15 */
#define R_RAX		0x00
#define R_RCX		0x01
#define R_RDX		0x02
#define R_RBX		0x03
#define R_RSP		0x04
#define R_RBP		0x05
#define R_RSI		0x06
#define R_RDI		0x07

#define REG_RET		R_RAX

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

static void op_x(int op, int r1, int r2, int bt)
{
	int sz = T_SZ(bt);
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
	int sz = T_SZ(bt);
	if (sz == 4)
		return bt & T_MSIGN ? I_MOVSXD : mov;
	if (sz == 2)
		return OP2(0x0f, bt & T_MSIGN ? 0xbf : 0xb7);
	if (sz == 1)
		return OP2(0x0f, bt & T_MSIGN ? 0xbe : 0xb6);
	return mov;
}

static void mov_r2r(int rd, int r1, unsigned bt)
{
	if (rd != r1 || T_SZ(bt) != LONGSZ)
		op_rr(movrx_op(bt, I_MOVR), rd, r1, movrx_bt(bt));
}

static int i_imm(int op, long imm)
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

static void i_num(int rd, long n)
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
		if (op & O_FSIGN)
			op_x(I_CQO, R_RAX, R_RDX, LONGSZ);
		else
			i_num(R_RDX, 0);
	}
	op_rr(I_MUL, op & O_FSIGN ? 7 : 6, r2, LONGSZ);
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
		sm = op & O_FSIGN ? 7 : 5;
	op_rr(I_SHX, sm, rd, LONGSZ);
}

static void i_shl_imm(int op, int rd, int rn, long n)
{
	int sm = (op & 0x1) ? (op & O_FSIGN ? 0xf8 : 0xe8) : 0xe0;
	char s[4] = {REX(0, rn), 0xc1, sm | (rn & 7), n & 0xff};
	os(s, 4);
}

static void i_sym(int rd, int sym, int off)
{
	int sz = X64_ABS_RL & OUT_RL32 ? 4 : LONGSZ;
	if (X64_ABS_RL & OUT_RLSX)
		op_rr(I_MOVI, 0, rd, sz);
	else
		op_x(I_MOVIR + (rd & 7), 0, rd, sz);
	out_rel(sym, OUT_CS | X64_ABS_RL, opos());
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

static void i_set(int op, int rd)
{
	/* lt, ge, eq, ne, le, gt */
	static int ucond[] = {0x92, 0x93, 0x94, 0x95, 0x96, 0x97};
	static int scond[] = {0x9c, 0x9d, 0x94, 0x95, 0x9e, 0x9f};
	int cond = op & O_FSIGN ? scond[op & 0x0f] : ucond[op & 0x0f];
	char set[] = "\x0f\x00\xc0";
	set[1] = cond;
	os(set, 3);			/* setl al */
	os("\x48\x0f\xb6\xc0", 4);	/* movzx rax, al */
}

static void i_lnot(int rd)
{
	char cmp[] = "\x00\x83\xf8\x00";
	cmp[0] = REX(0, rd);
	cmp[2] |= rd & 7;
	os(cmp, 4);		/* cmp rax, 0 */
	i_set(O_EQ, rd);
}

static long i_jmp(int rn, int z, int nbytes)
{
	long ret;
	if (nbytes > 1)
		nbytes = 4;
	if (rn >= 0) {
		i_tst(rn, rn);
		if (nbytes == 1) {
			oi(z ? 0x74 : 0x75, 1);	/* jx $addr */
		} else {
			char op[2] = {0x0f, z ? 0x84 : 0x85};
			os(op, 2);		/* jx $addr */
		}
		ret = opos();
		oi(0, nbytes);
	} else {
		os(nbytes == 1 ? "\xeb" : "\xe9", 1);	/* jmp $addr */
		ret = opos();
		oi(0, nbytes);
	}
	return ret;
}

void i_fill(long src, long dst, long nbytes)
{
	if (nbytes > 1)
		nbytes = 4;
	oi_at(src, dst - src - nbytes, nbytes);
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
	mov_r2r(rd, r1, T_MSIGN | (bits >> 3));
}

static void i_cast(int rd, int rn, int bt)
{
	if (T_SZ(bt) == 8) {
		if (rd != rn)
			i_mov(rd, rn);
	} else {
		if (bt & T_MSIGN)
			i_sx(rd, rn, T_SZ(bt) * 8);
		else
			i_zx(rd, rn, T_SZ(bt) * 8);
	}
}

static void i_add_anyimm(int rd, int rn, long n)
{
	op_rm(I_LEA, rd, rn, n, LONGSZ);
}

static void i_op_imm(int op, int rd, int r1, long n)
{
	if (op & O_FADD) {	/* add */
		if (rd == r1 && i_imm(O_ADD, n))
			i_add_imm(op, rd, r1, n);
		else
			i_add_anyimm(rd, r1, n);
	}
	if (op & O_FSHT)	/* shl */
		i_shl_imm(op, rd, r1, n);
	if (op & O_FMUL)	/* mul */
		die("mul/imm not implemented");
	if (op & O_FCMP) {	/* cmp */
		i_cmp_imm(r1, n);
		i_set(op, rd);
	}
}

static int func_sargs;		/* saved arguments */
static int func_sregs;		/* saved registers */
static int func_initfp;
static int func_nsregs;		/* number of saved registers */
static int func_nsargs;		/* number of saved arguments */
static int func_spsub;		/* stack pointer subtraction */

static void i_saveargs(void)
{
	int i;
	os("\x58", 1);		/* pop rax */
	for (i = N_ARGS - 1; i >= 0; i--)
		if ((1 << argregs[i]) & func_sargs)
			i_push(argregs[i]);
	os("\x50", 1);		/* push rax */
}

void i_prolog(int argc, int varg, int sargs, int sregs, int initfp, int spsub)
{
	int i;
	int mod16;
	int nsregs = 0;
	int nsargs = 0;
	func_sargs = sargs;
	func_sregs = sregs;
	func_initfp = initfp;
	func_spsub = 0;
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
	for (i = 0; i < N_TMPS; i++)
		if ((1 << tmpregs[i]) & func_sregs)
			nsregs++;
	for (i = 0; i < N_ARGS; i++)
		if ((1 << argregs[i]) & func_sargs)
			nsargs++;
	func_nsregs = nsregs;
	func_nsargs = nsargs;
	/* forcing 16-byte alignment */
	mod16 = (spsub + (nsargs + nsregs) * LONGSZ) % 16;
	if (spsub) {
		os("\x48\x81\xec", 3);
		func_spsub = spsub + (16 - mod16);
		oi(func_spsub, 4);
	}
}

static void i_ret(void)
{
	int i;
	if (func_spsub)
		i_add_anyimm(R_RSP, R_RBP, -func_nsregs * LONGSZ);
	if (func_sregs) {
		for (i = 0; i < N_TMPS; i++)
			if ((1 << tmpregs[i]) & func_sregs)
				i_pop(tmpregs[i]);
	}
	if (func_initfp)
		os("\xc9", 1);		/* leave */
	if (func_sargs) {
		os("\xc2", 1);		/* ret n */
		oi(func_nsargs * LONGSZ, 2);
	} else {
		os("\xc3", 1);		/* ret */
	}
}

void i_epilog(void)
{
}

void i_done(void)
{
}

long i_reg(long op, long *rd, long *r1, long *r2, long *tmp, long bt)
{
	*rd = 0;
	*r1 = R_TMPS;
	*r2 = op & O_FNUM ? 0 : R_TMPS;
	*tmp = 0;
	if (op & O_FADD)	/* add */
		return 0;
	if (op & O_FSHT) {
		if (~op & O_FNUM) {
			*r2 = 1 << R_RCX;
			*r1 = R_TMPS & ~*r2;
		}
		return 0;
	}
	if (op & O_FMUL) {	/* mul */
		*rd = (op & ~O_FSIGN) == O_MOD ? (1 << R_RDX) : (1 << R_RAX);
		*r1 = (1 << R_RAX);
		*r2 = R_TMPS & ~*rd & ~*r1;
		if ((op & ~O_FSIGN) == O_DIV)
			*r2 &= ~(1 << R_RDX);
		*tmp = (1 << R_RDX) | (1 << R_RAX);
		return 0;
	}
	if (op & O_FCMP) {	/* cmp */
		*rd = 1 << R_RAX;
		return 0;
	}
	if (op & O_FUOP) {	/* uop */
		*r2 = 0;
		if (op == O_LNOT)
			*r1 = 1 << R_RAX;
		return 0;
	}
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
	if (op == O_MOV) {
		*rd = R_TMPS;
		*r1 = R_TMPS;
		*r2 = 0;
	}
	if (op == O_RET) {
		*rd = (1 << REG_RET);
		*r1 = 0;
		*r2 = 0;
	}
	if (op == O_CALL) {
		*rd = (1 << REG_RET);
		*r1 = R_TMPS;
		*r2 = 0;
	}
	if (op == O_SAVE || op == O_LOAD) {
		*rd = R_TMPS;
		*r1 = R_TMPS;
		*r2 = 0;
	}
	if (op == O_JZ || op == O_SYM) {
		*rd = R_TMPS;
		*r1 = 0;
		*r2 = 0;
	}
	return 0;
}

long i_ins(long op, long r0, long r1, long r2, long bt)
{
	if (op & O_FADD) {
		if (op & O_FNUM)
			i_op_imm(O_ADD, r0, r1, r2);
		else
			i_add(op, r1, r1, r2);
	}
	if (op & O_FSHT)
		i_shl(op, r1, r1, r2);
	if (op & O_FMUL) {
		if ((op & ~O_FSIGN) == O_MUL)
			i_mul(R_RAX, r1, r2);
		if ((op & ~O_FSIGN) == O_DIV)
			i_div(op, R_RAX, r1, r2);
		if ((op & ~O_FSIGN) == O_MOD)
			i_div(op, R_RDX, r1, r2);
		return 0;
	}
	if (op & O_FCMP) {
		i_cmp(r1, r2);
		i_set(op, r0);
		return 0;
	}
	if (op & O_FUOP) {	/* uop */
		if (op == O_NEG)
			i_neg(r1);
		if (op == O_NOT)
			i_not(r1);
		if (op == O_LNOT)
			i_lnot(r1);
		return 0;
	}
	if (op == O_CALL) {
		op_rr(I_CALL, 2, r1, LONGSZ);
		return 0;
	}
	if (op == (O_CALL | O_FSYM)) {
		os("\xe8", 1);		/* call $x */
		out_rel(r2, OUT_CS | OUT_RLREL, opos());
		oi(-4 + r1, 4);
		return 0;
	}
	if (op == O_SYM) {
		i_sym(r0, r2, r1);
		return 0;
	}
	if (op == O_NUM) {
		i_num(r0, r2);
		return 0;
	}
	if (op == O_MSET) {
		os("\xfc\xf3\xaa", 3);		/* cld; rep stosb */
		return 0;
	}
	if (op == O_MCPY) {
		os("\xfc\xf3\xa4", 3);		/* cld; rep movs */
		return 0;
	}
	if (op == O_RET) {
		i_ret();
		return 0;
	}
	if (op == O_LOAD) {
		op_rm(movrx_op(bt, I_MOVR), r0, r1, r2, movrx_bt(bt));
		return 0;
	}
	if (op == O_SAVE) {
		op_rm(I_MOV, r0, r1, r2, bt);
		return 0;
	}
	if (op == O_MOV) {
		i_cast(r0, r1, bt);
		return 0;
	}
	if (op == O_JMP) {
		return i_jmp(-1, 0, r2);
	}
	if (op == O_JZ) {
		return i_jmp(r0, 1, r2);
	}
	return 1;
}
