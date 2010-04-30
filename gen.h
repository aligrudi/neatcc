#define SECSIZE		(1 << 12)

/* basic types */
#define BT_SZMASK		0x00ff
#define BT_SIGNED		0x0100
#define BT_FLOAT		0x0200
#define BT_SZ(bt)		(1 << ((bt) & BT_SZMASK))

void o_func_beg(char *name);
void o_func_end(void);
void o_ret(unsigned bt);

void o_num(int n, unsigned bt);
void o_local(long addr, unsigned bt);
void o_assign(unsigned bt);
void o_deref(unsigned bt);
void o_symaddr(char *name, unsigned bt);
void o_call(int argc, unsigned *bt, unsigned ret_vs);
void o_add(void);
void o_sub(void);

long o_mklocal(unsigned bt);
long o_arg(int i, unsigned bt);
void o_droptmp(void);
long o_mklabel(void);
void o_jz(long addr);
long o_stubjz(void);
void o_filljz(long addr);

void out_init(void);
void out_write(int fd);
void out_func_beg(char *name);
void out_func_end(char *buf, int len);
void out_rela(char *name, int off);
