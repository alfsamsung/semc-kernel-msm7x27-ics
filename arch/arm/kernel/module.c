/*
 *  linux/arch/arm/kernel/module.c
 *
 *  Copyright (C) 2002 Russell King.
 *  Modified for nommu by Hyok S. Choi
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Module allocation method suggested by Andi Kleen.
 */
#include <linux/module.h>
#include <linux/moduleloader.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/elf.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/gfp.h>

#include <asm/pgtable.h>
#include <asm/sections.h>
#include <asm/unwind.h>

#ifdef CONFIG_XIP_KERNEL
/*
 * The XIP kernel text is mapped in the module area for modules and
 * some other stuff to work without any indirect relocations.
 * MODULES_VADDR is redefined here and not in asm/memory.h to avoid
 * recompiling the whole kernel when CONFIG_XIP_KERNEL is turned on/off.
 */
#undef MODULES_VADDR
#define MODULES_VADDR	(((unsigned long)_etext + ~PGDIR_MASK) & PGDIR_MASK)
#endif

#ifdef CONFIG_MMU
void *module_alloc(unsigned long size)
{
	return __vmalloc_node_range(size, 1, MODULES_VADDR, MODULES_END,
				GFP_KERNEL, PAGE_KERNEL_EXEC, -1,
				__builtin_return_address(0));
}
#else /* CONFIG_MMU */
void *module_alloc(unsigned long size)
{
	return size == 0 ? NULL : vmalloc(size);
}
#endif /* !CONFIG_MMU */

void module_free(struct module *module, void *region)
{
	vfree(region);
}

int module_frob_arch_sections(Elf_Ehdr *hdr,
			      Elf_Shdr *sechdrs,
			      char *secstrings,
			      struct module *mod)
{
	return 0;
}

int
apply_relocate(Elf32_Shdr *sechdrs, const char *strtab, unsigned int symindex,
	       unsigned int relindex, struct module *module)
{
	Elf32_Shdr *symsec = sechdrs + symindex;
	Elf32_Shdr *relsec = sechdrs + relindex;
	Elf32_Shdr *dstsec = sechdrs + relsec->sh_info;
	Elf32_Rel *rel = (void *)relsec->sh_addr;
	unsigned int i;

	for (i = 0; i < relsec->sh_size / sizeof(Elf32_Rel); i++, rel++) {
		unsigned long loc;
		Elf32_Sym *sym;
		s32 offset;

		offset = ELF32_R_SYM(rel->r_info);
		if (offset < 0 || offset > (symsec->sh_size / sizeof(Elf32_Sym))) {
			printk(KERN_ERR "%s: bad relocation, section %d reloc %d\n",
				module->name, relindex, i);
			return -ENOEXEC;
		}

		sym = ((Elf32_Sym *)symsec->sh_addr) + offset;

		if (rel->r_offset < 0 || rel->r_offset > dstsec->sh_size - sizeof(u32)) {
			printk(KERN_ERR "%s: out of bounds relocation, "
				"section %d reloc %d offset %d size %d\n",
				module->name, relindex, i, rel->r_offset,
				dstsec->sh_size);
			return -ENOEXEC;
		}

		loc = dstsec->sh_addr + rel->r_offset;

		switch (ELF32_R_TYPE(rel->r_info)) {
		case R_ARM_NONE:
			/* ignore */
			break;

		case R_ARM_ABS32:
			*(u32 *)loc += sym->st_value;
			break;

		case R_ARM_PC24:
		case R_ARM_CALL:
		case R_ARM_JUMP24:
			offset = (*(u32 *)loc & 0x00ffffff) << 2;
			if (offset & 0x02000000)
				offset -= 0x04000000;

			offset += sym->st_value - loc;
			if (offset & 3 ||
			    offset <= (s32)0xfe000000 ||
			    offset >= (s32)0x02000000) {
				printk(KERN_ERR
				       "%s: relocation out of range, section "
				       "%d reloc %d sym '%s'\n", module->name,
				       relindex, i, strtab + sym->st_name);
				return -ENOEXEC;
			}

			offset >>= 2;

			*(u32 *)loc &= 0xff000000;
			*(u32 *)loc |= offset & 0x00ffffff;
			break;

		case R_ARM_V4BX:
		       /* Preserve Rm and the condition code. Alter
			* other bits to re-code instruction as
			* MOV PC,Rm.
			*/
		       *(u32 *)loc &= 0xf000000f;
		       *(u32 *)loc |= 0x01a0f000;
		       break;
		      
		case R_ARM_PREL31:
			offset = *(u32 *)loc + sym->st_value - loc;
			*(u32 *)loc = offset & 0x7fffffff;
			break;
		       
		case R_ARM_MOVW_ABS_NC:
		case R_ARM_MOVT_ABS:
			offset = *(u32 *)loc;
			offset = ((offset & 0xf0000) >> 4) | (offset & 0xfff);
			offset = (offset ^ 0x8000) - 0x8000;

			offset += sym->st_value;
			if (ELF32_R_TYPE(rel->r_info) == R_ARM_MOVT_ABS)
				offset >>= 16;

			*(u32 *)loc &= 0xfff0f000;
			*(u32 *)loc |= ((offset & 0xf000) << 4) |
					(offset & 0x0fff);
			break;

		default:
			printk(KERN_ERR "%s: unknown relocation: %u\n",
			       module->name, ELF32_R_TYPE(rel->r_info));
			return -ENOEXEC;
		}
	}
	return 0;
}

