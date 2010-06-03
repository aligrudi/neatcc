#include <elf.h>
#include <string.h>
#include <unistd.h>
#include "gen.h"

#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))

#define MAXSECS			(1 << 10)
#define MAXSYMS			(1 << 12)
#define MAXRELA			(1 << 12)
#define SEC_SYMS		1
#define SEC_SYMSTR		2
#define SEC_DAT			3
#define SEC_DATRELA		4
#define SEC_BSS			5
#define SEC_BEG			6

static Elf64_Ehdr ehdr;
static Elf64_Shdr shdr[MAXSECS];
static int nshdr = SEC_BEG;
static Elf64_Sym syms[MAXSYMS];
static int nsyms = 1;
static char symstr[MAXSYMS * 8];
static int nsymstr = 1;
static int bsslen;
static char dat[SECSIZE];
static int datlen;
static Elf64_Rela datrela[MAXRELA];
static int ndatrela;

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

static char *putstr(char *s, char *r)
{
	while (*r)
		*s++ = *r++;
	*s++ = '\0';
	return s;
}

static int sym_find(char *name)
{
	int i;
	for (i = 0; i < nsyms; i++)
		if (!strcmp(name, symstr + syms[i].st_name))
			return i;
	return -1;
}

static Elf64_Sym *put_sym(char *name)
{
	int found = name ? sym_find(name) : -1;
	Elf64_Sym *sym = found != -1 ? &syms[found] : &syms[nsyms++];
	if (!name)
		sym->st_name = 0;
	if (name && found == -1) {
		sym->st_name = nsymstr;
		nsymstr = putstr(symstr + nsymstr, name) - symstr;
	}
	return sym;
}

#define S_BIND(global)		((global) ? STB_GLOBAL : STB_LOCAL)

long out_func_beg(char *name, int global)
{
	sec = &secs[nsecs++];
	sec->sym = put_sym(name);
	sec->sym->st_shndx = nshdr;
	sec->sym->st_info = ELF64_ST_INFO(S_BIND(global), STT_FUNC);
	sec->sec_shdr = &shdr[nshdr++];
	sec->rel_shdr = &shdr[nshdr++];
	sec->rel_shdr->sh_link = SEC_SYMS;
	sec->rel_shdr->sh_info = sec->sec_shdr - shdr;
	return sec->sym - syms;
}

void out_func_end(char *buf, int len)
{
	memcpy(sec->buf, buf, len);
	sec->len = len;
	sec->sym->st_size = len;
}

void out_rela(long addr, int off, int rel)
{
	Elf64_Rela *rela = &sec->rela[sec->nrela++];
	rela->r_offset = off;
	rela->r_info = ELF64_R_INFO(addr, rel ? R_X86_64_PC32 : R_X86_64_32);
}

void out_datrela(long addr, long dataddr, int off)
{
	Elf64_Rela *rela = &datrela[ndatrela++];
	rela->r_offset = syms[dataddr].st_value + off;
	rela->r_info = ELF64_R_INFO(addr, R_X86_64_32);
}

#define SYMLOCAL(i)		(ELF64_ST_BIND(syms[i].st_info) & STB_LOCAL)

static void mvrela(int *mv, Elf64_Rela *rela, int nrela)
{
	int i;
	for (i = 0; i < nrela; i++) {
		int sym = ELF64_R_SYM(rela[i].r_info);
		int type = ELF64_R_TYPE(rela[i].r_info);
		rela[i].r_info = ELF64_R_INFO(mv[sym], type);
	}
}

