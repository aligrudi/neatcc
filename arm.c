/* architecture-dependent code generation for ARM */
#include <string.h>
#include "gen.h"
#include "ncc.h"
#include "out.h"
#include "tok.h"

#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))
#define oi4(i)			oi((i), 4)

#define REG_DP		10	/* data pointer register */
#define REG_TMP		12	/* temporary register */
#define REG_LR		14	/* link register */
#define REG_PC		15	/* program counter */

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

int tmpregs[] = {4, 5, 6, 7, 8, 9, 3, 2, 1, 0};
int argregs[] = {0, 1, 2, 3};

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

/* output div/mod functions */
static int putdiv = 0;

static void insert_spsub(void);

static void i_div(char *func)
{
	putdiv = 1;
	insert_spsub();
	i_call(func, 0);
}

void i_done(void)
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
}

/* for optimizing cmp + bcc */
#define OPT_ISCMP()		(last_cmp + 12 == cslen && last_set + 4 == cslen)
#define OPT_CCOND()		(*(unsigned int *) ((void *) cs + last_set) >> 28)

static long last_cmp = -1;
static long last_set = -1;

/* data pool */
static long num_offs[NNUMS];		/* data immediate value */
static char num_names[NNUMS][NAMELEN];	/* relocation data symbol name */
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
		if (num_names[i] && !pass1)
			out_rel(num_names[i], OUT_CS, cslen);
		oi4(num_offs[i]);
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

static int add_encimm(unsigned n)
{
	int i = 0;
	while (i < 12 && (n >> ((4 + i) << 1)))
		i++;
	return (n >> (i << 1)) | (((16 - i) & 0x0f) << 8);
}

static unsigned add_decimm(int n)
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

static int opcode_add(int op)
{
	/* opcode for O_ADD, O_SUB, O_AND, O_OR, O_XOR */
	static int rx[] = {I_ADD, I_SUB, I_AND, I_ORR, I_EOR};
	return rx[op & 0x0f];
}

static void i_add(int op, int rd, int rn, int rm)
{
	oi4(ADD(opcode_add(op), rd, rn, 0, 0, 14) | rm);
}

int i_imm(int op, long imm)
{
	return (op & 0xf0) != 0x20 && add_decimm(add_encimm(imm)) == imm;
}

static void i_add_imm(int op, int rd, int rn, long n)
{
	oi4(ADD(opcode_add(op), rd, rn, 0, 1, 14) | add_encimm(n));
}

static void i_ldr(int l, int rd, int rn, int off, int bt);

void i_num(int rd, long n)
{
	int enc = add_encimm(n);
	if (n == add_decimm(enc)) {
		oi4(ADD(I_MOV, rd, 0, 0, 1, 14) | enc);
		return;
	}
	enc = add_encimm(-n - 1);
	if (~n == add_decimm(enc)) {
		oi4(ADD(I_MVN, rd, 0, 0, 1, 14) | enc);
		return;
	}
	i_ldr(1, rd, REG_DP, pool_num(n), LONGSZ);
}

static void i_add_anyimm(int rd, int rn, long n)
{
	int neg = n < 0;
	int imm = add_encimm(neg ? -n : n);
	if (imm == add_decimm(neg ? -n : n)) {
		oi4(ADD(neg ? I_SUB : I_ADD, rd, rn, 0, 1, 14) | imm);
	} else {
		i_num(rd, n);
		i_add(O_ADD, rd, rd, rn);
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
	oi4(MUL(rd, rn, rm));
}

static int opcode_set(int op)
{
	/* lt, gt, le, ge, eq, neq */
	static int ucond[] = {3, 8, 9, 2, 0, 1};
	static int scond[] = {11, 12, 13, 10, 0, 1};
	return op & O_SIGNED ? scond[op & 0x0f] : ucond[op & 0x0f];
}

static void i_tst(int rn, int rm)
{
	oi4(ADD(I_TST, 0, rn, 1, 0, 14) | rm);
}

static void i_cmp(int rn, int rm)
{
	last_cmp = cslen;
	oi4(ADD(I_CMP, 0, rn, 1, 0, 14) | rm);
}

static void i_cmp_imm(int rn, long n)
{
	last_cmp = cslen;
	oi4(ADD(I_CMP, 0, rn, 1, 1, 14) | add_encimm(n));
}

static void i_set(int cond, int rd)
{
	oi4(ADD(I_MOV, rd, 0, 0, 1, 14));
	last_set = cslen;
	oi4(ADD(I_MOV, rd, 0, 0, 1, opcode_set(cond)) | 1);
}

#define SM_LSL		0
#define SM_LSR		1
#define SM_ASR		2

static int opcode_shl(int op)
{
	if (op & 0x0f)
		return op & O_SIGNED ? SM_ASR : SM_LSR;
	return SM_LSL;
}

static void i_shl(int op, int rd, int rm, int rs)
{
	int sm = opcode_shl(op);
	oi4(ADD(I_MOV, rd, 0, 0, 0, 14) | (rs << 8) | (sm << 5) | (1 << 4) | rm);
}

static void i_shl_imm(int op, int rd, int rn, long n)
{
	int sm = opcode_shl(op);
	oi4(ADD(I_MOV, rd, 0, 0, 0, 14) | (n << 7) | (sm << 5) | rn);
}

void i_mov(int rd, int rn)
{
	oi4(ADD(I_MOV, rd, 0, 0, 0, 14) | rn);
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
		oi4(ADD(neg ? I_SUB : I_ADD, REG_TMP, rn, 0, 1, 14) | imm);
		rn = REG_TMP;
		off -= add_decimm(imm);
	}
	if (!half)
		oi4(LDR(l, rd, rn, b, !neg, 1, 0) | off);
	else
		oi4(LDRH(l, rd, rn, s, h, !neg, 1) |
			((off & 0xf0) << 4) | (off & 0x0f));
}

