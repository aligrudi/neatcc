/* architecture-dependent header for x86 */
#define LONGSZ		4	/* word size */
#define I_ARCH		"__i386__"

#define N_REGS		8	/* number of registers */
#define N_TMPS		6	/* number of tmp registers */
#define N_ARGS		0	/* number of arg registers */
#define R_TMPS		0x00cf	/* mask of tmp registers */
#define R_ARGS		0x0000	/* mask of arg registers */
#define R_PERM		0x00c8	/* mask of callee-saved registers */

#define REG_FP		5	/* frame pointer register */
#define REG_SP		4	/* stack pointer register */
