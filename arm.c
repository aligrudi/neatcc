/* architecture-dependent code generation for ARM */
#include <stdlib.h>
#include <string.h>
#include "ncc.h"

#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))
#define oi4(i)			oi((i), 4)

#define REG_DP		10	/* data pointer register */
#define REG_TMP		12	/* temporary register */
#define REG_LR		14	/* link register */
#define REG_PC		15	/* program counter */
#define REG_RET		0	/* returned value register */

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

static struct mem cs;		/* generated code */

/* code generation functions */
static char *ointbuf(long n, int l)
{
	static char buf[16];
	int i;
	for (i = 0; i < l; i++) {
		buf[i] = n & 0xff;
		n >>= 8;
	}
	return buf;
}

static void oi(long n, int l)
{
	mem_put(&cs, ointbuf(n, l), l);
}

static void oi_at(long pos, long n, int l)
{
	mem_cpy(&cs, pos, ointbuf(n, l), l);
}

static long opos(void)
{
	return mem_len(&cs);
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

static long *rel_sym;		/* relocation symbols */
static long *rel_flg;		/* relocation flags */
static long *rel_off;		/* relocation offsets */
static long rel_n, rel_sz;	/* relocation count */

static long lab_sz;		/* label count */
static long *lab_loc;		/* label offsets in cs */
static long jmp_n, jmp_sz;	/* jump count */
static long *jmp_off;		/* jump offsets */
static long *jmp_dst;		/* jump destinations */
static long jmp_ret;		/* the position of the last return jmp */

static void lab_add(long id)
{
	while (id >= lab_sz) {
		int lab_n = lab_sz;
		lab_sz = MAX(128, lab_sz * 2);
		lab_loc = mextend(lab_loc, lab_n, lab_sz, sizeof(*lab_loc));
	}
	lab_loc[id] = opos();
}

static void jmp_add(long off, long dst)
{
	if (jmp_n == jmp_sz) {
		jmp_sz = MAX(128, jmp_sz * 2);
		jmp_off = mextend(jmp_off, jmp_n, jmp_sz, sizeof(*jmp_off));
		jmp_dst = mextend(jmp_dst, jmp_n, jmp_sz, sizeof(*jmp_dst));
	}
	jmp_off[jmp_n] = off;
	jmp_dst[jmp_n] = dst;
	jmp_n++;
}

void i_label(long id)
{
	lab_add(id + 1);
}

static void rel_add(long sym, long flg, long off)
{
	if (rel_n == rel_sz) {
		rel_sz = MAX(128, rel_sz * 2);
		rel_sym = mextend(rel_sym, rel_n, rel_sz, sizeof(*rel_sym));
		rel_flg = mextend(rel_flg, rel_n, rel_sz, sizeof(*rel_flg));
		rel_off = mextend(rel_off, rel_n, rel_sz, sizeof(*rel_off));
	}
	rel_sym[rel_n] = sym;
	rel_flg[rel_n] = flg;
	rel_off[rel_n] = off;
	rel_n++;
}

static int putdiv = 0;		/* output div/mod functions */
static int func_call;		/* */

static void i_call(long sym, long off);

static void i_div(char *func)
{
	putdiv = 1;
	func_call = 1;
	i_call(out_sym(func), 0);
}

/* data pool */
static long *num_off;			/* data immediate value */
static long *num_sym;	/* relocation data symbol name */
static int num_n, num_sz;

static int pool_find(long sym, long off)
{
	int i;
	for (i = 0; i < num_n; i++)
		if (sym == num_sym[i] && off == num_off[i])
			return i << 2;
	if (num_n == num_sz) {
		num_sz = MAX(128, num_sz * 2);
		num_off = mextend(num_off, num_n, num_sz, sizeof(*num_off));
		num_sym = mextend(num_sym, num_n, num_sz, sizeof(*num_sym));
	}
	num_off[i] = off;
	num_sym[i] = sym;
	return (num_n++) << 2;
}

static int pool_num(long num)
{
	return pool_find(-1, num);
}

static int pool_reloc(long sym, long off)
{
	return pool_find(sym, off);
}

static void pool_write(void)
{
	int i;
	for (i = 0; i < num_n; i++) {
		if (num_sym[i] >= 0)
			rel_add(num_sym[i], OUT_CS, opos());
		oi4(num_off[i]);
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

static void i_add_imm(int op, int rd, int rn, long n)
{
	oi4(ADD(opcode_add(op), rd, rn, 0, 1, 14) | add_encimm(n));
}

static void i_ldr(int l, int rd, int rn, int off, int bt);

static void i_num(int rd, long n)
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

static int opcode_set(long op)
{
	/* lt, ge, eq, ne, le, gt */
	static int ucond[] = {3, 2, 0, 1, 9, 8};
	static int scond[] = {11, 10, 0, 1, 13, 12};
	long bt = O_T(op);
	return bt & T_MSIGN ? scond[op & 0x0f] : ucond[op & 0x0f];
}

static void i_tst(int rn, int rm)
{
	oi4(ADD(I_TST, 0, rn, 1, 0, 14) | rm);
}

static void i_cmp(int rn, int rm)
{
	oi4(ADD(I_CMP, 0, rn, 1, 0, 14) | rm);
}

static void i_cmp_imm(int rn, long n)
{
	oi4(ADD(I_CMP, 0, rn, 1, 1, 14) | add_encimm(n));
}

static void i_set(int cond, int rd)
{
	oi4(ADD(I_MOV, rd, 0, 0, 1, 14));
	oi4(ADD(I_MOV, rd, 0, 0, 1, opcode_set(cond)) | 1);
}

#define SM_LSL		0
#define SM_LSR		1
#define SM_ASR		2

static int opcode_shl(long op)
{
	if (op & 0x0f)
		return O_T(op) & T_MSIGN ? SM_ASR : SM_LSR;
	return SM_LSL;
}

static void i_shl(long op, int rd, int rm, int rs)
{
	int sm = opcode_shl(op);
	oi4(ADD(I_MOV, rd, 0, 0, 0, 14) | (rs << 8) | (sm << 5) | (1 << 4) | rm);
}

static void i_shl_imm(long op, int rd, int rn, long n)
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
 * S: signed
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
	int b = T_SZ(bt) == 1;
	int h = T_SZ(bt) == 2;
	int s = l && (bt & T_MSIGN);
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

static void i_sym(int rd, long sym, long off)
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

static void i_lnot(int rd, int r1)
{
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
	i_shl_imm(O_MK(O_SHR, SLNG), rd, rd, 32 - bits);
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
static long i_jmp(long op, long rn, long rm)
{
	long pos;
	if (O_C(op) == O_JMP) {
		pos = opos();
		oi4(BL(14, 0, 0));
		return pos;
	}
	if (O_C(op) & O_JZ) {
		i_tst(rn, rn);
		pos = opos();
		oi4(BL(O_C(op) == O_JZ ? 0 : 1, 0, 0));
		return pos;
	}
	if (O_C(op) & O_JCC) {
		if (op & O_NUM)
			i_cmp_imm(rn, rm);
		else
			i_cmp(rn, rm);
		pos = opos();
		oi4(BL(opcode_set(op), 0, 0));
		return pos;
	}
	return -1;
}

static void i_memcpy(int rd, int rs, int rn)
{
	oi4(ADD(I_SUB, rn, rn, 1, 1, 14) | 1);
	oi4(BL(4, 0, 16));
	oi4(LDR(1, REG_TMP, rs, 1, 1, 0, 0) | 1);
	oi4(LDR(0, REG_TMP, rd, 1, 1, 0, 0) | 1);
	oi4(BL(14, 0, -16));
}

static void i_memset(int rd, int rs, int rn)
{
	oi4(ADD(I_SUB, rn, rn, 1, 1, 14) | 1);
	oi4(BL(4, 0, 12));
	oi4(LDR(0, rs, rd, 1, 1, 0, 0) | 1);
	oi4(BL(14, 0, -12));
}

static void i_call_reg(int rd)
{
	i_mov(REG_LR, REG_PC);
	i_mov(REG_PC, rd);
}

static void i_call(long sym, long off)
{
	rel_add(sym, OUT_CS | OUT_RLREL | OUT_RL24, opos());
	oi4(BL(14, 1, off));
}

int i_imm(long lim, long n)
{
	return add_decimm(add_encimm(n)) == n;
}

long i_reg(long op, long *rd, long *r1, long *r2, long *r3, long *tmp)
{
	long oc = O_C(op);
	*rd = 0;
	*r1 = 0;
	*r2 = 0;
	*r3 = 0;
	*tmp = 0;
	if (oc & O_MOV) {
		*rd = R_TMPS;
		*r1 = oc & (O_NUM | O_SYM) ? 32 : R_TMPS;
		return 0;
	}
	if (oc & O_MUL && oc & (O_NUM | O_SYM))
		return 1;
	if (oc == O_DIV || oc == O_MOD) {
		*rd = 1 << REG_RET;
		*r1 = 1 << argregs[0];
		*r2 = 1 << argregs[1];
		*tmp = R_TMPS & ~R_PERM;
		return 0;
	}
	if (oc & O_BOP) {
		*rd = R_TMPS;
		*r1 = R_TMPS;
		*r2 = op & O_NUM ? 0 : R_TMPS;
		return 0;
	}
	if (oc & O_UOP) {
		*rd = R_TMPS;
		*r1 = op & O_NUM ? 0 : R_TMPS;
		return 0;
	}
	if (oc == O_MSET || oc == O_MCPY) {
		*r1 = 1 << 4;
		*r2 = 1 << 5;
		*r3 = 1 << 6;
		*tmp = (1 << 4) | (1 << 6) | (oc == O_MCPY ? (1 << 5) : 0);
		return 0;
	}
	if (oc == O_RET) {
		*r1 = (1 << REG_RET);
		return 0;
	}
	if (oc & O_CALL) {
		*rd = (1 << REG_RET);
		*r1 = oc & O_SYM ? 0 : R_TMPS;
		*tmp = R_TMPS & ~R_PERM;
		return 0;
	}
	if (oc & O_LD) {
		*rd = R_TMPS;
		*r1 = R_TMPS;
		*r2 = oc & O_NUM ? 0 : R_TMPS;
		return 0;
	}
	if (oc & O_ST) {
		*r1 = R_TMPS;
		*r2 = R_TMPS;
		*r3 = oc & O_NUM ? 0 : R_TMPS;
		return 0;
	}
	if (oc & O_JZ) {
		*r1 = R_TMPS;
		return 0;
	}
	if (oc & O_JCC) {
		*r1 = R_TMPS;
		*r2 = oc & O_NUM ? 0 : R_TMPS;
		return 0;
	}
	if (oc == O_JMP)
		return 0;
	return 1;
}

long i_ins(long op, long rd, long r1, long r2, long r3)
{
	long oc = O_C(op);
	long bt = O_T(op);
	if (op & O_ADD) {
		if (op & O_NUM) {
			if (i_imm(0, r2))
				i_add_imm(op, rd, r1, r2);
			else
				i_add_anyimm(rd, r1, r2);
		} else {
			i_add(op, rd, r1, r2);
		}
	}
	if (op & O_SHL) {
		if (op & O_NUM)
			i_shl_imm(op, rd, r1, r2);
		else
			i_shl(op, rd, r1, r2);
	}
	if (op & O_MUL) {
		if (oc == O_MUL)
			i_mul(rd, r1, r2);
		if (oc == O_DIV)
			i_div(O_T(op) & T_MSIGN ? "__divdi3" : "__udivdi3");
		if (oc == O_MOD)
			i_div(O_T(op) & T_MSIGN ? "__moddi3" : "__umoddi3");
		return 0;
	}
	if (oc & O_CMP) {
		if (op & O_NUM)
			i_cmp_imm(r1, r2);
		else
			i_cmp(r1, r2);
		i_set(op, rd);
		return 0;
	}
	if (oc & O_UOP) {
		if (oc == O_NEG)
			i_neg(rd, r1);
		if (oc == O_NOT)
			i_not(rd, r1);
		if (oc == O_LNOT)
			i_lnot(rd, r1);
		return 0;
	}
	if (oc == O_CALL) {
		func_call = 1;
		i_call_reg(r1);
		return 0;
	}
	if (oc == (O_CALL | O_SYM)) {
		func_call = 1;
		i_call(r1, r2);
		return 0;
	}
	if (oc == (O_MOV | O_SYM)) {
		i_sym(rd, r1, r2);
		return 0;
	}
	if (oc == (O_MOV | O_NUM)) {
		i_num(rd, r1);
		return 0;
	}
	if (oc == O_MSET) {
		i_memset(r1, r2, r3);
		return 0;
	}
	if (oc == O_MCPY) {
		i_memcpy(r1, r2, r3);
		return 0;
	}
	if (oc == O_RET) {
		jmp_ret = opos();
		jmp_add(i_jmp(O_JMP, 0, 0), 0);
		return 0;
	}
	if (oc == (O_LD | O_NUM)) {
		i_ldr(1, rd, r1, r2, bt);
		return 0;
	}
	if (oc == (O_ST | O_NUM)) {
		i_ldr(0, r1, r2, r3, bt);
		return 0;
	}
	if (oc == O_MOV) {
		if (T_SZ(bt) == LONGSZ)
			i_mov(rd, r1);
		else {
			if (bt & T_MSIGN)
				i_sx(rd, r1, T_SZ(bt) * 8);
			else
				i_zx(rd, r1, T_SZ(bt) * 8);
		}
		return 0;
	}
	if (oc & O_JXX) {
		jmp_add(i_jmp(op, r1, r2), r3 + 1);
		return 0;
	}
	return 1;
}

void i_wrap(int argc, long sargs, long spsub, int initfp, long sregs, long sregs_pos)
{
	long body_n;
	void *body;
	long diff;		/* prologue length */
	long dpadd;
	int nsargs = 0;		/* number of saved arguments */
	int initdp = num_n > 0;	/* initialize data pointer */
	long pregs = 1;		/* registers saved in function prologue */
	int i;
	if (func_call)
		initfp = 1;
	if (!initfp && !spsub && !initdp && !sargs && argc < N_ARGS)
		pregs = 0;
	initfp = initfp || pregs;
	/* removing the last jmp to the epilogue */
	if (jmp_ret + 4 == opos()) {
		mem_cut(&cs, jmp_ret);
		jmp_n--;
	}
	lab_add(0);		/* the return label */
	body_n = mem_len(&cs);
	body = mem_get(&cs);
	/* generating function prologue */
	for (i = 0; i < N_ARGS; i++)
		if ((1 << argregs[i]) & sargs)
			nsargs++;
	if (nsargs & 0x1) {			/* keeping stack 8-aligned */
		for (i = 0; i < N_ARGS; i++)
			if (!((1 << argregs[i]) & sargs))
				break;
		sargs |= 1 << argregs[i];
	}
	if (sargs)
		oi4(0xe92d0000 | sargs);	/* stmfd sp!, {r0-r3} */
	if (pregs) {
		oi4(0xe1a0c00d);		/* mov   r12, sp */
		oi4(0xe92d5c00);		/* stmfd sp!, {sl, fp, ip, lr} */
	}
	if (initfp)
		oi4(0xe1a0b00d);		/* mov   fp, sp */
	if (sregs) {	/* sregs_pos should be encoded as immediate */
		int npos = add_decimm(add_rndimm(add_encimm(-sregs_pos)));
		spsub += npos + sregs_pos;
		sregs_pos = -npos;
	}
	if (spsub) {				/* sub   sp, sp, xx */
		spsub = ALIGN(spsub, 8);
		spsub = add_decimm(add_rndimm(add_encimm(spsub)));
		oi4(0xe24dd000 | add_encimm(spsub));
	}
	if (initdp) {
		dpadd = opos();
		oi4(0xe28fa000);		/* add   dp, pc, xx */
	}
	if (sregs) {				/* saving registers */
		oi4(0xe24bc000 | add_encimm(-sregs_pos));
		oi4(0xe88c0000 | sregs);	/* stmea ip, {r4-r9} */
	}
	diff = mem_len(&cs);
	mem_put(&cs, body, body_n);
	free(body);
	/* generating function epilogue */
	if (sregs) {				/* restoring saved registers */
		oi4(0xe24bc000 | add_encimm(-sregs_pos));
		oi4(0xe89c0000 | sregs);	/* ldmfd ip, {r4-r9} */
	}
	if (pregs) {
		oi4(0xe89bac00);		/* ldmfd fp, {sl, fp, sp, pc} */
	} else {
		oi4(0xe1a0f00e);		/* mov   pc, lr */
	}
	/* adjusting code offsets */
	for (i = 0; i < rel_n; i++)
		rel_off[i] += diff;
	for (i = 0; i < jmp_n; i++)
		jmp_off[i] += diff;
	for (i = 0; i < lab_sz; i++)
		lab_loc[i] += diff;
	/* writing the data pool */
	if (initdp) {
		int dpoff = opos() - dpadd - 8;
		dpoff = add_decimm(add_rndimm(add_encimm(dpoff)));
		mem_putz(&cs, dpadd + dpoff + 8 - opos());
		/* fill data ptr addition: dp = pc + xx */
		oi_at(dpadd, 0xe28fa000 | add_encimm(dpoff), 4);
	}
	pool_write();
}

static void i_fill(long src, long dst)
{
	long *d = mem_buf(&cs) + src;
	long c = (*d & 0xff000000) | (((dst - src - 8) >> 2) & 0x00ffffff);
	oi_at(src, c, 4);
}

void i_code(char **c, long *c_len, long **rsym, long **rflg, long **roff, long *rcnt)
{
	int i;
	for (i = 0; i < jmp_n; i++)	/* filling jmp destinations */
		i_fill(jmp_off[i], lab_loc[jmp_dst[i]]);
	*c_len = mem_len(&cs);
	*c = mem_get(&cs);
	*rsym = rel_sym;
	*rflg = rel_flg;
	*roff = rel_off;
	*rcnt = rel_n;
	rel_sym = NULL;
	rel_flg = NULL;
	rel_off = NULL;
	rel_n = 0;
	rel_sz = 0;
	jmp_n = 0;
	num_n = 0;
	func_call = 0;
}

void i_done(void)
{
	if (putdiv) {
		o_code("__udivdi3", (void *) udivdi3, sizeof(udivdi3));
		o_code("__umoddi3", (void *) umoddi3, sizeof(umoddi3));
		o_code("__divdi3", (void *) divdi3, sizeof(divdi3));
		o_code("__moddi3", (void *) moddi3, sizeof(moddi3));
	}
	free(jmp_off);
	free(jmp_dst);
	free(lab_loc);
	free(num_sym);
	free(num_off);
}
