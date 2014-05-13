/* architecture-dependent header for ARM */
#define LONGSZ		4	/* word size */
#define I_ARCH		"__arm__"

#define N_REGS		16	/* number of registers */
#define N_ARGS		4	/* number of arg registers */
#define N_TMPS		10	/* number of tmp registers */
#define R_TMPS		0x03ff	/* mask of tmp registers */
#define R_ARGS		0x000f	/* mask of arg registers */
#define R_SAVED		0x0ff0	/* mask of callee-saved registers */

#define R_CALL		R_TMPS	/* mask of regs than can hold call dst */
#define R_BYTE		R_TMPS	/* mask of regs that can perform byte-wide instructions */

/* special registers */
#define REG_FP		11	/* frame pointer register */
#define REG_SP		13	/* stack pointer register */
#define REG_RET		0	/* returned value register */
#define REG_FORK	0	/* result of conditional branches */