void i_load(int rd, int rn, int off, int bt)
{
	i_ldr(1, rd, rn, off, bt);
}

void i_save(int rd, int rn, int off, int bt)
{
	i_ldr(0, rd, rn, off, bt);
}

void i_sym(int rd, char *sym, int off)
{
	int doff = pool_reloc(sym, off);
	i_ldr(1, rd, REG_DP, doff, LONGSZ);
}

static void i_neg(int rd, int r1)
{
	oi4(ADD(I_RSB, rd, r1, 0, 1, 14));
}

static void i_not(int rd, int r1)
{
	oi4(ADD(I_MVN, rd, 0, 0, 0, 14) | r1);
}

static int cond_nots[] = {1, 0, 3, 2, -1, -1, -1, -1, 9, 8, 11, 10, 13, 12, -1};

static void i_lnot(int rd, int r1)
{
	if (OPT_ISCMP()) {
		unsigned int *lset = (void *) cs + last_set;
		int cond = cond_nots[OPT_CCOND()];
		*lset = (*lset & 0x0fffffff) | (cond << 28);
		return;
	}
	i_tst(r1, r1);
	i_set(O_EQ, rd);
}

/* rd = rd & ((1 << bits) - 1) */
static void i_zx(int rd, int r1, int bits)
{
	if (bits <= 8) {
		oi4(ADD(I_AND, rd, r1, 0, 1, 14) | add_encimm((1 << bits) - 1));
	} else {
		i_shl_imm(O_SHL, rd, r1, 32 - bits);
		i_shl_imm(O_SHR, rd, rd, 32 - bits);
	}
}