static int syms_sort(void)
{
	int mv[MAXSYMS];
	int i, j;
	int glob_beg;
	for (i = 0; i < nsyms; i++)
		mv[i] = i;
	i = 1;
	j = nsyms - 1;
	while (1) {
		Elf64_Sym t;
		while (j > i && !SYMLOCAL(j))
			j--;
		while (i < j && SYMLOCAL(i))
			i++;
		if (i >= j)
			break;
		t = syms[j];
		syms[i] = syms[j];
		syms[i] = t;
		mv[i] = j;
		mv[j] = i;
	}
	glob_beg = i;
	for (i = 0; i < nsecs; i++)
		mvrela(mv, sec[i].rela, sec[i].nrela);
	mvrela(mv, datrela, ndatrela);
	return glob_beg;
}

void out_write(int fd)
{
	Elf64_Shdr *symstr_shdr = &shdr[SEC_SYMSTR];
	Elf64_Shdr *syms_shdr = &shdr[SEC_SYMS];
	Elf64_Shdr *dat_shdr = &shdr[SEC_DAT];
	Elf64_Shdr *datrela_shdr = &shdr[SEC_DATRELA];
	Elf64_Shdr *bss_shdr = &shdr[SEC_BSS];
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
	syms_shdr->sh_info = syms_sort();
	offset += syms_shdr->sh_size;

	dat_shdr->sh_type = SHT_PROGBITS;
	dat_shdr->sh_flags = SHF_WRITE;
	dat_shdr->sh_offset = offset;
	dat_shdr->sh_size = datlen;
	dat_shdr->sh_entsize = 1;
	dat_shdr->sh_addralign = 8;
	offset += dat_shdr->sh_size;

	datrela_shdr->sh_type = SHT_RELA;
	datrela_shdr->sh_offset = offset;
	datrela_shdr->sh_size = ndatrela * sizeof(datrela[0]);
	datrela_shdr->sh_entsize = sizeof(datrela[0]);
	datrela_shdr->sh_link = SEC_SYMS;
	datrela_shdr->sh_info = SEC_DAT;
	offset += datrela_shdr->sh_size;

	bss_shdr->sh_type = SHT_NOBITS;
	bss_shdr->sh_flags = SHF_ALLOC | SHF_WRITE;
	bss_shdr->sh_offset = offset;
	bss_shdr->sh_size = bsslen;
	bss_shdr->sh_entsize = 1;
	bss_shdr->sh_addralign = 8;

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
	write(fd, dat, datlen);
	write(fd, datrela, ndatrela * sizeof(datrela[0]));
	write(fd, symstr, nsymstr);
	for (i = 0; i < nsecs; i++) {
		struct sec *sec = &secs[i];
		write(fd, sec->buf, sec->len);
		write(fd, sec->rela, sec->nrela * sizeof(sec->rela[0]));
	}
}

long out_mkvar(char *name, int size, int global)
{
	Elf64_Sym *sym = put_sym(name);
	sym->st_shndx = SEC_BSS;
	sym->st_value = bsslen;
	sym->st_size = size;
	sym->st_info = ELF64_ST_INFO(S_BIND(global), STT_OBJECT);
	bsslen = ALIGN(bsslen + size, 8);
	return sym - syms;
}

long out_mkundef(char *name, int sz)
{
	Elf64_Sym *sym = put_sym(name);
	sym->st_shndx = SHN_UNDEF;
	sym->st_info = ELF64_ST_INFO(STB_GLOBAL, sz ? STT_OBJECT : STT_FUNC);
	sym->st_size = sz;
	return sym - syms;
}

long out_mkdat(char *name, char *buf, int len, int global)
{
	Elf64_Sym *sym = put_sym(name);
	sym->st_shndx = SEC_DAT;
	sym->st_value = datlen;
	sym->st_size = len;
	sym->st_info = ELF64_ST_INFO(S_BIND(global), STT_OBJECT);
	if (buf)
		memcpy(dat + datlen, buf, len);
	else
		memset(dat + datlen, 0, len);
	datlen = ALIGN(datlen + len, 8);
	return sym - syms;
}

void out_datcpy(long addr, int off, char *buf, int len)
{
	memcpy(dat + syms[addr].st_value + off, buf, len);
}
