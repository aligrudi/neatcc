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

void o_num(int n, unsigned bt);
void o_local(long addr, unsigned bt);
void o_assign(unsigned bt);
void o_deref(unsigned bt);
void o_arrayderef(unsigned bt);
void o_addr(void);
void o_symaddr(char *name, unsigned bt);
void o_call(int argc, unsigned *bt, unsigned ret_vs);
void o_add(void);
void o_sub(void);

long o_mklocal(int size);
long o_arg(int i, unsigned bt);
void o_droptmp(int n);
long o_mklabel(void);
void o_jz(long addr);
void o_jmp(long addr);
long o_jmpstub(void);
long o_jzstub(void);
void o_filljmp(long addr);

void out_init(void);
void out_write(int fd);
void out_func_beg(char *name);
void out_func_end(char *buf, int len);
void out_rela(char *name, int off);
