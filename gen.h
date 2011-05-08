#define SECSIZE		(1 << 18)
#define MAXTMP		(1 << 12)
#define LONGSZ		4

/* basic types */
#define BT_SZMASK	0x00ff
#define BT_SIGNED	0x0100
#define BT_SZ(bt)	((bt) & BT_SZMASK)

#define O_SIGNED	0x100
/* binary operations for o_bop() */
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
/* unary operations for o_uop() */
#define O_NEG		0x40
#define O_NOT		0x41
#define O_LNOT		0x42

/* operations */
void o_bop(int op);
void o_uop(int op);
void o_cast(unsigned bt);
void o_memcpy(void);
void o_memset(void);
void o_call(int argc, int ret);
void o_ret(int ret);
void o_assign(unsigned bt);
void o_deref(unsigned bt);
void o_load(void);
int o_popnum(long *c);
/* pushing values */
void o_num(long n);
void o_local(long addr);
void o_sym(char *sym);
void o_tmpdrop(int n);
void o_tmpswap(void);
void o_tmpcopy(void);
/* handling locals */
long o_mklocal(int size);
void o_rmlocal(long addr, int sz);
long o_arg2loc(int i);
/* branches */
long o_mklabel(void);
long o_jz(long addr);
long o_jnz(long addr);
long o_jmp(long addr);
void o_filljmp(long addr);
void o_filljmp2(long addr, long jmpdst);
/* conditional instructions */
void o_fork(void);
void o_forkpush(void);
void o_forkjoin(void);
/* data/bss sections */
void o_mkbss(char *name, int size, int global);
void *o_mkdat(char *name, int size, int global);
void o_datset(char *name, int off, unsigned bt);
/* functions */
void o_func_beg(char *name, int argc, int global, int vararg);
void o_func_end(void);
/* output */
void o_write(int fd);
