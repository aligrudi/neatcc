#include <unistd.h>
#include <elf.h>
#include "tok.h"

#define MAXSECS		(1 << 7)
#define MAXSYMS		(1 << 10)

#define SECSIZE		(1 << 12)
#define MAXRELA		(1 << 10)

#define SEC_SYMS		1
#define SEC_SYMSTR		2
#define SEC_BEG			3

static Elf64_Ehdr ehdr;
static Elf64_Shdr shdr[MAXSECS];
static int nshdr = SEC_BEG;
static Elf64_Sym syms[MAXSYMS];
static int nsyms;
static char symstr[MAXSYMS * 8];
static int nsymstr = 1;

static struct sec {
	char buf[SECSIZE];
	int len;
	Elf64_Sym *sym;
	Elf64_Rela rela[MAXRELA];
	int nrela;
	Elf64_Shdr *sec_shdr;
	Elf64_Shdr *rel_shdr;
} secs[MAXSECS];
static int nsecs;
static struct sec *sec;

#define MAXTEMP		(1 << 12)
#define TMP_CONST	1
#define TMP_ADDR	2

static char *cur;
static long sp;
static long spsub_addr;
static long maxsp;
static struct tmp {
	long addr;
	int type;
} tmp[MAXTEMP];
static int ntmp;

static void putint(char *s, long n, int l)
{
	while (l--) {
		*s++ = n;
		n >>= 8;
	}
}

static char *putstr(char *s, char *r)
{
	while (*r)
		*s++ = *r++;
	*s++ = '\0';
	return s;
}

static Elf64_Sym *put_sym(char *name)
{
	Elf64_Sym *sym = &syms[nsyms++];
	sym->st_name = nsymstr;
	nsymstr = putstr(symstr + nsymstr, name) - symstr;
	sym->st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
	return sym;
}

static void os(char *s, int n)
{
	while (n--)
		*cur++ = *s++;
}

static void oi(long n, int l)
{
	while (l--) {
		*cur++ = n;
		n >>= 8;
	}
}

static long sp_push(int size)
{
	long osp = sp;
	sp += size;
	if (sp > maxsp)
		maxsp = sp;
	return osp;
}

static int tmp_pop(void)
{
	os("\x48\x8b\x85", 3);	/* mov top(%rbp), %rax */
	oi(-tmp[--ntmp].addr, 4);
	sp = tmp[ntmp].addr;
	return tmp[ntmp].type;
}

static void tmp_push(int type)
{
	tmp[ntmp].addr = sp_push(8);
	tmp[ntmp].type = type;
	os("\x48\x89\x85", 3);	/* mov %rax, top(%rbp) */
	oi(-tmp[ntmp++].addr, 4);
}

void o_droptmp(void)
{
	if (ntmp)
		sp = tmp[0].addr;
	ntmp = 0;
}

static long codeaddr(void)
{
	return cur - sec->buf;
}

void o_func_beg(char *name)
{
	sec = &secs[nsecs++];
	sec->sym = put_sym(name);
	sec->sym->st_shndx = nshdr;
	sec->sec_shdr = &shdr[nshdr++];
	sec->rel_shdr = &shdr[nshdr++];
	sec->rel_shdr->sh_link = SEC_SYMS;
	sec->rel_shdr->sh_info = sec->sec_shdr - shdr;
	cur = sec->buf;
	os("\x55", 1);			/* push %rbp */
	os("\x48\x89\xe5", 3);		/* mov %rsp, %rbp */
	sp = 8;
	maxsp = 0;
	ntmp = 0;
	os("\x48\x81\xec", 3);		/* sub $xxx, %rsp */
	spsub_addr = codeaddr();
	oi(0, 4);
}

void o_num(int n)
{
	os("\xb8", 1);
	oi(n, 4);
	tmp_push(TMP_CONST);
}

static void deref(void)
{
	os("\x48\x8b\x00", 3);	/* mov (%rax), %rax */
}

void o_deref(void)
{
	if (tmp_pop() == TMP_ADDR)
		deref();
	tmp_push(TMP_ADDR);
}

void o_ret(int ret)
{
	if (ret) {
		if (tmp_pop() == TMP_ADDR)
			deref();
	} else {
		os("\x48\x31\xc0", 3);	/* xor %rax, %rax */
	}
	os("\xc9\xc3", 2);		/* leave; ret; */
}

static void binop(void)
{
	if (tmp_pop() == TMP_ADDR)
		deref();
	os("\x48\x89\xc3", 3);		/* mov %rax, %rbx */
	if (tmp_pop() == TMP_ADDR)
		deref();
}

void o_add(void)
{
	binop();
	os("\x48\x01\xd8", 3);		/* add %rax, %rbx */
	tmp_push(TMP_CONST);
}

void o_sub(void)
{
	binop();
	os("\x48\x29\xd8", 3);		/* sub %rax, %rbx */
	tmp_push(TMP_CONST);
}

void o_func_end(void)
{
	os("\xc9\xc3", 2);		/* leave; ret; */
	sec->len = cur - sec->buf;
	sec->sym->st_size = sec->len;
	putint(sec->buf + spsub_addr, maxsp + 8, 4);
}

void o_local(long addr)
{
	os("\x48\x89\xe8", 3);		/* mov %rbp, %rax */
	os("\x48\x05", 2);		/* add $addr, %rax */
	oi(-addr, 4);
	tmp_push(TMP_ADDR);
}

long o_mklocal(void)
{
	return sp_push(8);
}

