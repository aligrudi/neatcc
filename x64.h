#define LONGSZ		8	/* word size */
#define I_ARCH		"__x86_64__"

#define N_REGS		16	/* number of registers */
#define N_ARGS		6	/* number of arg registers */
#define N_TMPS		14	/* number of tmp registers */
#define R_TMPS		0xffcf	/* mask of tmp registers */
#define R_ARGS		0x03c6	/* mask of arg registers */
#define R_SAVED		0xf008	/* mask of callee-saved registers */

#define R_CALL		0x0001	/* mask of regs than can hold call dst */
#define R_BYTE		R_TMPS	/* mask of regs that can perform byte-wide instructions */

/* special registers */
#define REG_FP		5	/* frame pointer register */
#define REG_SP		4	/* stack pointer register */
#define REG_RET		0	/* returned value register */
#define REG_FORK	0	/* result of conditional branches */

/* memory model */
#define X64_ABS_RL	(OUT_RL32)
