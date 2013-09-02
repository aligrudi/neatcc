/* basic types */
#define BT_SZMASK	0x00ff
#define BT_SIGNED	0x0100
#define BT_SZ(bt)	((bt) & BT_SZMASK)

#define O_SIGNED	0x100
/* binary instructions for o_bop() */
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
/* unary instructions for o_uop() */
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
void o_label(int id);
void o_jmp(int id);
void o_jz(int id);
void o_jnz(int id);
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
/* passes */
void o_pass1(void);
void o_pass2(void);

/*
 * neatcc architecture-dependent code-generation interface
 *
 * To make maintaining three different architectures easier and unifying the
 * optimization patch, I've extracted gen.c from x86.c and arm.c.  The i_*()
 * functions are now the low level architecture-specific code generation
 * entry points.  The differences between RISC and CISC architectures,
 * actually the annoying asymmetry in CISC architecture, made this interface
 * a bit more complex than it could have ideally been.  Nevertheless, the
 * benefits of extracting gen.c and the cleaner design, especially with the
 * presence of the optimization patch, is worth the added complexity.
 *
 * I tried to make the interface as small as possible.  I'll describe the
 * key functions and macros here.  Overall, there were many challenges for
 * extracting gen.c including:
 * + Different register sets; caller/callee saved and argument registers
 * + CISC-style instructions that work on limited registers and parameters
 * + Different instruction formats and immediate value limitations
 * + Producing epilog, prolog, and local variable addresses when optimizing
 *
 * Instructions:
 * + i_reg(): The mask of allowed registers for each operand of an instruction.
 *   If md is zero, we assume the destination register should be equal to the
 *   first register, as in CISC architectures.  m2 can be zero which means
 *   the instruction doesn't have three operands.  mt denotes the mask of
 *   registers that may lose their contents after the instruction.
 * + i_load(), i_save(), i_mov(), i_num(), i_sym(): The name is clear.
 * + i_imm(): Specifies if the given immediate can be encoded for the given
 *   instruction.
 * + i_jmp(), i_fill(): Branching instructions.  If rn >= 0, the branch is
 *   a conditional branch: jump only the register rn is zero (or nonzero if
 *   jc is nonzero).  nbytes specifies the number of bytes necessary for
 *   holding the jump distance; useful if the architecture supports short
 *   branching instructions.  i_fill() actually fills the jump at src in
 *   code segment.  It returns the amount of bytes jumped.
 * + i_args(): The offset of the first argument from the frame pointer.
 *   It is probably positive.
 * + i_args(): The offset of the first local from the frame pointer.
 *   It is probably negative
 * + tmpregs: Register that can be used for holding temporaries.
 * + argregs: Register for holding the first N_ARGS arguments.
 *
 * There are a few other macros defined in arch headers.  See x64.h as
 * an example.
 *
 */
#ifdef NEATCC_ARM
#include "arm.h"
#endif
#ifdef NEATCC_X64
#include "x64.h"
#endif
#ifdef NEATCC_X86
#include "x86.h"
#endif

/* intermediate instructions */
#define O_IMM		0x200	/* mask for immediate instructions */
#define O_MSET		0x51	/* memset() */
#define O_MCPY		0x52	/* memcpy() */
#define O_MOV		0x53	/* mov */
#define O_SX		0x54	/* sign extend */
#define O_ZX		0x55	/* zero extend */

void i_load(int rd, int rn, int off, int bt);
void i_save(int rd, int rn, int off, int bt);
void i_mov(int rd, int rn);
void i_reg(int op, int *md, int *m1, int *m2, int *mt);
void i_op(int op, int rd, int r1, int r2);
int i_imm(int op, long imm);
void i_op_imm(int op, int rd, int r1, long n);

void i_num(int rd, long n);
void i_sym(int rd, char *sym, int off);

void i_jmp(int rn, int jc, int nbytes);
long i_fill(long src, long dst, int nbytes);

void i_call(char *sym, int off);
void i_call_reg(int rd);
void i_memset(int r0, int r1, int r2);
void i_memcpy(int r0, int r1, int r2);

int i_args(void);	/* the address of the first arg relative to fp */
int i_sp(void);		/* the address of the first local relative to fp */

void i_prolog(int argc, int varg, int sargs, int sregs, int initfp, int subsp);
void i_epilog(int sp_max);
void i_done(void);

extern int tmpregs[];
extern int argregs[];

/* code segment text */
extern char cs[];		/* code segment */
extern int cslen;		/* code segment length */
extern int pass1;		/* first pass */

void os(void *s, int n);
void oi(long n, int l);
