/* architecture-dependent header for ARM */
#define LONGSZ		4	/* word size */
#define I_ARCH		"__arm__"

#define N_REGS		16	/* number of registers */
#define N_TMPS		10	/* number of tmp registers */
#define N_ARGS		4	/* number of arg registers */
#define R_TMPS		0x03ff	/* mask of tmp registers */
#define R_ARGS		0x000f	/* mask of arg registers */
#define R_PERM		0x0ff0	/* mask of callee-saved registers */

/* special registers */
#define REG_FP		11	/* frame pointer register */
#define REG_SP		13	/* stack pointer register */

/* stack positions */
#define I_ARG0		(-16)	/* offset of the first argument from FP */
#define I_LOC0		0	/* offset of the first local from FP */
