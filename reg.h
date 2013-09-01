void r_func(int nargs, int vargs);	/* reset variables for functions */
int r_alloc(int leaf, int used);	/* register allocation */

int r_regmap(int id);			/* the register allocated to a local */
int r_lregs(void);			/* registers assigned to locals */
int r_sargs(void);			/* arguments to save on the stack */

void r_mk(int sz);			/* create local */
void r_rm(int id);			/* remove local */
void r_read(int id);			/* read local */
void r_write(int id);			/* write to local */
void r_addr(int id);			/* using local address */
void r_label(int l);			/* create label */
void r_jmp(int l);			/* jump to label */
