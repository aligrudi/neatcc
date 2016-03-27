#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ncc.h"

#define MEMSZ		512

static void mem_extend(struct mem *mem)
{
	char *s = mem->s;
	mem->sz = mem->sz ? mem->sz + mem->sz : MEMSZ;
	mem->s = malloc(mem->sz);
	if (mem->n)
		memcpy(mem->s, s, mem->n);
	free(s);
}

void mem_init(struct mem *mem)
{
	memset(mem, 0, sizeof(*mem));
}

void mem_done(struct mem *mem)
{
	free(mem->s);
	memset(mem, 0, sizeof(*mem));
}

void mem_cut(struct mem *mem, long pos)
{
	mem->n = pos < mem->n ? pos : mem->n;
}

void mem_cpy(struct mem *mem, long off, void *buf, long len)
{
	while (mem->n + off + len + 1 >= mem->sz)
		mem_extend(mem);
	memcpy(mem->s + off, buf, len);
}

void mem_put(struct mem *mem, void *buf, long len)
{
	mem_cpy(mem, mem->n, buf, len);
	mem->n += len;
}

void mem_putc(struct mem *mem, int c)
{
	if (mem->n + 2 >= mem->sz)
		mem_extend(mem);
	mem->s[mem->n++] = c;
}

void mem_putz(struct mem *mem, long sz)
{
	while (mem->n + sz + 1 >= mem->sz)
		mem_extend(mem);
	memset(mem->s + mem->n, 0, sz);
	mem->n += sz;
}

/* return a pointer to mem's buffer; valid as long as mem is not modified */
void *mem_buf(struct mem *mem)
{
	if (!mem->s)
		return "";
	mem->s[mem->n] = '\0';
	return mem->s;
}

long mem_len(struct mem *mem)
{
	return mem->n;
}

void *mem_get(struct mem *mem)
{
	void *ret;
	if (!mem->s)
		mem_extend(mem);
	ret = mem->s;
	mem_init(mem);
	return ret;
}
