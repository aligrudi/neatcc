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

static long reg_get(long mask)
{
	int i;
	for (i = 0; i < N_TMPS; i++)
		if ((1 << tmpregs[i]) & mask)
			return tmpregs[i];
	return 0;
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

static long iv_map(long iv, long mask)
{
	return reg_get(mask);
}

/* allocate registers for a 3 operand instruction */
static void ic_map(struct ic *ic, int *r0, int *r1, int *r2)
{
	long m0, m1, m2, mt;
	long all = 0;
	if (i_reg(ic->op, &m0, &m1, &m2, &mt, ULNG))
		die("ncc: instruction %06lx not supported\n", ic->op);
	if (m2) {
		*r2 = iv_map(ic->arg2, m2);
		all |= (1 << *r2);
	}
	if (m1) {
		*r1 = iv_map(ic->arg1, m1 & ~all);
		all |= (1 << *r1);
	}
	if (m0) {
		int wop = ic->op & (O_MBOP | O_MUOP);
		if (wop && m2 && m0 & (1 << *r2))
			*r0 = *r2;
		else if (wop && m1 && m0 & (1 << *r1))
			*r0 = *r1;
		else
			*r0 = iv_map(ic->arg0, m0 & ~all);
	} else {
		*r0 = *r1;
	}
	all |= *r0;
	func_regs |= all;
}

static long iv_addr(long iv)
{
	return loc_pos + iv * ULNG + ULNG;
}

static void iv_load(long iv, int reg)
{
	i_ins(O_LOAD, reg, REG_FP, -iv_addr(iv), ULNG);
}

static void iv_save(long iv, int reg)
{
	i_ins(O_SAVE, reg, REG_FP, -iv_addr(iv), ULNG);
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

static void o_gencode(struct ic *ic, int ic_n)
{
	int *pos;	/* the position of ic instructions in code segment */
	int *pos_jmp;
	int *use;
	int r0, r1, r2;
	int i, j;
	use = calloc(ic_n, sizeof(pos[0]));
	for (i = ic_n - 1; i >= 0; --i) {
		long *w, *r1, *r2, *r3;
		ic_info(ic + i, &w, &r1, &r2, &r3);
		if (!w || ic[i].op & O_FCALL)
			use[i]++;
		if (!use[i])
			continue;
		if (r1)
			use[*r1]++;
		if (r2)
			use[*r2]++;
		if (r3)
			use[*r3]++;
		if (ic[i].op == O_CALL)
			for (j = 0; j < ic[i].arg2; j++)
				use[ic[i].args[j]]++;
	}
	pos = malloc(ic_n * sizeof(pos[0]));
	pos_jmp = malloc(ic_n * sizeof(pos_jmp[0]));
	for (i = 0; i < ic_n; i++) {
		int op = ic[i].op;
		pos[i] = mem_len(&cs);
		if (!use[i])
			continue;
		if (op & O_MBOP) {
			ic_map(ic + i, &r0, &r1, &r2);
			iv_load(ic[i].arg1, r1);
			iv_load(ic[i].arg2, r2);
			i_ins(op, r0, r1, r2, ULNG);
			iv_save(ic[i].arg0, r0);
		}
		if (op & O_MUOP) {
			ic_map(ic + i, &r0, &r1, &r2);
			iv_load(ic[i].arg1, r1);
			i_ins(op, r0, r1, r2, ULNG);
			iv_save(ic[i].arg0, r0);
		}
		if (op == O_NUM) {
			ic_map(ic + i, &r0, &r1, &r2);
			i_ins(op, r0, 0, ic[i].arg2, ULNG);
			iv_save(ic[i].arg0, r0);
		}
		if (op == O_LOC) {
			r0 = iv_map(ic[i].arg0, ~0);
			i_ins(O_ADD | O_FNUM, r0, REG_FP,
				-loc_off[ic[i].arg2], ULNG);
			iv_save(ic[i].arg0, r0);
		}
		if (op == O_SYM) {
			ic_map(ic + i, &r0, &r1, &r2);
			i_ins(O_SYM, r0, 0, ic[i].arg2, ULNG);
			iv_save(ic[i].arg0, r0);
		}
		if (op == O_LOAD) {
			ic_map(ic + i, &r0, &r1, &r2);
			iv_load(ic[i].arg1, r1);
			i_ins(O_LOAD, r0, r1, 0, ic[i].arg2);
			iv_save(ic[i].arg0, r0);
		}
		if (op == O_SAVE) {
			ic_map(ic + i, &r0, &r1, &r2);
			iv_load(ic[i].arg1, r1);
			iv_load(ic[i].arg0, r0);
			i_ins(O_SAVE, r1, r0, 0, ic[i].arg2);
		}
		if (op == O_RET) {
			ic_map(ic + i, &r0, &r1, &r2);
			iv_load(ic[i].arg0, r0);
			i_ins(O_RET, r0, 0, 0, ULNG);
		}
		if (op == O_MOV) {
			ic_map(ic + i, &r0, &r1, &r2);
			iv_load(ic[i].arg1, r0);
			if (T_SZ(ic[i].arg2) != ULNG)
				i_ins(O_MOV, r0, r0, 0, ic[i].arg2);
			iv_save(ic[i].arg0, r0);
		}
		if (op == O_CALL) {
			int argc = ic[i].arg2;
			int aregs = MIN(N_ARGS, argc);
			ic_map(ic + i, &r0, &r1, &r2);
			for (j = argc - 1; j >= aregs; --j) {
				iv_load(ic[i].args[j], r0);
				i_ins(O_SAVE, r0, REG_SP,
					(j - aregs) * ULNG, ULNG);
			}
			for (j = aregs - 1; j >= 0; --j)
				iv_load(ic[i].args[j], argregs[j]);
			iv_load(ic[i].arg1, r1);
			i_ins(O_CALL, r0, r1, 0, ULNG);
			iv_save(ic[i].arg0, 0);
		}
		if (op == O_JMP) {
			pos_jmp[i] = i_ins(O_JMP, 0, 0, 4, ULNG);
		}
		if (op == O_JZ || op == O_JN) {
			ic_map(ic + i, &r0, &r1, &r2);
			iv_load(ic[i].arg0, r0);
			pos_jmp[i] = i_ins(op, r0, 0, 4, ULNG);
		}
		if (op & O_FJCMP) {
			ic_map(ic + i, &r0, &r1, &r2);
			iv_load(ic[i].arg0, r0);
			iv_load(ic[i].arg1, r1);
			pos_jmp[i] = i_ins(op, r0, r1, 4, ULNG);
		}
		if (op == O_MSET) {
			ic_map(ic + i, &r0, &r1, &r2);
			iv_load(ic[i].arg0, r0);
			iv_load(ic[i].arg1, r1);
			iv_load(ic[i].arg2, r2);
			i_ins(O_MSET, r0, r1, r2, 0);
		}
		if (op == O_MCPY) {
			ic_map(ic + i, &r0, &r1, &r2);
			iv_load(ic[i].arg0, r0);
			iv_load(ic[i].arg1, r1);
			iv_load(ic[i].arg2, r2);
			i_ins(O_MCPY, r0, r1, r2, 0);
		}
	}
	for (i = 0; i < ic_n; i++)
		if (ic[i].op & O_FJMP)
			i_fill(pos_jmp[i], pos[ic[i].arg2], 4);
	free(use);
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
	o_gencode(ic, ic_n);
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
