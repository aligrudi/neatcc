/* neatcc code generation */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ncc.h"

static struct mem ds;		/* data segment */
static struct mem cs;		/* code segment */
static long bsslen;		/* bss segment size */

static long *loc_off;		/* offset of locals on the stack */
static long loc_n, loc_sz;	/* number of locals */
static long loc_pos;		/* current stack position */

static char (*ds_name)[NAMELEN];/* data section symbols */
static long *ds_off;		/* data section offsets */
static long ds_n, ds_sz;	/* number of data section symbols */

static int func_argc;		/* number of arguments */
static int func_varg;		/* varargs */
static int func_regs;		/* used registers */

static long loc_add(long pos)
{
	if (loc_n >= loc_sz) {
		loc_sz = MAX(128, loc_sz * 2);
		loc_off = mextend(loc_off, loc_n, loc_sz, sizeof(loc_off[0]));
	}
	loc_off[loc_n] = pos;
	return loc_n++;
}

long o_mklocal(long sz)
{
	loc_pos += ALIGN(sz, ULNG);
	return loc_add(loc_pos);
}

void o_rmlocal(long addr, long sz)
{
}

long o_arg2loc(int i)
{
	return i;
}

void o_bsnew(char *name, long size, int global)
{
	out_def(name, OUT_BSS | (global ? OUT_GLOB : 0), bsslen, size);
	bsslen += ALIGN(size, OUT_ALIGNMENT);
}

long o_dsnew(char *name, long size, int global)
{
	int idx;
	if (ds_n >= ds_sz) {
		ds_sz = MAX(128, ds_sz * 2);
		ds_name = mextend(ds_name, ds_n, ds_sz, sizeof(ds_name[0]));
		ds_off = mextend(ds_off, ds_n, ds_sz, sizeof(ds_off[0]));
	}
	idx = ds_n++;
	strcpy(ds_name[idx], name);
	ds_off[idx] = mem_len(&ds);
	out_def(name, OUT_DS | (global ? OUT_GLOB : 0), mem_len(&ds), size);
	mem_putz(&ds, ALIGN(size, OUT_ALIGNMENT));
	return ds_off[idx];
}

void o_dscpy(long addr, void *buf, long len)
{
	mem_cpy(&ds, addr, buf, len);
}

static int dat_off(char *name)
{
	int i;
	for (i = 0; i < ds_n; i++)
		if (!strcmp(name, ds_name[i]))
			return ds_off[i];
	return 0;
}

void o_dsset(char *name, long off, long bt)
{
	long sym_off = dat_off(name) + off;
	long num, roff, rsym;
	if (!o_popnum(&num)) {
		mem_cpy(&ds, sym_off, &num, T_SZ(bt));
		return;
	}
	if (!o_popsym(&rsym, &roff)) {
		out_rel(rsym, OUT_DS, sym_off);
		mem_cpy(&ds, sym_off, &roff, T_SZ(bt));
	}
}

/* number of register arguments */
static int ic_regcnt(struct ic *ic)
{
	long o = ic->op;
	if (o & O_BOP)
		return o & (O_NUM | O_SYM | O_LOC) ? 2 : 3;
	if (o & O_UOP)
		return o & (O_NUM | O_SYM | O_LOC) ? 1 : 2;
	if (o & O_CALL)
		return o & (O_NUM | O_SYM | O_LOC) ? 1 : 2;
	if (o & O_MOV)
		return o & (O_NUM | O_SYM | O_LOC) ? 1 : 2;
	if (o & O_MEM)
		return 3;
	if (o & O_JMP)
		return 0;
	if (o & O_JZ)
		return 1;
	if (o & O_JCC)
		return o & (O_NUM | O_SYM | O_LOC) ? 1 : 2;
	if (o & O_RET)
		return 1;
	if (o & (O_LD | O_ST) && o & (O_SYM | O_LOC))
		return 1;
	if (o & (O_LD | O_ST))
		return o & O_NUM ? 2 : 3;
	return 0;
}

