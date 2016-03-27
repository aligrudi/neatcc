/* architecture-dependent header for x86_64 */
#define LONGSZ		8	/* word size */
#define I_ARCH		"__x86_64__"

#define N_REGS		16	/* number of registers */
#define N_TMPS		14	/* number of tmp registers */
#define N_ARGS		6	/* number of arg registers */
#define R_TMPS		0xffcf	/* mask of tmp registers */
#define R_ARGS		0x03c6	/* mask of arg registers */
#define R_PERM		0xf008	/* mask of callee-saved registers */

#define REG_FP		5	/* frame pointer register */
#define REG_SP		4	/* stack pointer register */

#define I_ARG0		(-16)	/* offset of the first argument from FP */
#define I_LOC0		0	/* offset of the first local from FP */

#define X64_ABS_RL	(OUT_RL32)	/* x86_64 memory model */
