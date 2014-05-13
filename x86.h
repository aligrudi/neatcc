/* architecture-dependent header for x86 */
#define LONGSZ		4	/* word size */
#define I_ARCH		"__i386__"

#define N_REGS		8	/* number of registers */
#define N_ARGS		0	/* number of arg registers */
#define N_TMPS		6	/* number of tmp registers */
#define R_TMPS		0x00cf	/* mask of tmp registers */
#define R_ARGS		0x0000	/* mask of arg registers */
#define R_SAVED		0x00c8	/* mask of callee-saved registers */

#define R_CALL		0x0001	/* mask of regs than can hold call dst */
#define R_BYTE		0x0007	/* mask of regs that can perform byte-wide instructions */

/* special registers */
#define REG_FP		5	/* frame pointer register */
#define REG_SP		4	/* stack pointer register */
#define REG_RET		0	/* returned value register */
#define REG_FORK	0	/* result of conditional branches */