static long *iv_use;		/* the last time each value is used */
static long *iv_gmask;		/* the mask of good registers for each value */
static long *iv_bbeg;		/* whether each instruction begins a basic block */
static long *iv_pos;		/* the current position of each value */
static long iv_regmap[N_REGS];	/* the value stored in each register */
static long iv_live[NTMPS];	/* live values */
static int iv_maxlive;		/* the number of values stored on the stack */

/* find a register, with the given good, acceptable, and bad registers */
static long iv_map(long iv, long gmask, long amask, long bmask)
{
	int i;
	gmask &= ~bmask & amask;
	amask &= ~bmask;
	if (iv_pos[iv] >= 0 && (1 << iv_pos[iv]) & (gmask | amask))
		return iv_pos[iv];
	for (i = 0; i < N_TMPS; i++)
		if ((1 << tmpregs[i]) & gmask && iv_regmap[tmpregs[i]] < 0)
			return tmpregs[i];
	for (i = 0; i < N_TMPS; i++)
		if ((1 << tmpregs[i]) & amask && iv_regmap[tmpregs[i]] < 0)
			return tmpregs[i];
	for (i = 0; i < N_TMPS; i++)
		if ((1 << tmpregs[i]) & gmask)
			return tmpregs[i];
	for (i = 0; i < N_TMPS; i++)
		if ((1 << tmpregs[i]) & amask)
			return tmpregs[i];
	die("neatcc: cannot allocate an acceptable register\n");
	return 0;
}

/* allocate registers for a 3 operand instruction */
static void ic_map(struct ic *ic, int *r0, int *r1, int *r2, long *mt)
{
	long m0, m1, m2;
	long all = 0;
	int n = ic_regcnt(ic);
	int oc = O_C(ic->op);
	int i;
	*r0 = 0;
	*r1 = 0;
	*r2 = 0;
	*mt = 0;
	if (oc & O_CALL)
		for (i = 0; i < MIN(ic->arg2, N_ARGS); i++)
			all |= (1 << argregs[i]);
	if (oc & O_LOC) {
		if (oc & O_MOV)
			oc = O_ADD | O_NUM;
		if (oc & (O_ST | O_LD))
			oc = (oc & ~O_LOC) & O_NUM;
	}
	if (i_reg(ic->op, &m0, &m1, &m2, mt))
		die("neatcc: instruction %08lx not supported\n", ic->op);
	if (n >= 3) {
		*r2 = iv_map(ic->arg2, m2, m2, all);
		all |= (1 << *r2);
	}
	if (n >= 2) {
		*r1 = iv_map(ic->arg1, m1, m1, all);
		all |= (1 << *r1);
	}
	if (n >= 1 && m0) {
		int wop = ic->op & O_OUT;
		if (wop && n >= 3 && m0 & (1 << *r2))
			*r0 = *r2;
		else if (wop && n >= 2 && m0 & (1 << *r1))
			*r0 = *r1;
		else
			*r0 = iv_map(ic->arg0, iv_gmask[ic->arg0], m0, all);
	}
	if (n >= 1 && !m0)
		*r0 = *r1;
	if (n)
		all |= *r0;
	func_regs |= all | *mt;
}

static long iv_rank(long iv)
{
	int i;
	for (i = 0; i < LEN(iv_live); i++)
		if (iv_live[i] == iv)
			return i;
	die("neatcc: the specified value is not live\n");
	return 0;
}

static long iv_addr(long rank)
{
	return loc_pos + rank * ULNG + ULNG;
}

/* move the value to the stack */
static void iv_spill(long iv)
{
	if (iv_pos[iv] >= 0) {
		long rank = iv_rank(iv);
		iv_maxlive = MAX(iv_maxlive, rank + 1);
		i_ins(O_MK(O_ST | O_NUM, ULNG), iv_pos[iv], REG_FP, -iv_addr(rank));
		iv_regmap[iv_pos[iv]] = -1;
		iv_pos[iv] = -1;
	}
}

