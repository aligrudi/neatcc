#include <stdio.h>
#include <string.h>
#include "gen.h"
#include "reg.h"

static int l_sz[NLOCALS];	/* size of locals */
static int l_nr[NLOCALS];	/* # of reads */
static int l_nw[NLOCALS];	/* # of writes */
static int l_na[NLOCALS];	/* # of address accesses */
static int l_reg[NLOCALS];	/* register mapped to locals */
static int l_n;			/* # of locals */

static int f_argc;		/* number of arguments */
static int f_varg;		/* function has variable argument list */
static int f_lregs;		/* mask of R_TMPS allocated to locals */
static int f_sargs;		/* mask of R_ARGS to be saved */

void r_func(int nargs, int varg)
{
	int i;
	f_varg = varg;
	f_argc = nargs;
	f_lregs = 0;
	l_n = 0;
	for (i = 0; i < f_argc; i++)
		r_mk(LONGSZ);
	f_sargs = f_varg ? R_ARGS : 0;
	for (i = 0; i < f_argc && i < N_ARGS; i++)
		f_sargs |= 1 << argregs[i];
}

void r_mk(int sz)
{
	if (!f_lregs) {
		l_sz[l_n] = sz;
		l_nr[l_n] = 0;
		l_nw[l_n] = 0;
		l_na[l_n] = 0;
		l_reg[l_n] = -1;
		l_n++;
	}
}

void r_rm(int id)
{
}

void r_read(int id)
{
	l_nr[id]++;
}

void r_write(int id)
{
	l_nw[id]++;
}

void r_addr(int id)
{
	l_na[id]++;
}

void r_label(int l)
{
}

void r_jmp(int l)
{
}

int r_regmap(int id)
{
	return l_reg[id];
}

int r_lregs(void)
{
	return f_lregs;
}

int r_sargs(void)
{
	return f_sargs;
}

/* sort locals for register allocation based on the number of accesses */
static int *sortedlocals(void)
{
	static int ord[NLOCALS];
	int i, j;
	for (i = 0; i < l_n; i++) {
		for (j = i - 1; j >= 0; j--) {
			if (l_nr[i] + l_nw[i] <= l_nw[ord[j]] + l_nr[ord[j]])
				break;
			ord[j + 1] = ord[j];
		}
		ord[j + 1] = i;
	}
	return ord;
}

int r_alloc(int leaf, int used)
{
	int nlocregs = 0;
	int *ord = sortedlocals();
	int idx = 0;
	int i;
	f_lregs = 0;
	f_sargs = f_varg ? R_ARGS : 0;
	/* except unused arguments, save all arguments on the stack */
	for (i = 0; i < f_argc && i < N_ARGS; i++)
		if (f_varg || l_nr[i] + l_nw[i] + l_na[i])
			f_sargs |= 1 << argregs[i];
	/* letting arguments stay in their registers for leaf functions */
	if (!f_varg && leaf) {
		for (i = 0; i < f_argc && i < N_ARGS; i++) {
			if (l_sz[i] > LONGSZ || (1 << argregs[i]) & used)
				continue;
			if (l_nr[i] + l_nw[i] && !l_na[i]) {
				l_reg[i] = argregs[i];
				f_sargs &= ~(1 << argregs[i]);
				f_lregs |= (1 << argregs[i]);
				nlocregs++;
			}
		}
	}
	/* try finding a register for each local */
	for (i = 0; i < l_n; i++) {
		int l = ord[i];
		int nmask = (leaf ? 0 : ~R_SAVED) | used | f_lregs;
		if (l_reg[l] >= 0 || (f_varg && l < f_argc))
			continue;
		/* find a free register */
		while (idx < N_TMPS && ((1 << tmpregs[idx]) & nmask))
			idx++;
		if (idx >= N_TMPS)
			break;
		if (l_sz[l] > LONGSZ || l_na[l])
			continue;
		if (l_nr[l] + l_nw[l] > (leaf ? 0 : 1)) {
			l_reg[l] = tmpregs[idx];
			f_lregs |= 1 << tmpregs[idx];
			if (l < N_ARGS && l < f_argc)
				f_sargs &= ~(1 << argregs[l]);
			nlocregs++;
			idx++;
		}
	}
	return nlocregs;
}