int
apply_relocate_add(Elf32_Shdr *sechdrs, const char *strtab,
		   unsigned int symindex, unsigned int relsec, struct module *module)
{
	printk(KERN_ERR "module %s: ADD RELOCATION unsupported\n",
	       module->name);
	return -ENOEXEC;
}

struct mod_unwind_map {
	const Elf_Shdr *unw_sec;
	const Elf_Shdr *txt_sec;
};

int module_finalize(const Elf32_Ehdr *hdr, const Elf_Shdr *sechdrs,
		    struct module *mod)
{
#ifdef CONFIG_ARM_UNWIND
	const char *secstrs = (void *)hdr + sechdrs[hdr->e_shstrndx].sh_offset;
	const Elf_Shdr *s, *sechdrs_end = sechdrs + hdr->e_shnum;
	struct mod_unwind_map maps[ARM_SEC_MAX];
	int i;
	memset(maps, 0, sizeof(maps));

	for (s = sechdrs; s < sechdrs_end; s++) {
		const char *secname = secstrs + s->sh_name;
		
		if (!(s->sh_flags & SHF_ALLOC))
			continue;

		if (strcmp(".ARM.exidx.init.text", secname) == 0)
			maps[ARM_SEC_INIT].unw_sec = s;
		else if (strcmp(".ARM.exidx.devinit.text", secname) == 0)
			maps[ARM_SEC_DEVINIT].unw_sec = s;
		else if (strcmp(".ARM.exidx", secname) == 0)
			maps[ARM_SEC_CORE].unw_sec = s;
		else if (strcmp(".ARM.exidx.exit.text", secname) == 0)
			maps[ARM_SEC_EXIT].unw_sec = s;
		else if (strcmp(".ARM.exidx.devexit.text", secname) == 0)
			maps[ARM_SEC_DEVEXIT].unw_sec = s;
		else if (strcmp(".init.text", secname) == 0)
			maps[ARM_SEC_INIT].txt_sec = s;
		else if (strcmp(".devinit.text", secname) == 0)
			maps[ARM_SEC_DEVINIT].txt_sec = s;
		else if (strcmp(".text", secname) == 0)
			maps[ARM_SEC_CORE].txt_sec = s;
		else if (strcmp(".exit.text", secname) == 0)
			maps[ARM_SEC_EXIT].txt_sec = s;
		else if (strcmp(".devexit.text", secname) == 0)
			maps[ARM_SEC_DEVEXIT].txt_sec = s;
	}
	for (i = 0; i < ARM_SEC_MAX; i++)
		if (maps[i].unw_sec && maps[i].txt_sec)
			mod->arch.unwind[i] =
				unwind_table_add(maps[i].unw_sec->sh_addr,
						  maps[i].unw_sec->sh_size,
						  maps[i].txt_sec->sh_addr,
						  maps[i].txt_sec->sh_size);
#endif
	return 0;
}

void
module_arch_cleanup(struct module *mod)
{
#ifdef CONFIG_ARM_UNWIND
	int i;

	for (i = 0; i < ARM_SEC_MAX; i++)
		if (mod->arch.unwind[i])
			unwind_table_del(mod->arch.unwind[i]);
#endif
}