/* set the value to the given register */
static void iv_save(long iv, int reg)
{
	int i;
	iv_regmap[reg] = iv;
	iv_pos[iv] = reg;
	for (i = 0; i < LEN(iv_live); i++)
		if (iv_live[i] < 0)
			break;
	if (i == LEN(iv_live))
		die("neatcc: too many live values\n");
	iv_live[i] = iv;
}

/* load the value into a register */
static void iv_load(long iv, int reg)
{
	if (iv_regmap[reg] == iv)
		return;
	if (iv_regmap[reg] >= 0)
		iv_spill(iv_regmap[reg]);
	if (iv_pos[iv] >= 0) {
		iv_regmap[iv_pos[iv]] = -1;
		i_ins(O_MK(O_MOV, ULNG), reg, iv_pos[iv], 0);
	} else {
		i_ins(O_MK(O_LD | O_NUM, ULNG), reg, REG_FP, -iv_addr(iv_rank(iv)));
	}
	iv_regmap[reg] = iv;
	iv_pos[iv] = reg;
}

/* the value is no longer needed */
static void iv_drop(long iv)
{
	int i;
	for (i = 0; i < LEN(iv_live); i++)
		if (iv_live[i] == iv)
			iv_live[i] = -1;
	if (iv_pos[iv] >= 0) {
		iv_regmap[iv_pos[iv]] = -1;
		iv_pos[iv] = -1;
	}
}

/* return the values written to and read from in the given instruction */
static void ic_info(struct ic *ic, long **w, long **r1, long **r2, long **r3)
{
	long n = ic_regcnt(ic);
	long o = ic->op & O_OUT;
	*r1 = NULL;
	*r2 = NULL;
	*r3 = NULL;
	*w = NULL;
	if (o) {
		*w = &ic->arg0;
		*r1 = n >= 2 ? &ic->arg1 : NULL;
		*r2 = n >= 3 ? &ic->arg2 : NULL;
	} else {
		*r1 = n >= 1 ? &ic->arg0 : NULL;
		*r2 = n >= 2 ? &ic->arg1 : NULL;
		*r3 = n >= 3 ? &ic->arg2 : NULL;
	}
}

static void iv_init(struct ic *ic, int ic_n)
{
	long m0, m1, m2, mt;
	int i, j;
	iv_use = calloc(ic_n, sizeof(iv_use[0]));
	iv_gmask = calloc(ic_n, sizeof(iv_gmask[0]));
	iv_bbeg = calloc(ic_n, sizeof(iv_bbeg[0]));
	iv_pos = malloc(ic_n * sizeof(iv_pos[0]));
	/* iv_use */
	for (i = ic_n - 1; i >= 0; --i) {
		long *w, *r1, *r2, *r3;
		ic_info(ic + i, &w, &r1, &r2, &r3);
		if (!iv_use[i])
			if (!w || ic[i].op & O_CALL)
				iv_use[i] = i;
		if (!iv_use[i])
			continue;
		if (r1 && !iv_use[*r1])
			iv_use[*r1] = i;
		if (r2 && !iv_use[*r2])
			iv_use[*r2] = i;
		if (r3 && !iv_use[*r3])
			iv_use[*r3] = i;
		if (ic[i].op & O_CALL)
			for (j = 0; j < ic[i].arg2; j++)
				if (!iv_use[ic[i].args[j]])
					iv_use[ic[i].args[j]] = i;
	}
	/* iv_gmask */
	for (i = 0; i < ic_n; i++) {
		int n = ic_regcnt(ic + i);
		int op = ic[i].op;
		if (!iv_use[i])
			continue;
		i_reg(op, &m0, &m1, &m2, &mt);
		if (n >= 1 && !(op & O_OUT))
			iv_gmask[ic[i].arg0] = m0;
		if (n >= 2)
			iv_gmask[ic[i].arg1] = m1;
		if (n >= 3)
			iv_gmask[ic[i].arg2] = m2;
		if (op & O_CALL)
			for (j = 0; j < ic[j].arg2; j++)
				iv_gmask[ic[i].args[j]] = 1 << argregs[j];
	}
	/* iv_bbeg */
	for (i = 0; i < ic_n; i++) {
		if (!iv_use[i])
			continue;
		if (i + 1 < ic_n && ic[i].op & (O_JXX | O_CALL | O_RET))
			iv_bbeg[i + 1] = 1;
		if (ic[i].op & O_JXX && ic[i].arg2 < ic_n)
			iv_bbeg[ic[i].arg2] = 1;
	}
	/* iv_pos */
	for (i = 0; i < ic_n; i++)
		iv_pos[i] = -1;
	/* iv_regmap */
	for (i = 0; i < LEN(iv_regmap); i++)
		iv_regmap[i] = -1;
	/* iv_live */
	for (i = 0; i < LEN(iv_live); i++)
		iv_live[i] = -1;
	iv_maxlive = 0;
}

