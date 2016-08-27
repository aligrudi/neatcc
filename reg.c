/* neatcc global register allocation */
#include <stdio.h>
#include <stdlib.h>
#include "ncc.h"

#define IC_LLD(ic, i)		(O_C((ic)[i].op) == (O_LD | O_LOC) ? (ic)[i].a1 : -1)
#define IC_LST(ic, i)		(O_C((ic)[i].op) == (O_ST | O_LOC) ? (ic)[i].a2 : -1)

static int ic_loc(struct ic *ic, long iv, long *loc, long *off)
{
	long oc = O_C(ic[iv].op);
	if (oc == (O_LD | O_LOC) || oc == (O_MOV | O_LOC)) {
		*loc = ic[iv].a1;
		*off = ic[iv].a2;
		return 0;
	}
	if (oc == (O_ST | O_LOC)) {
		*loc = ic[iv].a2;
		*off = ic[iv].a3;
		return 0;
	}
	return 1;
}

/* local live region */
struct rgn {
	long loc;	/* local number */
	long beg;	/* region start (instruction number) */
	long end;	/* region end */
	long cnt;	/* number of accesses */
	int reg;	/* register allocated to this region */
};

static struct rgn *rgn;		/* live regions */
static int rgn_n;		/* number of entries in rgn[] */
static int rgn_sz;		/* size of rgn[] */

static int *loc_ptr;		/* if the address of locals is accessed */
static int loc_n;		/* number of locals */

static long *dst_head;		/* lists of jumps to each instruction */
static long *dst_next;		/* next entries in dst_head[] lists */

static void rgn_add(long loc, long beg, long end, long cnt)
{
	int i;
	for (i = 0; i < rgn_n; i++) {
		if (rgn[i].loc == loc && rgn[i].beg < end && rgn[i].end > beg) {
			if (beg > rgn[i].beg)
				beg = rgn[i].beg;
			if (end < rgn[i].end)
				end = rgn[i].end;
			cnt += rgn[i].cnt;
			rgn[i].loc = -1;
		}
	}
	for (i = 0; i < rgn_n; i++)
		if (rgn[i].loc < 0)
			break;
	if (i == rgn_n) {
		if (rgn_n >= rgn_sz) {
			rgn_sz = MAX(16, rgn_sz * 2);
			rgn = mextend(rgn, rgn_n, rgn_sz, sizeof(rgn[0]));
		}
		rgn_n++;
	}
	rgn[i].loc = loc;
	rgn[i].beg = beg;
	rgn[i].end = end;
	rgn[i].cnt = cnt;
	rgn[i].reg = -1;
}

/* return nonzero if register reg is free from beg till end */
static int rgn_available(long beg, long end, int reg)
{
	int i;
	for (i = 0; i < rgn_n; i++)
		if (rgn[i].reg == reg)
			if (rgn[i].beg < end && rgn[i].end > beg)
				return 0;
	return 1;
}

static long reg_region(struct ic *ic, long ic_n, long loc, long pos,
		long *beg, long *end, char *mark)
{
	long cnt = 0;
	long dst;
	for (; pos >= 0; pos--) {
		if (pos < *beg)
			*beg = pos;
		if (pos + 1 > *end)
			*end = pos + 1;
		if (mark[pos])
			break;
		mark[pos] = 1;
		if (IC_LST(ic, pos) == loc)
			break;
		if (IC_LLD(ic, pos) == loc)
			cnt++;
		dst = dst_head[pos];
		while (dst >= 0) {
			cnt += reg_region(ic, ic_n, loc, dst, beg, end, mark);
			dst = dst_next[dst];
		}
		if (pos > 0 && ic[pos - 1].op & O_JMP)
			break;
	}
	return cnt;
}

/* compute local's live regions */
static void reg_regions(struct ic *ic, long ic_n, long loc)
{
	char *mark;
	long beg, end;
	long cnt;
	long i;
	mark = calloc(ic_n, sizeof(mark[0]));
	for (i = 0; i < ic_n; i++) {
		if (IC_LLD(ic, i) == loc && !mark[i]) {
			beg = i;
			end = i + 1;
			cnt = reg_region(ic, ic_n, loc, i, &beg, &end, mark);
			rgn_add(loc, beg, end, cnt);
		}
	}
	for (i = 0; i < ic_n; i++)
		if (IC_LST(ic, i) == loc && !mark[i])
			rgn_add(loc, i, i + 1, 1);
	free(mark);
}

