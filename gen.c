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
	if (o & O_MBOP)
		return o & O_FNUM ? 2 : 3;
	if (o & O_MUOP)
		return o & O_FNUM ? 1 : 2;
	if (o & O_FCALL)
		return o & O_FSYM ? 1 : 2;
	if (o & O_FIO)
		return o & (O_FSYM | O_FLOC) ? 1 : 2;
	if (o & (O_FNUM | O_FLOC | O_FSYM | O_FJX | O_FRET))
		return 1;
	if (o & (O_FJCMP | O_FMOV))
		return 2;
	if (o & O_FMEM)
		return 3;
	return 0;
}

static long *iv_use;		/* the last time each value is used */
static long *iv_gmask;		/* the mask of good registers for each value */
static long *iv_bbeg;		/* whether each instruction begins a basic block */
static long *iv_pos;		/* the current position of each value */
static long iv_regmap[N_REGS];	/* the value stored in each register */
static long iv_live[NTMPS];	/* live values */

/* find a register, with the given good, acceptable, and bad registers */
static long iv_map(long iv, long gmask, long amask, long bmask)
{
	int i;
	gmask &= ~bmask;
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
	int op = ic->op;
	int i;
	if (op == O_CALL)
		for (i = 0; i < MIN(ic->arg2, N_ARGS); i++)
			all |= (1 << argregs[i]);
	if (op == O_LOC) {
		m0 = ~0;
		m1 = 0;
		m2 = 0;
		*mt = 0;
	} else if (i_reg(op, &m0, &m1, &m2, mt, ULNG)) {
		die("neatcc: instruction %06lx not supported\n", op);
	}
	if (m2) {
		*r2 = iv_map(ic->arg2, m2, m2, all);
		all |= (1 << *r2);
	}
	if (m1) {
		*r1 = iv_map(ic->arg1, m1, m1, all);
		all |= (1 << *r1);
	}
	if (m0) {
		int wop = op & O_MOUT;
		if (wop && m2 && m0 & (1 << *r2))
			*r0 = *r2;
		else if (wop && m1 && m0 & (1 << *r1))
			*r0 = *r1;
		else
			*r0 = iv_map(ic->arg0, iv_gmask[ic->arg0], m0, all);
	} else {
		*r0 = *r1;
	}
	if (m0 | m1 | m2)
		all |= *r0;
	func_regs |= all | *mt;
}

static long iv_addr(long iv)
{
	int i;
	for (i = 0; i < LEN(iv_live); i++)
		if (iv_live[i] == iv)
			return loc_pos + i * ULNG + ULNG;
	die("neatcc: the specified value is not live\n");
	return 0;
}

