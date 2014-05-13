/* variable length buffer */
struct mem {
	char *s;		/* allocated buffer */
	int sz;			/* buffer size */
	int n;			/* length of data stored in s */
};

void mem_init(struct mem *mem);
void mem_done(struct mem *mem);
void mem_cut(struct mem *mem, int pos);
void *mem_buf(struct mem *mem);
void mem_put(struct mem *mem, void *buf, int len);
void mem_putc(struct mem *mem, int c);
void mem_putz(struct mem *mem, int sz);
void mem_cpy(struct mem *mem, int off, void *buf, int len);
int mem_len(struct mem *mem);
