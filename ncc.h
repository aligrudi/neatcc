/* predefined array limits; (p.f. means per function) */
#define SECLEN		(1 << 19)	/* size of CS section */
#define NDATS		1024		/* number of DS data symbols */
#define NSYMS		4096		/* number of elf symbols */
#define NREL		4096		/* number of elf relocations */
#define NGLOBALS	1024		/* number of global variables */
#define NLOCALS		1024		/* number of locals p.f. */
#define NARGS		32		/* number of function/macro arguments */
#define NTMPS		64		/* number of expression temporaries */
#define NNUMS		1024		/* number of integer constants p.f. (arm.c) */
#define NJMPS		4096		/* number of jmp instructions p.f. */
#define NFUNCS		1024		/* number of functions */
#define NENUMS		1024		/* number of enum constants */
#define NTYPEDEFS	1024		/* number of typedefs */
#define NSTRUCTS	512		/* number of structs */
#define NFIELDS		128		/* number of fields in structs */
#define NARRAYS		1024		/* number of arrays */
#define NLABELS		1024		/* number of labels p.f. */
#define NAMELEN		128		/* size of identifiers */
#define NDEFS		1024		/* number of macros */
#define MARGLEN		1024		/* size of macro arguments */
#define MDEFLEN		2048		/* size of macro definitions */
#define NBUFS		32		/* macro expansion stack depth */
#define NLOCS		1024		/* number of header search paths */

#define LEN(a)		(sizeof(a) / sizeof((a)[0]))