/* perform global register allocation */
static void reg_glob(int leaf)
{
	int *srt;
	int regs[N_REGS];
	int i, j;
	int regs_max = MIN(N_TMPS >> 1, 4);
	long regs_mask = leaf ? R_TMPS : R_PERM;
	int regs_n = 0;
	for (i = leaf ? 1 : 3; i < N_TMPS && regs_n < regs_max; i++)
		if ((1 << i) & regs_mask)
			regs[regs_n++] = i;
	srt = malloc(rgn_n * sizeof(srt[0]));
	/* sorting locals */
	for (i = 0; i < rgn_n; i++) {
		for (j = i - 1; j >= 0 && rgn[i].cnt > rgn[srt[j]].cnt; j--)
			srt[j + 1] = srt[j];
		srt[j + 1] = i;
	}
	/* allocating registers */
	for (i = 0; i < rgn_n; i++) {
		int r = srt[i];
		long loc = rgn[r].loc;
		long beg = rgn[r].beg;
		long end = rgn[r].end;
		if (loc < 0 || loc_ptr[loc])
			continue;
		if (leaf && loc < N_ARGS && beg == 0 &&
				rgn_available(beg, end, argregs[loc])) {
			rgn[r].reg = argregs[loc];
			continue;
		}
		for (j = 0; j < regs_n; j++)
			if (rgn_available(beg, end, regs[j]))
				break;
		if (j < regs_n)
			rgn[r].reg = regs[j];
	}
	free(srt);
}

void reg_init(struct ic *ic, long ic_n)
{
	long loc, off;
	int *loc_sz;
	int leaf = 1;
	long i;
	for (i = 0; i < ic_n; i++)
		if (ic[i].op & O_LOC && !ic_loc(ic, i, &loc, &off))
			if (loc + 1 >= loc_n)
				loc_n = loc + 1;
	loc_ptr = calloc(loc_n, sizeof(loc_ptr[0]));
	loc_sz = calloc(loc_n, sizeof(loc_sz[0]));
	for (i = 0; i < ic_n; i++) {
		long oc = O_C(ic[i].op);
		if (ic_loc(ic, i, &loc, &off))
			continue;
		if (oc == (O_LD | O_LOC) || oc == (O_ST | O_LOC)) {
			int sz = T_SZ(O_T(ic[i].op));
			if (!loc_sz[loc])
				loc_sz[loc] = sz;
			if (off || sz < 2 || sz != loc_sz[loc])
				loc_ptr[loc]++;
		}
		if (oc == (O_MOV | O_LOC))
			loc_ptr[loc]++;
	}
	free(loc_sz);
	for (i = 0; i < ic_n; i++)
		if (ic[i].op & O_CALL)
			leaf = 0;
	dst_head = malloc(ic_n * sizeof(dst_head[0]));
	dst_next = malloc(ic_n * sizeof(dst_next[0]));
	for (i = 0; i < ic_n; i++)
		dst_head[i] = -1;
	for (i = 0; i < ic_n; i++)
		dst_next[i] = -1;
	for (i = 0; i < ic_n; i++) {
		if (ic[i].op & O_JXX) {
			dst_next[i] = dst_head[ic[i].a3];
			dst_head[ic[i].a3] = i;
		}
	}
	for (i = 0; i < loc_n; i++)
		if (!loc_ptr[i])
			reg_regions(ic, ic_n, i);
	reg_glob(leaf);
}

long reg_mask(void)
{
	long ret = 0;
	int i;
	for (i = 0; i < rgn_n; i++)
		if (rgn[i].reg >= 0)
			ret |= 1 << rgn[i].reg;
	return ret;
}

/* return the allocated register of local loc */
int reg_lmap(long c, long loc)
{
	int i;
	for (i = 0; i < rgn_n; i++)
		if (rgn[i].loc == loc)
			if (rgn[i].beg <= c && rgn[i].end > c)
				return rgn[i].reg;
	return -1;
}

/* return the local to which register reg is allocated */
int reg_rmap(long c, long reg)
{
	int i;
	for (i = 0; i < rgn_n; i++)
		if (rgn[i].reg == reg)
			if (rgn[i].beg <= c && rgn[i].end > c)
				return rgn[i].loc;
	return -1;
}

void reg_done(void)
{
	free(dst_head);
	free(dst_next);
	free(loc_ptr);
	free(rgn);
	loc_ptr = NULL;
	rgn = NULL;
	rgn_sz = 0;
	rgn_n = 0;
	loc_n = 0;
}

int reg_safe(long loc)
{
	return loc < loc_n && !loc_ptr[loc];
}
