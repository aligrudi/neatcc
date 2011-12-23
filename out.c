#include <elf.h>
#include <string.h>
#include <unistd.h>
#include "out.h"
#include "gen.h"

#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))

#define MAXSYMS			(1 << 12)
#define MAXREL			(1 << 12)
#define SEC_TEXT		1
#define SEC_REL			2
#define SEC_SYMS		3
#define SEC_SYMSTR		4
#define SEC_DAT			5
#define SEC_DATREL		6
#define SEC_BSS			7
#define NSECS			8

/* simplifed elf struct and macro names */
#if LONGSZ == 8
#  define USERELA	1
#  define Elf_Ehdr	Elf64_Ehdr
#  define Elf_Shdr	Elf64_Shdr
#  define Elf_Sym	Elf64_Sym
#  define Elf_Rel	Elf64_Rela
#  define ELF_ST_INFO	ELF64_ST_INFO
#  define ELF_ST_BIND	ELF64_ST_BIND
#  define ELF_R_SYM	ELF64_R_SYM
#  define ELF_R_TYPE	ELF64_R_TYPE
#  define ELF_R_INFO	ELF64_R_INFO
#else
#  define USERELA	0
#  define Elf_Ehdr	Elf32_Ehdr
#  define Elf_Shdr	Elf32_Shdr
#  define Elf_Sym	Elf32_Sym
#  define Elf_Rel	Elf32_Rel
#  define ELF_ST_INFO	ELF32_ST_INFO
#  define ELF_ST_BIND	ELF32_ST_BIND
#  define ELF_R_SYM	ELF32_R_SYM
#  define ELF_R_TYPE	ELF32_R_TYPE
#  define ELF_R_INFO	ELF32_R_INFO
#endif

static Elf_Ehdr ehdr;
static Elf_Shdr shdr[NSECS];
static Elf_Sym syms[MAXSYMS];
static int nsyms = 1;
static char symstr[MAXSYMS * 8];
static int nsymstr = 1;

static Elf_Rel dsrels[MAXREL];
static int ndsrels;
static Elf_Rel rels[MAXREL];
static int nrels;

static int rel_type(int flags);
static void ehdr_init(Elf_Ehdr *ehdr);

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

static Elf_Sym *put_sym(char *name)
{
	int found = sym_find(name);
	Elf_Sym *sym = found != -1 ? &syms[found] : &syms[nsyms++];
	if (found >= 0)
		return sym;
	sym->st_name = symstr_add(name);
	sym->st_shndx = SHN_UNDEF;
	sym->st_info = ELF_ST_INFO(STB_GLOBAL, STT_FUNC);
	return sym;
}

#define SYMLOCAL(i)		(ELF_ST_BIND(syms[i].st_info) == STB_LOCAL)