static void iv_done(void)
{
	free(iv_use);
	free(iv_gmask);
	free(iv_bbeg);
	free(iv_pos);
}

static void ic_gencode(struct ic *ic, int ic_n)
{
	int r0, r1, r2;
	long mt;
	int i, j;
	iv_init(ic, ic_n);
	for (i = 0; i < ic_n; i++) {
		long op = ic[i].op;
		long oc = O_C(op);
		int n = ic_regcnt(ic + i);
		i_label(i);
		if (!iv_use[i])
			continue;
		ic_map(ic + i, &r0, &r1, &r2, &mt);
		if (oc & O_CALL) {
			int argc = ic[i].arg2;
			int aregs = MIN(N_ARGS, argc);
			/* arguments passed via stack */
			for (j = argc - 1; j >= aregs; --j) {
				int v = ic[i].args[j];
				iv_load(v, r0);
				i_ins(O_MK(O_ST | O_NUM, ULNG), r0, REG_SP,
					(j - aregs) * ULNG);
				iv_drop(v);
			}
			iv_maxlive += argc - aregs;
			/* arguments passed via registers */
			for (j = aregs - 1; j >= 0; --j)
				iv_load(ic[i].args[j], argregs[j]);
		}
		/* loading the arguments */
		if (n >= 1 && !(oc & O_OUT))
			iv_load(ic[i].arg0, r0);
		if (n >= 2 && !(oc & O_LOC))
			iv_load(ic[i].arg1, r1);
		if (n >= 3)
			iv_load(ic[i].arg2, r2);
		/* saving values stored in registers that may change */
		for (j = 0; j < N_REGS; j++)
			if (iv_regmap[j] >= 0 && mt & (1 << j))
				iv_spill(iv_regmap[j]);
		/* overwriting a value that is needed later */
		if (n >= 1 && oc & O_OUT && iv_regmap[r0] >= 0)
			if (iv_use[iv_regmap[r0]] > i)
				iv_spill(iv_regmap[r0]);
		/* dropping values that are no longer used */
		for (j = 0; j < LEN(iv_live); j++)
			if (iv_live[j] >= 0 && iv_use[iv_live[j]] <= i)
				iv_drop(iv_live[j]);
		/* the last instruction of a basic block */
		if (i + 1 < ic_n && iv_bbeg[i + 1])
			for (j = 0; j < LEN(iv_live); j++)
				if (iv_live[j] >= 0)
					iv_spill(iv_live[j]);
		/* performing the instruction */
		if (oc & O_BOP)
			i_ins(op, r0, r1, oc & O_NUM ? ic[i].arg2 : r2);
		if (oc & O_UOP)
			i_ins(op, r0, r1, r2);
		if (oc == (O_LD | O_NUM))
			i_ins(op, r0, r1, ic[i].arg2);
		if (oc == (O_LD | O_LOC))
			i_ins((op & ~O_LOC) | O_NUM, r0, REG_FP,
				-loc_off[ic[i].arg1] + ic[i].arg2);
		if (oc == (O_ST | O_NUM))
			i_ins(op, r0, r1, ic[i].arg2);
		if (oc == (O_ST | O_LOC))
			i_ins((op & ~O_LOC) | O_NUM , r0, REG_FP,
				-loc_off[ic[i].arg1] + ic[i].arg2);
		if (oc == O_RET)
			i_ins(op, r0, 0, 0);
		if (oc == O_MOV)
			i_ins(op, r0, r0, 0);
		if (oc == (O_MOV | O_NUM))
			i_ins(op, r0, ic[i].arg1, 0);
		if (oc == (O_MOV | O_LOC))
			i_ins(O_ADD | O_NUM, r0, REG_FP,
				-loc_off[ic[i].arg1] + ic[i].arg2);
		if (oc == (O_MOV | O_SYM))
			i_ins(op, r0, ic[i].arg1, ic[i].arg2);
		if (oc == O_CALL)
			i_ins(op, r0, r1, 0);
		if (oc == (O_CALL | O_SYM))
			i_ins(op, r0, ic[i].arg1, 0);
		if (oc == O_JMP)
			i_ins(op, 0, 0, ic[i].arg2);
		if (oc & O_JZ)
			i_ins(op, r0, 0, ic[i].arg2);
		if (oc & O_JCC)
			i_ins(op, r0, oc & O_NUM ? ic[i].arg1 : r1, ic[i].arg2);
		if (oc == O_MSET)
			i_ins(op, r0, r1, r2);
		if (oc == O_MCPY)
			i_ins(op, r0, r1, r2);
		/* saving back the output register */
		if (oc & O_OUT)
			iv_save(ic[i].arg0, r0);
	}
	i_label(ic_n);
	iv_done();
}

