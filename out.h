#define OUT_CS		0x0001		/* code segment symbol */
#define OUT_DS		0x0002		/* data segment symbol */
#define OUT_BSS		0x0004		/* bss segment symbol */

#define OUT_GLOB	0x0010		/* global symbol */

#define OUT_REL		0x0100		/* relative relocation */
#define OUT_REL24	0x0200		/* 24-bit relative relocation */

#define OUT_ALIGNMENT	16		/* amount of section alignment */

void out_init(int flags);

void out_sym(char *name, int flags, int off, int len);
void out_rel(char *name, int flags, int off);

void out_write(int fd, char *cs, int cslen, char *ds, int dslen);
