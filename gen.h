#define SECSIZE		(1 << 12)

#define VS_SIZEMASK		0x0fffffffu
#define VS_SIGNED		0x10000000u
#define VS_FLOAT		0x20000000u

void o_func_beg(char *name);
void o_func_end(void);
void o_ret(unsigned vs);

void o_num(int n, unsigned vs);
void o_local(long addr, unsigned vs);
void o_assign(unsigned vs);
void o_deref(unsigned vs);
void o_symaddr(char *name, unsigned vs);
void o_call(int argc, unsigned *vs, unsigned ret_vs);
void o_add(void);
void o_sub(void);

long o_mklocal(unsigned vs);
long o_arg(int i, unsigned vs);
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