/* move the value to the stack */
static void iv_spill(long iv)
{
	if (iv_pos[iv] >= 0) {
		i_ins(O_SAVE, iv_pos[iv], REG_FP, -iv_addr(iv), ULNG);
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
		i_ins(O_MOV, reg, iv_pos[iv], 0, ULNG);
	} else {
		i_ins(O_LOAD, reg, REG_FP, -iv_addr(iv), ULNG);
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
	long o = ic->op & O_MOUT;
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
			if (!w || ic[i].op & O_FCALL)
				iv_use[i] = i;
		if (!iv_use[i])
			continue;
		if (r1 && !iv_use[*r1])
			iv_use[*r1] = i;
		if (r2 && !iv_use[*r2])
			iv_use[*r2] = i;
		if (r3 && !iv_use[*r3])
			iv_use[*r3] = i;
		if (ic[i].op == O_CALL)
			for (j = 0; j < ic[i].arg2; j++)
				if (!iv_use[ic[i].args[j]])
					iv_use[ic[i].args[j]] = i;
	}
	/* iv_gmask */
	for (i = 0; i < ic_n; i++) {
		int n = ic_regcnt(ic + i);
		int op = ic->op;
		if (!iv_use[i])
			continue;
		i_reg(op, &m0, &m1, &m2, &mt, ULNG);
		if (n >= 1 && !(op & O_MOUT))
			iv_gmask[ic[i].arg0] = m0;
		if (n >= 2)
			iv_gmask[ic[i].arg1] = m1;
		if (n >= 3)
			iv_gmask[ic[i].arg2] = m2;
		if (op & O_FCALL)
			for (j = 0; j < ic[j].arg2; j++)
				iv_gmask[ic[i].args[j]] = 1 << argregs[j];
	}
	/* iv_bbeg */
	for (i = 0; i < ic_n; i++) {
		if (!iv_use[i])
			continue;
		if (i + 1 < ic_n && ic[i].op & (O_FJMP | O_FCALL | O_FRET))
			iv_bbeg[i + 1] = 1;
		if (ic[i].op & O_FJMP)
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
}

static void iv_done(void)
{
	free(iv_use);
	free(iv_gmask);
	free(iv_bbeg);
	free(iv_pos);
}

void os(void *s, int n)
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

void oi(long n, int l)
{
	mem_put(&cs, ointbuf(n, l), l);
}

void oi_at(long pos, long n, int l)
{
	mem_cpy(&cs, pos, ointbuf(n, l), l);
}

long opos(void)
{
	return mem_len(&cs);
}

static void ic_gencode(struct ic *ic, int ic_n)
{
	int *pos;	/* the position of ic instructions in code segment */
	int *pos_jmp;	/* the position of jump instruction offsets */
	int r0, r1, r2;
	long mt;
	int i, j;
	iv_init(ic, ic_n);
	pos = malloc(ic_n * sizeof(pos[0]));
	pos_jmp = malloc(ic_n * sizeof(pos_jmp[0]));
	for (i = 0; i < ic_n; i++) {
		int op = ic[i].op;
		int n = ic_regcnt(ic + i);
		pos[i] = mem_len(&cs);
		if (!iv_use[i])
			continue;
		ic_map(ic + i, &r0, &r1, &r2, &mt);
		if (op & O_FCALL) {
			int argc = ic[i].arg2;
			int aregs = MIN(N_ARGS, argc);
			/* arguments passed via stack */
			for (j = argc - 1; j >= aregs; --j) {
				int v = ic[i].args[j];
				iv_load(v, r0);
				i_ins(O_SAVE, r0, REG_SP,
					(j - aregs) * ULNG, ULNG);
				iv_drop(v);
			}
			/* arguments passed via registers */
			for (j = aregs - 1; j >= 0; --j)
				iv_load(ic[i].args[j], argregs[j]);
		}
		/* loading the arguments */
		if (n >= 1 && !(op & O_MOUT))
			iv_load(ic[i].arg0, r0);
		if (n >= 2)
			iv_load(ic[i].arg1, r1);
		if (n >= 3)
			iv_load(ic[i].arg2, r2);
		/* saving values stored in registers that may change */
		for (j = 0; j < N_REGS; j++)
			if (iv_regmap[j] >= 0 && mt & (1 << j))
				iv_spill(iv_regmap[j]);
		/* overwriting a value that is needed later */
		if (n >= 1 && op & O_MOUT && iv_regmap[r0] >= 0)
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
		if (op & O_MBOP)
			i_ins(op, r0, r1, r2, ULNG);
		if (op & O_MUOP)
			i_ins(op, r0, r1, r2, ULNG);
		if (op == O_NUM)
			i_ins(op, r0, 0, ic[i].arg2, ULNG);
		if (op == O_LOC)
			i_ins(O_ADD | O_FNUM, r0, REG_FP, -loc_off[ic[i].arg2], ULNG);
		if (op == O_SYM)
			i_ins(O_SYM, r0, 0, ic[i].arg2, ULNG);
		if (op == O_LOAD)
			i_ins(O_LOAD, r0, r1, 0, ic[i].arg2);
		if (op == O_SAVE)
			i_ins(O_SAVE, r1, r0, 0, ic[i].arg2);
		if (op == O_RET)
			i_ins(O_RET, r0, 0, 0, ULNG);
		if (op == O_MOV)
			i_ins(O_MOV, r0, r0, 0, ic[i].arg2);
		if (op == O_CALL)
			i_ins(O_CALL, r0, r1, 0, ULNG);
		if (op == O_JMP)
			pos_jmp[i] = i_ins(O_JMP, 0, 0, 4, ULNG);
		if (op == O_JZ || op == O_JN)
			pos_jmp[i] = i_ins(op, r0, 0, 4, ULNG);
		if (op & O_FJCMP)
			pos_jmp[i] = i_ins(op, r0, r1, 4, ULNG);
		if (op == O_MSET)
			i_ins(O_MSET, r0, r1, r2, 0);
		if (op == O_MCPY)
			i_ins(O_MCPY, r0, r1, r2, 0);
		/* saving back the output register */
		if (op & O_MOUT)
			iv_save(ic[i].arg0, r0);
	}
	for (i = 0; i < ic_n; i++)
		if (ic[i].op & O_FJMP)
			i_fill(pos_jmp[i], pos[ic[i].arg2], 4);
	iv_done();
	free(pos);
	free(pos_jmp);
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
	out_def(name, (global ? OUT_GLOB : 0) | OUT_CS, opos(), 0);
}

static long ic_maxtmp(struct ic *ic, long ic_n)
{
	long *w, *r1, *r2, *r3;
	long max = -1;
	int i;
	for (i = 0; i < ic_n; i++) {
		ic_info(ic + i, &w, &r1, &r2, &r3);
		if (w && *w > max)
			max = *w;
		if (r1 && *r1 > max)
			max = *r1;
		if (r2 && *r2 > max)
			max = *r2;
		if (r3 && *r3 > max)
			max = *r3;
	}
	return max;
}

void o_func_end(void)
{
	struct ic *ic;
	long ic_n, spsub;
	long sargs = 0;
	long sregs = R_PERM & func_regs;
	int i;
	for (i = 0; i < MIN(N_ARGS, func_argc); i++)
		sargs |= 1 << argregs[i];
	ic_get(&ic, &ic_n);
	spsub = loc_pos + ic_maxtmp(ic, ic_n) * ULNG;
	i_prolog(func_argc, func_varg, sargs, sregs, 1, spsub);
	ic_gencode(ic, ic_n);
	i_epilog();
	for (i = 0; i < ic_n; i++)
		if (ic[i].op == O_CALL)
			free(ic[i].args);
	free(ic);
	ic_reset();
}

void o_write(int fd)
{
	i_done();
	out_write(fd, mem_buf(&cs), mem_len(&cs), mem_buf(&ds), mem_len(&ds));
}