static void i_sx(int rd, int r1, int bits)
{
	i_shl_imm(O_SHL, rd, r1, 32 - bits);
	i_shl_imm(O_SIGNED | O_SHR, rd, rd, 32 - bits);
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
void i_jmp(int rn, int jc, int nbytes)
{
	if (!nbytes)
		return;
	if (rn < 0) {
		oi4(BL(14, 0, 0));
		return;
	}
	if (OPT_ISCMP()) {
		int cond = OPT_CCOND();
		cslen = last_cmp + 4;
		last_set = -1;
		oi4(BL(jc ? cond_nots[cond] : cond, 0, 0));
		return;
	}
	i_tst(rn, rn);
	oi4(BL(jc ? 0 : 1, 0, 0));
}

long i_fill(long src, long dst, int nbytes)
{
	long *d = (void *) cs + src - 4;
	if (!nbytes)
		return 0;
	*d = (*d & 0xff000000) | (((dst - src - 4) >> 2) & 0x00ffffff);
	return dst - src;
}

void i_memcpy(int rd, int rs, int rn)
{
	oi4(ADD(I_SUB, rn, rn, 1, 1, 14) | 1);
	oi4(BL(4, 0, 16));
	oi4(LDR(1, REG_TMP, rs, 1, 1, 0, 0) | 1);
	oi4(LDR(0, REG_TMP, rd, 1, 1, 0, 0) | 1);
	oi4(BL(14, 0, -16));
}

void i_memset(int rd, int rs, int rn)
{
	oi4(ADD(I_SUB, rn, rn, 1, 1, 14) | 1);
	oi4(BL(4, 0, 12));
	oi4(LDR(0, rs, rd, 1, 1, 0, 0) | 1);
	oi4(BL(14, 0, -12));
}

void i_call_reg(int rd)
{
	i_mov(REG_LR, REG_PC);
	i_mov(REG_PC, rd);
}

void i_call(char *sym, int off)
{
	if (!pass1)
		out_rel(sym, OUT_CS | OUT_RLREL | OUT_RL24, cslen);
	oi4(BL(14, 1, off));
}

void i_reg(int op, int *rd, int *r1, int *r2, int *tmp)
{
	*rd = R_TMPS;
	*r1 = R_TMPS;
	*r2 = (op & O_IMM || (op & 0xf0) == 0x40) ? 0 : R_TMPS;
	*tmp = 0;
	if ((op & 0xff) == O_DIV || (op & 0xff) == O_MOD) {
		*rd = 1 << REG_RET;
		*r1 = 1 << argregs[0];
		*r2 = 1 << argregs[1];
		*tmp = R_TMPS & ~R_SAVED;
	}
}

void i_op(int op, int rd, int r1, int r2)
{
	if ((op & 0xf0) == 0x00)
		i_add(op, rd, r1, r2);
	if ((op & 0xf0) == 0x10)
		i_shl(op, rd, r1, r2);
	if ((op & 0xf0) == 0x20) {
		if ((op & 0xff) == O_MUL)
			i_mul(rd, r1, r2);
		if ((op & 0xff) == O_DIV)
			i_div(op & O_SIGNED ? "__divdi3" : "__udivdi3");
		if ((op & 0xff) == O_MOD)
			i_div(op & O_SIGNED ? "__moddi3" : "__umoddi3");
		return;
	}
	if ((op & 0xf0) == 0x30) {
		i_cmp(r1, r2);
		i_set(op, rd);
		return;
	}
	if ((op & 0xf0) == 0x40) {	/* uop */
		if ((op & 0xff) == O_NEG)
			i_neg(rd, r1);
		if ((op & 0xff) == O_NOT)
			i_not(rd, r1);
		if ((op & 0xff) == O_LNOT)
			i_lnot(rd, r1);
		return;
	}
}

void i_op_imm(int op, int rd, int r1, long n)
{
	if ((op & 0xf0) == 0x00) {
		if (i_imm(O_ADD, n))
			i_add_imm(op, rd, r1, n);
		else
			i_add_anyimm(rd, r1, n);
	}
	if ((op & 0xf0) == 0x10)	/* shl */
		i_shl_imm(op, rd, r1, n);
	if ((op & 0xf0) == 0x30) {	/* imm */
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

static int func_argc;
static int func_varg;
static int func_spsub;
static int func_sargs;
static int func_sregs;
static int func_initfp;
static int func_initdp = 1;
static int spsub_addr;
static int dpadd_addr;

static int saved_regs(int args)
{
	int n = 2;
	int i;
	for (i = 0; i < N_REGS; i++) {
		if ((1 << i) & func_sregs)
			n++;
		if (args && (1 << i) & func_sargs)
			n++;
	}
	return n;
}

int i_args(void)
{
	return saved_regs(0) * LONGSZ;
}

int i_sp(void)
{
	return 0;
}

static int plain_function(void)
{
	return !func_initfp && !func_spsub && !func_initdp && !func_varg &&
		!func_sargs && !func_sregs && func_argc <= N_ARGS;
}

static void insert_spsub(void)
{
	if (!func_spsub) {
		func_spsub = 1;
		spsub_addr = cslen;
		oi4(0xe24dd000);	/* sub   sp, sp, xx */
	}
}

void i_prolog(int argc, int varg, int sargs, int sregs, int initfp, int spsub)
{
	last_set = -1;
	nums = 0;
	func_argc = argc;
	func_varg = varg;
	func_sargs = sargs;
	func_sregs = sregs;
	func_initfp = initfp;
	func_spsub = 0;
	if (plain_function())
		return;
	if (initfp)
		func_sregs |= 1 << REG_FP;
	if (func_initdp)
		func_sregs |= 1 << REG_DP;
	/* stack should remain 8-aligned */
	if (saved_regs(1) & 0x1)
		func_sregs |= 8;
	oi4(0xe1a0c00d);			/* mov   r12, sp */
	if (func_sargs)
		oi4(0xe92d0000 | func_sargs);	/* stmfd sp!, {r0-r3} */
	oi4(0xe92d5000 | func_sregs);		/* stmfd sp!, {r0-r11, r12, lr} */
	if (func_initfp)
		oi4(0xe1a0b00d);		/* mov   fp, sp */
	if (spsub)
		insert_spsub();
	if (func_initdp) {
		dpadd_addr = cslen;
		oi4(0xe28fa000);		/* add   dp, pc, xx */
	}
}

void i_epilog(int sp_max)
{
	sp_max = -sp_max;
	if (plain_function()) {
		oi4(0xe1a0f00e);		/* mov   pc, lr */
		return;
	}
	if (func_initfp)
		oi4(0xe89ba000 | func_sregs);/* ldmfd fp, {r4-r11, sp, pc} */
	if (!func_initfp)
		oi4(0xe89da000 | func_sregs);/* ldmfd sp, {r4-r11, sp, pc} */
	if (func_initdp) {
		int dpoff = cslen - dpadd_addr - 8;
		dpoff = add_decimm(add_rndimm(add_encimm(dpoff)));
		cslen = dpadd_addr + dpoff + 8;
		/* fill data ptr addition: dp = pc + xx */
		*(long *) (cs + dpadd_addr) |= add_encimm(dpoff);
	}
	if (func_initfp && func_spsub) {
		sp_max = ALIGN(sp_max, 8);
		sp_max = add_decimm(add_rndimm(add_encimm(sp_max)));
		/* fill stack sub: sp = sp - xx */
		*(long *) (cs + spsub_addr) |= add_encimm(sp_max);
	}
	pool_write();
}
