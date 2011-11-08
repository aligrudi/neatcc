#include <elf.h>
#include <string.h>
#include <unistd.h>
#include "out.h"

#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))

#define MAXSYMS			(1 << 12)
#define MAXRELA			(1 << 12)
#define SEC_TEXT		1
#define SEC_RELA		2
#define SEC_SYMS		3
#define SEC_SYMSTR		4
#define SEC_DAT			5
#define SEC_DATRELA		6
#define SEC_BSS			7
#define NSECS			8

static Elf64_Ehdr ehdr;
static Elf64_Shdr shdr[NSECS];
static Elf64_Sym syms[MAXSYMS];
static int nsyms = 1;
static char symstr[MAXSYMS * 8];
static int nsymstr = 1;

static Elf64_Rela datrela[MAXRELA];
static int ndatrela;
static Elf64_Rela rela[MAXRELA];
static int nrela;

static int symstr_add(char *name)
{
	int len = strlen(name) + 1;
	strcpy(symstr + nsymstr, name);
	nsymstr += len;
	return nsymstr - len;
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
	int found = sym_find(name);
	Elf64_Sym *sym = found != -1 ? &syms[found] : &syms[nsyms++];
	if (found >= 0)
		return sym;
	sym->st_name = symstr_add(name);
	sym->st_shndx = SHN_UNDEF;
	sym->st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
	return sym;
}

#define SYMLOCAL(i)		(ELF64_ST_BIND(syms[i].st_info) == STB_LOCAL)

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
	int glob_beg = 1;
	for (i = 0; i < nsyms; i++)
		mv[i] = i;
	i = 1;
	j = nsyms - 1;
	while (1) {
		Elf64_Sym t;
		while (i < j && SYMLOCAL(i))
			i++;
		while (j >= i && !SYMLOCAL(j))
			j--;
		if (i >= j)
			break;
		t = syms[j];
		syms[j] = syms[i];
		syms[i] = t;
		mv[i] = j;
		mv[j] = i;
	}
	glob_beg = j + 1;
	mvrela(mv, rela, nrela);
	mvrela(mv, datrela, ndatrela);
	return glob_beg;
}

void out_init(int flags)
{
}

void out_sym(char *name, int flags, int off, int len)
{
	Elf64_Sym *sym = put_sym(name);
	int type = (flags & OUT_CS) ? STT_FUNC : STT_OBJECT;
	int bind = (flags & OUT_GLOB) ? STB_GLOBAL : STB_LOCAL;
	if (flags & OUT_CS)
		sym->st_shndx = SEC_TEXT;
	if (flags & OUT_DS)
		sym->st_shndx = SEC_DAT;
	if (flags & OUT_BSS)
		sym->st_shndx = SEC_BSS;
	sym->st_info = ELF64_ST_INFO(bind, type);
	sym->st_value = off;
	sym->st_size = len;
}

static int rel_type(int flags)
{
	return flags & OUT_REL ? R_X86_64_PC32 : R_X86_64_64;
}

static void out_csrel(int idx, int off, int flags)
{
	Elf64_Rela *r = &rela[nrela++];
	r->r_offset = off;
	r->r_info = ELF64_R_INFO(idx, rel_type(flags));
}

static void out_dsrel(int idx, int off, int flags)
{
	Elf64_Rela *r = &datrela[ndatrela++];
	r->r_offset = off;
	r->r_info = ELF64_R_INFO(idx, rel_type(flags));
}

void out_rel(char *name, int flags, int off)
{
	Elf64_Sym *sym = put_sym(name);
	int idx = sym - syms;
	if (flags & OUT_DS)
		out_dsrel(idx, off, flags);
	else
		out_csrel(idx, off, flags);
}

static int bss_len(void)
{
	int len = 0;
	int i;
	for (i = 0; i < nsyms; i++) {
		int end = syms[i].st_value + syms[i].st_size;
		if (syms[i].st_shndx == SEC_BSS)
			if (len < end)
				len = end;
	}
	return len;
}

void out_write(int fd, char *cs, int cslen, char *ds, int dslen)
{
	Elf64_Shdr *text_shdr = &shdr[SEC_TEXT];
	Elf64_Shdr *rela_shdr = &shdr[SEC_RELA];
	Elf64_Shdr *symstr_shdr = &shdr[SEC_SYMSTR];
	Elf64_Shdr *syms_shdr = &shdr[SEC_SYMS];
	Elf64_Shdr *dat_shdr = &shdr[SEC_DAT];
	Elf64_Shdr *datrela_shdr = &shdr[SEC_DATRELA];
	Elf64_Shdr *bss_shdr = &shdr[SEC_BSS];
	unsigned long offset = sizeof(ehdr);

	/* workaround for the idiotic gnuld; use neatld instead! */
	text_shdr->sh_name = symstr_add(".cs");
	rela_shdr->sh_name = symstr_add(".rel.cs");
	dat_shdr->sh_name = symstr_add(".ds");
	datrela_shdr->sh_name = symstr_add(".rel.ds");

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
	ehdr.e_shnum = NSECS;
	ehdr.e_shstrndx = SEC_SYMSTR;
	offset += sizeof(shdr[0]) * NSECS;

	text_shdr->sh_type = SHT_PROGBITS;
	text_shdr->sh_flags = SHF_EXECINSTR | SHF_ALLOC;
	text_shdr->sh_offset = offset;
	text_shdr->sh_size = cslen;
	text_shdr->sh_entsize = 1;
	text_shdr->sh_addralign = OUT_ALIGNMENT;
	offset += text_shdr->sh_size;

	rela_shdr->sh_type = SHT_RELA;
	rela_shdr->sh_link = SEC_SYMS;
	rela_shdr->sh_info = SEC_TEXT;
	rela_shdr->sh_offset = offset;
	rela_shdr->sh_size = nrela * sizeof(rela[0]);
	rela_shdr->sh_entsize = sizeof(rela[0]);
	offset += rela_shdr->sh_size;

	syms_shdr->sh_type = SHT_SYMTAB;
	syms_shdr->sh_offset = offset;
	syms_shdr->sh_size = nsyms * sizeof(syms[0]);
	syms_shdr->sh_entsize = sizeof(syms[0]);
	syms_shdr->sh_link = SEC_SYMSTR;
	syms_shdr->sh_info = syms_sort();
	offset += syms_shdr->sh_size;

	dat_shdr->sh_type = SHT_PROGBITS;
	dat_shdr->sh_flags = SHF_ALLOC | SHF_WRITE;
	dat_shdr->sh_offset = offset;
	dat_shdr->sh_size = dslen;
	dat_shdr->sh_entsize = 1;
	dat_shdr->sh_addralign = OUT_ALIGNMENT;
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
	bss_shdr->sh_size = bss_len();
	bss_shdr->sh_entsize = 1;
	bss_shdr->sh_addralign = OUT_ALIGNMENT;

	symstr_shdr->sh_type = SHT_STRTAB;
	symstr_shdr->sh_offset = offset;
	symstr_shdr->sh_size = nsymstr;
	symstr_shdr->sh_entsize = 1;
	offset += symstr_shdr->sh_size;

	write(fd, &ehdr, sizeof(ehdr));
	write(fd, shdr,  NSECS * sizeof(shdr[0]));
	write(fd, cs, cslen);
	write(fd, rela, nrela * sizeof(rela[0]));
	write(fd, syms, nsyms * sizeof(syms[0]));
	write(fd, ds, dslen);
	write(fd, datrela, ndatrela * sizeof(datrela[0]));
	write(fd, symstr, nsymstr);
}
