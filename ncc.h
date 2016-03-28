/*
 * THE NEATCC C COMPILER
 *
 * This header file is organized as follows:
 *
 * 0. helper functions and data structures
 * 1. ncc.c -> tok.c: the interface for reading tokens
 * 2. ncc.c -> int.c: the interface for generating the intermediate code
 * 3. int.c -> gen.c: the intermediate code
 * 4. gen.c -> x64.c: the interface for generating the final code
 * 5. gen.c -> out.c: the interface for generating object files
 */

/* SECTION ZERO: Helper Functions */
/* predefined array limits; (p.f. means per function) */
#define NSYMS		4096*8		/* number of elf symbols */
#define NRELS		8192*8		/* number of elf relocations */
#define NGLOBALS	2048		/* number of global variables */
#define NLOCALS		1024		/* number of locals p.f. */
#define NARGS		32		/* number of function/macro arguments */
#define NTMPS		64		/* number of expression temporaries */
#define NNUMS		1024		/* number of integer constants p.f. (arm.c) */
#define NJMPS		4096		/* number of jmp instructions p.f. */
#define NFUNCS		1024		/* number of functions */
#define NENUMS		4096		/* number of enum constants */
#define NTYPEDEFS	1024		/* number of typedefs */
#define NSTRUCTS	512		/* number of structs */
#define NFIELDS		128		/* number of fields in structs */
#define NARRAYS		8192*8		/* number of arrays */
#define NLABELS		1024		/* number of labels p.f. */
#define NAMELEN		128		/* size of identifiers */
#define NDEFS		1024		/* number of macros */
#define MARGLEN		1024		/* size of macro arguments */
#define MDEFLEN		2048		/* size of macro definitions */
#define NBUFS		32		/* macro expansion stack depth */
#define NLOCS		1024		/* number of header search paths */

#define LEN(a)		(sizeof(a) / sizeof((a)[0]))
#define ALIGN(x, a)	(((x) + (a) - 1) & ~((a) - 1))
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) < (b) ? (b) : (a))

void *mextend(void *old, long oldsz, long newsz, long memsz);
void die(char *msg, ...);
void err(char *fmt, ...);

/* variable length buffer */
struct mem {
	char *s;		/* allocated buffer */
	long sz;		/* buffer size */
	long n;			/* length of data stored in s */
};

void mem_init(struct mem *mem);
void mem_done(struct mem *mem);
void mem_cut(struct mem *mem, long pos);
void *mem_buf(struct mem *mem);
void mem_put(struct mem *mem, void *buf, long len);
void mem_putc(struct mem *mem, int c);
void mem_putz(struct mem *mem, long sz);
void mem_cpy(struct mem *mem, long off, void *buf, long len);
long mem_len(struct mem *mem);

/* SECTION ONE: Tokenisation */
void tok_init(char *path);
char *tok_see(void);		/* return the current token; a static buffer */
char *tok_get(void);		/* return and consume the current token */
long tok_len(void);		/* the length of the last token */
long tok_num(char *tok, long *n);
long tok_addr(void);
void tok_jump(long addr);

int cpp_init(char *path);
void cpp_addpath(char *s);
void cpp_define(char *name, char *def);
char *cpp_loc(long addr);
int cpp_read(char **buf, long *len);

/* SECTION TWO: Intermediate Code Generation */
/* basic type meaning */
#define T_MSIZE		0x00ff
#define T_MSIGN		0x0100
#define T_SZ(bt)	((bt) & T_MSIZE)
#define T_MK(sign, size)	(((sign) & T_MSIGN) | ((size) & T_MSIZE))

/* number of bytes in basic types */
#define ULNG		(LONGSZ)
#define UINT		(4)
#define USHT		(2)
#define UCHR		(1)
/* basic types */
#define SLNG		(ULNG | T_MSIGN)
#define SINT		(UINT | T_MSIGN)
#define SSHT		(USHT | T_MSIGN)
#define SCHR		(UCHR | T_MSIGN)

/* instruction flags */
#define O_FADD		0x000010	/* addition group */
#define O_FSHT		0x000020	/* shift group */
#define O_FMUL		0x000040	/* multiplication group */
#define O_FCMP		0x000080	/* comparison group */
#define O_FUOP		0x000100	/* jump group */
#define O_FCALL		0x000200	/* call group */
#define O_FRET		0x000400	/* return group */
#define O_FJMP		0x000800	/* jump group */
#define O_FIO		0x001000	/* load/store */
#define O_FMEM		0x002000	/* memory operation */
#define O_FMOV		0x004000	/* mov or cast */
#define O_FSIGN		0x008000	/* for both signed and unsigned */
#define O_FSYM		0x010000	/* symbol address */
#define O_FLOC		0x020000	/* local address */
#define O_FNUM		0x040000	/* number */
#define O_FVAL		0x080000	/* load a value */
#define O_FJIF		0x100000	/* conditional jump */
#define O_FJX		0x200000	/* jump if zero or nonzero */
#define O_FJCMP		0x400000	/* conditional jump with comparison */
/* instruction masks */
#define O_MBOP		(O_FADD | O_FMUL | O_FCMP | O_FSHT)
#define O_MUOP		(O_FUOP)
#define O_MOUT		(O_MBOP | O_MUOP | O_FCALL | O_FMOV | O_FVAL)

