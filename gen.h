#define SECSIZE		(1 << 18)
#define MAXTMP		(1 << 12)

/* basic types */
#define BT_SZMASK		0x00ff
#define BT_SIGNED		0x0100
#define BT_FLOAT		0x0200
#define BT_SZ(bt)		((bt) & BT_SZMASK)

void o_num(long n, unsigned bt);
void o_local(long addr, unsigned bt);
void o_assign(unsigned bt);
void o_deref(unsigned bt);
void o_load(void);
void o_addr(void);
void o_symaddr(long addr, unsigned bt);
void o_call(int argc, unsigned *bt, unsigned ret_vs);
void o_add(void);
void o_sub(void);
void o_mul(void);
void o_div(void);
void o_mod(void);
void o_shl(void);
void o_shr(void);
void o_neg(void);
void o_not(void);
void o_or(void);
void o_xor(void);
void o_and(void);
void o_inc(void);
void o_dec(void);

void o_lt(void);
void o_gt(void);
void o_le(void);
void o_ge(void);
void o_eq(void);
void o_neq(void);
void o_lnot(void);

long o_mklocal(int size);
long o_arg(int i, unsigned bt);
void o_rmlocal(long addr, int sz);
void o_tmpdrop(int n);
void o_tmpswap(void);
void o_tmpcopy(void);
void o_fork(void);
void o_forkpush(void);
void o_forkjoin(void);
void o_cast(unsigned bt);
int o_popnum(long *c);
long o_mklabel(void);
long o_jz(long addr);
long o_jnz(long addr);
long o_jmp(long addr);
void o_filljmp(long addr);
void o_filljmp2(long addr, long jmpdst);
void o_memcpy(int sz);
void o_memset(int x, int sz);
void o_datset(long addr, int off, unsigned bt);

long o_func_beg(char *name, int global);
void o_func_end(void);
void o_ret(unsigned bt);

int o_nogen(void);
void o_dogen(void);

void out_init(void);
void out_write(int fd);
long out_func_beg(char *name, int global);
void out_func_end(char *buf, int len);
long out_mkvar(char *name, int size, int global);
long out_mkdat(char *name, char *buf, int len, int global);
long out_mkundef(char *name, int sz);
void out_rela(long addr, int off, int rel);
void out_datcpy(long addr, int off, char *buf, int len);
void out_datrela(long addr, long dataddr, int off);