long o_arg(int i)
{
	char mov[3];
	long addr = o_mklocal();
	mov[0] = "\x48\x48\x48\x48\x4c\x4c"[i];
	mov[1] = '\x89';
	mov[2] = "\xbd\xb5\x95\x8d\x85\x8d"[i];
	os(mov, 3);			/* mov %xxx, addr(%rbp) */
	oi(-addr, 4);
	return addr;
}

void o_assign(void)
{
	if (tmp_pop() == TMP_ADDR)
		deref();
	os("\x48\x89\xc3", 3);		/* mov %rax, %rbx */
	tmp_pop();
	os("\x48\x89\x18", 3);		/* mov %rbx, (%rax) */
}

long o_mklabel(void)
{
	return codeaddr();
}

void o_jz(long addr)
{
	os("\x48\x85\xc0", 3);		/* test %rax, %rax */
	os("\x0f\x84", 2);		/* jz $addr */
	oi(codeaddr() - addr - 4, 4);
}

long o_stubjz(void)
{
	o_jz(codeaddr());
	return cur - sec->buf - 4;
}

void o_filljz(long addr)
{
	putint(sec->buf + addr, codeaddr() - addr - 4, 4);
}

void out_init(void)
{
}

static int sym_find(char *name)
{
	int i;
	Elf64_Sym *sym;
	for (i = 0; i < nsyms; i++)
		if (!strcmp(name, symstr + syms[i].st_name))
			return i;
	sym = put_sym(name);
	sym->st_shndx = SHN_UNDEF;
	return sym - syms;
}

void o_symaddr(char *name)
{
	Elf64_Rela *rela = &sec->rela[sec->nrela++];
	os("\x48\xc7\xc0", 3);		/* mov $addr, %rax */
	rela->r_offset = codeaddr();
	rela->r_info = ELF64_R_INFO(sym_find(name), R_X86_64_32);
	oi(0, 4);
	tmp_push(TMP_ADDR);
}

static void setarg(int i)
{
	char mov[3];
	mov[0] = "\x48\x48\x48\x48\x49\x49"[i];
	mov[1] = '\x89';
	mov[2] = "\xc7\xc6\xc2\xc1\xc0\xc1"[i];
	os(mov, 3);			/* mov %rax, %xxx */
}

void o_call(int argc)
{
	int i;
	if (!argc)
		os("\x48\x31\xc0", 3);	/* xor %rax, %rax */
	for (i = 0; i < argc; i++) {
		if (tmp_pop() == TMP_ADDR)
			deref();
		setarg(argc - i - 1);
	}
	tmp_pop();
	os("\xff\xd0", 2);		/* callq *%rax */
	tmp_push(TMP_CONST);
}

void out_write(int fd)
{
	Elf64_Shdr *symstr_shdr = &shdr[SEC_SYMSTR];
	Elf64_Shdr *syms_shdr = &shdr[SEC_SYMS];
	unsigned long offset = sizeof(ehdr);
	int i;

	ehdr.e_ident[0] = 0x7f;
	ehdr.e_ident[1] = 'E';
	ehdr.e_ident[2] = 'L';
	ehdr.e_ident[3] = 'F';
	ehdr.e_ident[4] = ELFCLASS64;
	ehdr.e_ident[5] = ELFDATA2LSB;
	ehdr.e_ident[6] = EV_CURRENT;
	ehdr.e_type = ET_REL;
	ehdr.e_machine = EM_X86_64;
	ehdr.e_version = EV_CURRENT;
	ehdr.e_ehsize = sizeof(ehdr);
	ehdr.e_shentsize = sizeof(shdr[0]);
	ehdr.e_shoff = offset;
	ehdr.e_shnum = nshdr;
	ehdr.e_shstrndx = SEC_SYMSTR;
	offset += sizeof(shdr[0]) * nshdr;

	syms_shdr->sh_type = SHT_SYMTAB;
	syms_shdr->sh_offset = offset;
	syms_shdr->sh_size = nsyms * sizeof(syms[0]);
	syms_shdr->sh_entsize = sizeof(syms[0]);
	syms_shdr->sh_link = SEC_SYMSTR;
	offset += syms_shdr->sh_size;

	symstr_shdr->sh_type = SHT_STRTAB;
	symstr_shdr->sh_offset = offset;
	symstr_shdr->sh_size = nsymstr;
	symstr_shdr->sh_entsize = 1;
	offset += symstr_shdr->sh_size;

	for (i = 0; i < nsecs; i++) {
		struct sec *sec = &secs[i];

		sec->sec_shdr->sh_type = SHT_PROGBITS;
		sec->sec_shdr->sh_flags = SHF_EXECINSTR;
		sec->sec_shdr->sh_offset = offset;
		sec->sec_shdr->sh_size = sec->len;
		sec->sec_shdr->sh_entsize = 1;
		offset += sec->sec_shdr->sh_size;

		sec->rel_shdr->sh_type = SHT_RELA;
		sec->rel_shdr->sh_offset = offset;
		sec->rel_shdr->sh_size = sec->nrela * sizeof(sec->rela[0]);
		sec->rel_shdr->sh_entsize = sizeof(sec->rela[0]);
		offset += sec->rel_shdr->sh_size;
	}

	write(fd, &ehdr, sizeof(ehdr));
	write(fd, shdr, sizeof(shdr[0]) * nshdr);
	write(fd, syms, sizeof(syms[0]) * nsyms);
	write(fd, symstr, nsymstr);
	for (i = 0; i < nsecs; i++) {
		struct sec *sec = &secs[i];
		write(fd, sec->buf, sec->len);
		write(fd, sec->rela, sec->nrela * sizeof(sec->rela[0]));
	}
}
