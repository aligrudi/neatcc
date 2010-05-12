#define SECSIZE		(1 << 12)
#define MAXTMP		(1 << 12)

/* basic types */
#define BT_SZMASK		0x00ff
#define BT_SIGNED		0x0100
#define BT_FLOAT		0x0200
#define BT_SZ(bt)		((bt) & BT_SZMASK)

void o_func_beg(char *name);
void o_func_end(void);
void o_ret(unsigned bt);

void o_num(long n, unsigned bt);
void o_local(long addr, unsigned bt);
void o_assign(unsigned bt);
void o_deref(unsigned bt);
void o_load(void);
void o_addr(void);
void o_symaddr(char *name, unsigned bt);
void o_call(int argc, unsigned *bt, unsigned ret_vs);
void o_add(void);
void o_sub(void);
void o_mul(void);
void o_div(void);
void o_mod(void);
void o_shl(void);
void o_shr(void);

void o_lt(void);
void o_gt(void);
void o_le(void);
void o_ge(void);
void o_eq(void);
void o_neq(void);

long o_mklocal(int size);
long o_arg(int i, unsigned bt);
void o_tmpdrop(int n);
void o_tmpswap(void);
void o_tmpcopy(void);
void o_tmpfork(void);
void o_tmpjoin(void);
long o_mklabel(void);
long o_jz(long addr);
long o_jmp(long addr);
void o_filljmp(long addr);

void out_init(void);
void out_write(int fd);
void out_func_beg(char *name);
void out_func_end(char *buf, int len);
void out_rela(char *name, int off, int rel);