static void mvrela(int *mv, Elf_Rel *rels, int nrels)
{
	int i;
	for (i = 0; i < nrels; i++) {
		int sym = ELF_R_SYM(rels[i].r_info);
		int type = ELF_R_TYPE(rels[i].r_info);
		rels[i].r_info = ELF_R_INFO(mv[sym], type);
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
		Elf_Sym t;
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
	mvrela(mv, rels, nrels);
	mvrela(mv, dsrels, ndsrels);
	return glob_beg;
}

void out_init(int flags)
{
}

void out_sym(char *name, int flags, int off, int len)
{
	Elf_Sym *sym = put_sym(name);
	int type = (flags & OUT_CS) ? STT_FUNC : STT_OBJECT;
	int bind = (flags & OUT_GLOB) ? STB_GLOBAL : STB_LOCAL;
	if (flags & OUT_CS)
		sym->st_shndx = SEC_TEXT;
	if (flags & OUT_DS)
		sym->st_shndx = SEC_DAT;
	if (flags & OUT_BSS)
		sym->st_shndx = SEC_BSS;
	sym->st_info = ELF_ST_INFO(bind, type);
	sym->st_value = off;
	sym->st_size = len;
}

static void out_csrel(int idx, int off, int flags)
{
	Elf_Rel *r = &rels[nrels++];
	r->r_offset = off;
	r->r_info = ELF_R_INFO(idx, rel_type(flags));
}

static void out_dsrel(int idx, int off, int flags)
{
	Elf_Rel *r = &dsrels[ndsrels++];
	r->r_offset = off;
	r->r_info = ELF_R_INFO(idx, rel_type(flags));
}

void out_rel(char *name, int flags, int off)
{
	Elf_Sym *sym = put_sym(name);
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
	Elf_Shdr *text_shdr = &shdr[SEC_TEXT];
	Elf_Shdr *rela_shdr = &shdr[SEC_REL];
	Elf_Shdr *symstr_shdr = &shdr[SEC_SYMSTR];
	Elf_Shdr *syms_shdr = &shdr[SEC_SYMS];
	Elf_Shdr *dat_shdr = &shdr[SEC_DAT];
	Elf_Shdr *datrel_shdr = &shdr[SEC_DATREL];
	Elf_Shdr *bss_shdr = &shdr[SEC_BSS];
	unsigned long offset = sizeof(ehdr);

	/* workaround for the idiotic gnuld; use neatld instead! */
	text_shdr->sh_name = symstr_add(".cs");
	rela_shdr->sh_name = symstr_add(USERELA ? ".rela.cs" : ".rels.cs");
	dat_shdr->sh_name = symstr_add(".ds");
	datrel_shdr->sh_name = symstr_add(USERELA ? ".rela.ds" : ".rels.ds");

	ehdr.e_ident[0] = 0x7f;
	ehdr.e_ident[1] = 'E';
	ehdr.e_ident[2] = 'L';
	ehdr.e_ident[3] = 'F';
	ehdr.e_ident[4] = LONGSZ == 8 ? ELFCLASS64 : ELFCLASS32;
	ehdr.e_ident[5] = ELFDATA2LSB;
	ehdr.e_ident[6] = EV_CURRENT;
	ehdr.e_type = ET_REL;
	ehdr_init(&ehdr);
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

	rela_shdr->sh_type = USERELA ? SHT_RELA : SHT_REL;
	rela_shdr->sh_link = SEC_SYMS;
	rela_shdr->sh_info = SEC_TEXT;
	rela_shdr->sh_offset = offset;
	rela_shdr->sh_size = nrels * sizeof(rels[0]);
	rela_shdr->sh_entsize = sizeof(rels[0]);
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

	datrel_shdr->sh_type = USERELA ? SHT_RELA : SHT_REL;
	datrel_shdr->sh_offset = offset;
	datrel_shdr->sh_size = ndsrels * sizeof(dsrels[0]);
	datrel_shdr->sh_entsize = sizeof(dsrels[0]);
	datrel_shdr->sh_link = SEC_SYMS;
	datrel_shdr->sh_info = SEC_DAT;
	offset += datrel_shdr->sh_size;

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
	write(fd, rels, nrels * sizeof(rels[0]));
	write(fd, syms, nsyms * sizeof(syms[0]));
	write(fd, ds, dslen);
	write(fd, dsrels, ndsrels * sizeof(dsrels[0]));
	write(fd, symstr, nsymstr);
}

/* architecture dependent functions */

#ifdef NEATCC_ARM
static void ehdr_init(Elf_Ehdr *ehdr)
{
	ehdr->e_machine = EM_ARM;
	ehdr->e_flags = EF_ARM_EABI_VER4;
}

static int rel_type(int flags)
{
	if (flags & OUT_RL24)
		return R_ARM_PC24;
	return flags & OUT_RLREL ? R_ARM_REL32 : R_ARM_ABS32;

}
#endif

#ifdef NEATCC_X64
static void ehdr_init(Elf_Ehdr *ehdr)
{
	ehdr->e_machine = EM_X86_64;
}

static int rel_type(int flags)
{
	if (flags & OUT_RLREL)
		return R_X86_64_PC32;
	if (flags & OUT_RL32)
		return flags & OUT_RLSX ? R_X86_64_32S : R_X86_64_32;
	return R_X86_64_64;
}
#endif

#ifdef NEATCC_X86
static void ehdr_init(Elf_Ehdr *ehdr)
{
	ehdr->e_machine = EM_386;
}

static int rel_type(int flags)
{
	return flags & OUT_RLREL ? R_386_PC32 : R_386_32;
}
#endif