/* binary instructions for o_bop() */
#define O_ADD		(0 | O_FADD | O_FSIGN)
#define O_SUB		(1 | O_FADD | O_FSIGN)
#define O_AND		(2 | O_FADD | O_FSIGN)
#define O_OR		(3 | O_FADD | O_FSIGN)
#define O_XOR		(4 | O_FADD | O_FSIGN)
#define O_SHL		(0 | O_FSHT | O_FSIGN)
#define O_SHR		(1 | O_FSHT)
#define O_MUL		(0 | O_FMUL)
#define O_DIV		(1 | O_FMUL)
#define O_MOD		(2 | O_FMUL)
#define O_LT		(0 | O_FCMP)
#define O_GE		(1 | O_FCMP)
#define O_EQ		(2 | O_FCMP | O_FSIGN)
#define O_NE		(3 | O_FCMP | O_FSIGN)
#define O_LE		(4 | O_FCMP)
#define O_GT		(5 | O_FCMP)
/* unary instructions for o_uop() */
#define O_NEG		(0 | O_FUOP | O_FSIGN)
#define O_NOT		(1 | O_FUOP | O_FSIGN)
#define O_LNOT		(2 | O_FUOP | O_FSIGN)
/* other instructions */
#define O_CALL		(0 | O_FCALL | O_FSIGN)
#define O_JMP		(0 | O_FJMP | O_FSIGN)
#define O_JZ		(1 | O_FJMP | O_FJIF | O_FJX | O_FSIGN)
#define O_JN		(2 | O_FJMP | O_FJIF | O_FJX | O_FSIGN)
#define O_RET		(0 | O_FRET | O_FSIGN)
#define O_MSET		(0 | O_FMEM | O_FSIGN)
#define O_MCPY		(1 | O_FMEM | O_FSIGN)
#define O_LOAD		(0 | O_FIO | O_FVAL | O_FSIGN)
#define O_SAVE		(1 | O_FIO | O_FSIGN)
#define O_MOV		(0 | O_FMOV | O_FSIGN)
/* loading values */
#define O_NUM		(0 | O_FNUM | O_FVAL | O_FSIGN)
#define O_LOC		(0 | O_FLOC | O_FVAL | O_FSIGN)
#define O_SYM		(0 | O_FSYM | O_FVAL | O_FSIGN)

/* operations on the stack */
void o_bop(long op);		/* binary operation */
void o_uop(long op);		/* unary operation */
void o_cast(long bt);
void o_memcpy(void);
void o_memset(void);
void o_call(int argc, int ret);
void o_ret(int ret);
void o_assign(long bt);
void o_deref(long bt);
void o_load(void);
int o_popnum(long *num);
int o_popsym(long *sym, long *off);
/* pushing values to the stack */
void o_num(long n);
void o_local(long addr);
void o_sym(char *sym);
void o_tmpdrop(int n);
void o_tmpswap(void);
void o_tmpcopy(void);
/* handling locals */
long o_mklocal(long size);
void o_rmlocal(long addr, long sz);
long o_arg2loc(int i);
/* branches */
void o_label(long id);
void o_jmp(long id);
void o_jz(long id);
long o_mark(void);
void o_back(long mark);
/* data/bss sections */
long o_dsnew(char *name, long size, int global);
void o_dscpy(long addr, void *buf, long len);
void o_dsset(char *name, long off, long bt);
void o_bsnew(char *name, long size, int global);
/* functions */
void o_func_beg(char *name, int argc, int global, int vararg);
void o_func_end(void);
/* output */
void o_write(int fd);

/* SECTION THREE: The Intermediate Code */
/* intermediate code instructions */
struct ic {
	long op;		/* instruction opcode */
	long arg0;		/* first argument; usually destination */
	long arg1;		/* second argument */
	long arg2;		/* more information, like jump target */
	long *args;		/* call arguments */
};

/* get the generated intermediate code */
void ic_get(struct ic **c, long *n);
int ic_num(struct ic *ic, long iv, long *num);
int ic_sym(struct ic *ic, long iv, long *sym, long *off);

/* SECTION FOUR: Final Code Generation */
/*
 * neatcc architecture-dependent code-generation interface
 *
 * To make maintaining different architectures easier and to unify the
 * optimizations, code generation for different architectures
 * have been merged.  The i_*() functions are now the low level
 * architecture-specific code generation entry points.  The
 * differences between RISC and CISC architectures, actually the
 * annoying asymmetry in CISC architecture, made this interface a
 * bit more complex than it could have ideally been.  Nevertheless,
 * the benefits of extracting gen.c and the cleaner design, especially
 * with the presence of the optimizations, outweighs the added complexity.
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
 * + i_sp(): The offset of the first local from the frame pointer.
 *   It is probably negative.
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

/* architecture-specific operations */
long i_reg(long op, long *r0, long *r1, long *r2, long *mt, long bt);
long i_ins(long op, long r0, long r1, long r2, long bt);

void i_fill(long src, long dst, long nbytes);
void i_prolog(int argc, int varg, int sargs, int sregs, int initfp, int subsp);
void i_epilog(void);
void i_done(void);

extern int tmpregs[];
extern int argregs[];

/* code generation functions */
void os(void *s, int n);
void oi(long n, int l);
void oi_at(long pos, long n, int l);
long opos(void);

/* SECTION FIVE: Object File Generation */
#define OUT_CS		0x0001		/* code segment symbol */
#define OUT_DS		0x0002		/* data segment symbol */
#define OUT_BSS		0x0004		/* bss segment symbol */

#define OUT_GLOB	0x0010		/* global symbol */

#define OUT_RLREL	0x0020		/* relative relocation */
#define OUT_RLSX	0x0040		/* sign extend relocation */
#define OUT_RL24	0x0400		/* 3-byte relocation */
#define OUT_RL32	0x0800		/* 4-byte relocation */

#define OUT_ALIGNMENT	16		/* section alignment */

void out_init(long flags);

long out_sym(char *name);
void out_def(char *name, long flags, long off, long len);
void out_rel(long id, long flags, long off);

void out_write(int fd, char *cs, long cslen, char *ds, long dslen);
