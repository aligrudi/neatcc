#define SECSIZE		(1 << 18)
#define MAXTMP		(1 << 12)
#define LONGSZ		4

/* basic types */
#define BT_SZMASK		0x00ff
#define BT_SIGNED		0x0100
#define BT_FLOAT		0x0200
#define BT_SZ(bt)		((bt) & BT_SZMASK)

#define O_ADD		0x00
#define O_SUB		0x01
#define O_AND		0x02
#define O_OR		0x03
#define O_XOR		0x04
#define O_SHL		0x10
#define O_SHR		0x11
#define O_MUL		0x20
#define O_DIV		0x21
#define O_MOD		0x22
#define O_LT		0x30
#define O_GT		0x31
#define O_LE		0x32
#define O_GE		0x33
#define O_EQ		0x34
#define O_NEQ		0x35
#define O_NEG		0x40
#define O_NOT		0x41
#define O_LNOT		0x42
#define O_INC		0x50
#define O_DEC		0x51

#define O_SET		0x100

void o_bop(int op);
void o_uop(int op);

void o_num(long n, unsigned bt);
void o_local(long addr, unsigned bt);
void o_assign(unsigned bt);
void o_deref(unsigned bt);
void o_load(void);
void o_addr(void);
void o_symaddr(long addr, unsigned bt);
void o_call(int argc, unsigned *bt, unsigned ret_vs);

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