static void ic_reset(void)
{
	o_tmpdrop(-1);
	o_back(0);
	free(loc_off);
	loc_off = NULL;
	loc_n = 0;
	loc_sz = 0;
	loc_pos = 0;
}

void o_func_beg(char *name, int argc, int global, int varg)
{
	int i;
	func_argc = argc;
	func_varg = varg;
	ic_reset();
	for (i = 0; i < argc; i++)
		loc_add((-i - 2) * ULNG);
	out_def(name, (global ? OUT_GLOB : 0) | OUT_CS, mem_len(&cs), 0);
}

void o_code(char *name, char *c, long c_len)
{
	out_def(name, OUT_CS, mem_len(&cs), 0);
	mem_put(&cs, c, c_len);
}

void o_func_end(void)
{
	struct ic *ic;
	long ic_n, spsub;
	long sargs = 0;
	char *c;
	long c_len, *rsym, *rflg, *roff, rcnt;
	int i;
	ic_get(&ic, &ic_n);		/* the intermediate code */
	ic_gencode(ic, ic_n);		/* generating machine code */
	/* adding function prologue and epilogue */
	spsub = loc_pos + iv_maxlive * ULNG;
	for (i = 0; i < N_ARGS && (func_varg || i < func_argc); i++)
		sargs |= 1 << argregs[i];
	i_wrap(func_argc, sargs, R_PERM & func_regs, spsub || func_argc, spsub);
	i_code(&c, &c_len, &rsym, &rflg, &roff, &rcnt);
	for (i = 0; i < rcnt; i++)	/* adding the relocations */
		out_rel(rsym[i], rflg[i], roff[i] + mem_len(&cs));
	mem_put(&cs, c, c_len);		/* appending function code */
	free(c);
	free(rsym);
	free(rflg);
	free(roff);
	for (i = 0; i < ic_n; i++)
		if (ic[i].op & O_CALL)
			free(ic[i].args);
	free(ic);
	ic_reset();
}

void o_write(int fd)
{
	i_done();
	out_write(fd, mem_buf(&cs), mem_len(&cs), mem_buf(&ds), mem_len(&ds));
	free(loc_off);
	free(ds_name);
	free(ds_off);
	mem_done(&cs);
	mem_done(&ds);
}
