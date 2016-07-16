/* architecture-dependent code generation for x86 */
#include <stdlib.h>
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
#define R_BYTE		0x0007

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
#define I_CQO		0x99
#define I_PUSH		0x50
#define I_POP		0x58

#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))

int tmpregs[] = {0, 1, 2, 6, 7, 3};
int argregs[] = {0};

#define OP2(o2, o1)		(0x010000 | ((o2) << 8) | (o1))
#define O2(op)			(((op) >> 8) & 0xff)
#define O1(op)			((op) & 0xff)
#define MODRM(m, r1, r2)	((m) << 6 | (r1) << 3 | (r2))

static struct mem cs;		/* generated code */

/* code generation functions */
static void os(void *s, int n)
{
	mem_put(&cs, s, n);
}

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

static void op_x(int op, int r1, int r2, int bt)
{
	int sz = T_SZ(bt);
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

#define movrx_bt(bt)		(LONGSZ)

static int movrx_op(int bt, int mov)
{
	int sz = T_SZ(bt);
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

static void i_mov(int rd, int rn)
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
	unsigned char s[4] = {0x83, rx[op & 0x0f] | rd, n & 0xff};
	os((void *) s, 3);
}

static void i_num(int rd, long n)
{
	if (!n) {
		op_rr(I_XOR, rd, rd, 4);
		return;
	} else {
		op_x(I_MOVIR + (rd & 7), 0, rd, LONGSZ);
		oi(n, LONGSZ);
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
	long bt = O_T(op);
	if (r2 != R_RDX) {
		if (bt & T_MSIGN)
			op_x(I_CQO, R_RAX, R_RDX, LONGSZ);
		else
			i_num(R_RDX, 0);
	}
	op_rr(I_MUL, bt & T_MSIGN ? 7 : 6, r2, LONGSZ);
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
	unsigned char s[4] = {0x83, 0xf8 | rn, n & 0xff};
	os(s, 3);
}

static void i_shl(int op, int rd, int r1, int rs)
{
	long bt = O_T(op);
	int sm = 4;
	if ((op & 0x0f) == 1)
		sm = bt & T_MSIGN ? 7 : 5;
	op_rr(I_SHX, sm, rd, LONGSZ);
}

static void i_shl_imm(int op, int rd, int rn, long n)
{
	long bt = O_T(op);
	int sm = (op & 0x1) ? (bt & T_MSIGN ? 0xf8 : 0xe8) : 0xe0;
	char s[4] = {0xc1, sm | rn, n & 0xff};
	os(s, 3);
}

static void i_neg(int rd)
{
	op_rr(I_NOT, 3, rd, LONGSZ);
}

static void i_not(int rd)
{
	op_rr(I_NOT, 2, rd, LONGSZ);
}

static int i_cond(long op)
{
	/* lt, ge, eq, ne, le, gt */
	static int ucond[] = {0x92, 0x93, 0x94, 0x95, 0x96, 0x97};
	static int scond[] = {0x9c, 0x9d, 0x94, 0x95, 0x9e, 0x9f};
	long bt = O_T(op);
	return bt & T_MSIGN ? scond[op & 0x0f] : ucond[op & 0x0f];
}

static void i_set(long op, int rd)
{
	char set[] = "\x0f\x00\xc0";
	set[1] = i_cond(op);
	os(set, 3);			/* setl al */
	os("\x0f\xb6\xc0", 3);		/* movzx rax, al */
}

static void i_lnot(int rd)
{
	char cmp[] = "\x83\xf8\x00";
	cmp[1] |= rd;
	os(cmp, 3);		/* cmp rax, 0 */
	i_set(O_EQ, rd);
}

static void jx(int x, int nbytes)
{
	char op[2] = {0x0f};
	if (nbytes == 1) {
		oi(0x70 | (x & 0x0f), 1);	/* jx $addr */
	} else {
		op[1] = x;
		os(op, 2);			/* jx $addr */
	}
}

static long i_jmp(long op, long rn, long rm, int nbytes)
{
	long ret;
	if (nbytes > 1)
		nbytes = 4;
	if (op & (O_JZ | O_JCC)) {
		if (op & O_JZ) {
			i_tst(rn, rn);
			jx(O_C(op) == O_JZ ? 0x84 : 0x85, nbytes);
		} else {
			if (op & O_NUM)
				i_cmp_imm(rn, rm);
			else
				i_cmp(rn, rm);
			jx(i_cond(op) & ~0x10, nbytes);
		}
	} else {
		os(nbytes == 1 ? "\xeb" : "\xe9", 1);	/* jmp $addr */
	}
	ret = opos();
	oi(0, nbytes);
	return ret;
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

static void i_rel(long sym, long flg, long off)
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

static void i_sym(int rd, int sym, int off)
{
	op_x(I_MOVIR + (rd & 7), 0, rd, LONGSZ);
	i_rel(sym, OUT_CS, opos());
	oi(off, LONGSZ);
}

static void i_saveregs(long sregs, long sregs_pos, int st)
{
	int nsregs = 0;
	int i;
	for (i = 0; i < N_TMPS; i++)
		if ((1 << tmpregs[i]) & sregs)
			op_rm(st ? I_MOV : I_MOVR, tmpregs[i], REG_FP,
				sregs_pos + nsregs++ * ULNG, ULNG);
}

void i_wrap(int argc, long sargs, long spsub, int initfp, long sregs, long sregs_pos)
{
	long body_n;
	void *body;
	long diff;		/* prologue length */
	int i;
	/* removing the last jmp to the epilogue */
	if (jmp_ret + 5 == opos()) {
		mem_cut(&cs, jmp_ret);
		jmp_n--;
	}
	lab_add(0);				/* the return label */
	body_n = mem_len(&cs);
	body = mem_get(&cs);
	/* generating function prologue */
	if (initfp) {
		os("\x55", 1);			/* push rbp */
		os("\x89\xe5", 2);		/* mov rbp, rsp */
	}
	if (spsub) {
		os("\x81\xec", 2);
		spsub = ALIGN(spsub, 8);
		oi(spsub, 4);
	}
	i_saveregs(sregs, sregs_pos, 1);	/* saving registers */
	diff = mem_len(&cs);
	mem_put(&cs, body, body_n);
	free(body);
	/* generating function epilogue */
	i_saveregs(sregs, sregs_pos, 0);	/* restoring saved registers */
	if (initfp)
		os("\xc9", 1);			/* leave */
	os("\xc3", 1);				/* ret */
	/* adjusting code offsets */
	for (i = 0; i < rel_n; i++)
		rel_off[i] += diff;
	for (i = 0; i < jmp_n; i++)
		jmp_off[i] += diff;
	for (i = 0; i < lab_sz; i++)
		lab_loc[i] += diff;
}

void i_code(char **c, long *c_len, long **rsym, long **rflg, long **roff, long *rcnt)
{
	int i;
	for (i = 0; i < jmp_n; i++)	/* filling jmp destinations */
		oi_at(jmp_off[i], lab_loc[jmp_dst[i]] - jmp_off[i] - 4, 4);
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
}

void i_done(void)
{
	free(jmp_off);
	free(jmp_dst);
	free(lab_loc);
}

long i_reg(long op, long *rd, long *r1, long *r2, long *tmp)
{
	long oc = O_C(op);
	long bt = O_T(op);
	*rd = 0;
	*r1 = 0;
	*r2 = 0;
	*tmp = 0;
	if (oc & O_MOV) {
		*rd = R_TMPS;
		if (oc & (O_NUM | O_SYM))
			*r1 = oc & (O_NUM | O_SYM) ? LONGSZ * 8 : R_TMPS;
		else
			*r1 = T_SZ(bt) == 1 ? R_BYTE : R_TMPS;
		return 0;
	}
	if (oc & O_ADD) {
		*r1 = R_TMPS;
		*r2 = oc & O_NUM ? (oc == O_ADD ? 32 : 8) : R_TMPS;
		return 0;
	}
	if (oc & O_SHL) {
		if (oc & O_NUM) {
			*r1 = R_TMPS;
			*r2 = 8;
		} else {
			*r2 = 1 << R_RCX;
			*r1 = R_TMPS & ~*r2;
		}
		return 0;
	}
	if (oc & O_MUL) {
		if (oc & O_NUM)
			return 1;
		*rd = oc == O_MOD ? (1 << R_RDX) : (1 << R_RAX);
		*r1 = (1 << R_RAX);
		*r2 = R_TMPS & ~*rd & ~*r1;
		if (oc == O_DIV)
			*r2 &= ~(1 << R_RDX);
		*tmp = (1 << R_RDX) | (1 << R_RAX);
		return 0;
	}
	if (oc & O_CMP) {
		*rd = 1 << R_RAX;
		*r1 = R_TMPS;
		*r2 = oc & O_NUM ? 8 : R_TMPS;
		return 0;
	}
	if (oc & O_UOP) {
		*r1 = R_TMPS;
		if (oc == O_LNOT)
			*rd = 1 << R_RAX;
		else
			*rd = R_TMPS;
		return 0;
	}
	if (oc == O_MSET) {
		*rd = 1 << R_RDI;
		*r1 = 1 << R_RAX;
		*r2 = 1 << R_RCX;
		*tmp = (1 << R_RDI) | (1 << R_RCX);
		return 0;
	}
	if (oc == O_MCPY) {
		*rd = 1 << R_RDI;
		*r1 = 1 << R_RSI;
		*r2 = 1 << R_RCX;
		*tmp = (1 << R_RDI) | (1 << R_RSI) | (1 << R_RCX);
		return 0;
	}
	if (oc == O_RET) {
		*rd = (1 << REG_RET);
		return 0;
	}
	if (oc & O_CALL) {
		*rd = (1 << REG_RET);
		*r1 = oc & O_SYM ? 0 : R_TMPS;
		*tmp = R_TMPS & ~R_PERM;
		return 0;
	}
	if (oc & (O_LD | O_ST)) {
		*rd = T_SZ(bt) == 1 ? R_BYTE : R_TMPS;
		*r1 = R_TMPS;
		*r2 = oc & O_NUM ? 0 : R_TMPS;
		return 0;
	}
	if (oc & O_JZ) {
		*rd = R_TMPS;
		return 0;
	}
	if (oc & O_JCC) {
		*rd = R_TMPS;
		*r1 = oc & O_NUM ? 8 : R_TMPS;
		return 0;
	}
	if (oc == O_JMP)
		return 0;
	return 1;
}

int i_imm(long lim, long n)
{
	long max = (1 << (lim - 1)) - 1;
	return n <= max && n + 1 >= -max;
}

long i_ins(long op, long r0, long r1, long r2)
{
	long oc = O_C(op);
	long bt = O_T(op);
	if (oc & O_ADD) {
		if (oc & O_NUM) {
			if (r0 == r1 && r2 <= 127 && r2 >= -128)
				i_add_imm(op, r1, r1, r2);
			else
				i_add_anyimm(r0, r1, r2);
		} else {
			i_add(op, r1, r1, r2);
		}
	}
	if (oc & O_SHL) {
		if (oc & O_NUM)
			i_shl_imm(op, r1, r1, r2);
		else
			i_shl(op, r1, r1, r2);
	}
	if (oc & O_MUL) {
		if (oc == O_MUL)
			i_mul(R_RAX, r1, r2);
		if (oc == O_DIV)
			i_div(op, R_RAX, r1, r2);
		if (oc == O_MOD)
			i_div(op, R_RDX, r1, r2);
		return 0;
	}
	if (oc & O_CMP) {
		if (oc & O_NUM)
			i_cmp_imm(r1, r2);
		else
			i_cmp(r1, r2);
		i_set(op, r0);
		return 0;
	}
	if (oc & O_UOP) {	/* uop */
		if (oc == O_NEG)
			i_neg(r1);
		if (oc == O_NOT)
			i_not(r1);
		if (oc == O_LNOT)
			i_lnot(r1);
		return 0;
	}
	if (oc == O_CALL) {
		op_rr(I_CALL, 2, r1, LONGSZ);
		return 0;
	}
	if (oc == (O_CALL | O_SYM)) {
		os("\xe8", 1);		/* call $x */
		i_rel(r1, OUT_CS | OUT_RLREL, opos());
		oi(-4, 4);
		return 0;
	}
	if (oc == (O_MOV | O_SYM)) {
		i_sym(r0, r1, r2);
		return 0;
	}
	if (oc == (O_MOV | O_NUM)) {
		i_num(r0, r1);
		return 0;
	}
	if (oc == O_MSET) {
		os("\xfc\xf3\xaa", 3);		/* cld; rep stosb */
		return 0;
	}
	if (oc == O_MCPY) {
		os("\xfc\xf3\xa4", 3);		/* cld; rep movs */
		return 0;
	}
	if (oc == O_RET) {
		jmp_ret = opos();
		jmp_add(i_jmp(O_JMP, 0, 0, 4), 0);
		return 0;
	}
	if (oc == (O_LD | O_NUM)) {
		op_rm(movrx_op(bt, I_MOVR), r0, r1, r2, movrx_bt(bt));
		return 0;
	}
	if (oc == (O_ST | O_NUM)) {
		op_rm(I_MOV, r0, r1, r2, bt);
		return 0;
	}
	if (oc == O_MOV) {
		i_cast(r0, r1, bt);
		return 0;
	}
	if (oc & O_JXX) {
		jmp_add(i_jmp(op, r0, r1, 4), r2 + 1);
		return 0;
	}
	return 1;
}
