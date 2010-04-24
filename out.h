void out_init(void);
void out_write(int fd);

void o_func_beg(char *name);
void o_func_end(void);
void o_ret(int ret);

void o_num(int n);
void o_local(long addr);
void o_assign(void);
void o_deref(void);
void o_symaddr(char *name);
void o_call(int argc);
void o_add(void);
void o_sub(void);

long o_mklocal(void);
long o_arg(int i);
void o_droptmp(void);
long o_mklabel(void);
void o_jz(long addr);
long o_stubjz(void);
void o_filljz(long addr);
