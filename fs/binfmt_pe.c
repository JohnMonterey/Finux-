// SPDX-License-Identifier: GPL-2.0
/*
 * Load PE/COFF (Windows) executables.
 *
 * This is a binfmt handler, exactly like fs/binfmt_elf.c, but for the
 * Portable Executable format that Microsoft Windows uses.  When execve()
 * is handed a file whose first bytes are "MZ" and whose PE header says it
 * is a 64-bit AMD64 image, this loader maps it into memory the way the
 * Windows loader would and transfers control to its entry point.
 *
 * Scope of this file
 * ------------------
 * This is the *loader*: it turns a PE file on disk into a running image.
 * That means header validation, mapping each section at its preferred
 * address with the memory protection the section asks for, zero-filling
 * the uninitialised tail (.bss), and starting the thread at the entry
 * point.  It is enough to run a freestanding native PE - one that makes
 * its own system calls and imports nothing.
 *
 * It deliberately stops there.  A real Windows program additionally needs
 *
 *   - its imports resolved against ntdll/kernel32 and friends,
 *   - the NT process environment block (PEB/TEB) laid out in memory,
 *   - base relocations applied when it cannot get its preferred address,
 *   - and the NT system-call surface behind ntdll.
 *
 * None of those are needed to *load* the image, so none of them live here.
 * They are built on top of this loader.  Where this file makes a
 * simplifying assumption it rejects the image with -ENOEXEC rather than
 * loading it wrongly; those boundaries are called out at each check.
 *
 * The on-disk structures come from <linux/pe.h>, which the EFI stub
 * already uses to describe the kernel's own PE header.  PE is a
 * little-endian format and this loader only supports the AMD64 machine
 * type, so the fields are read natively; the build depends on X86_64.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/errno.h>
#include <linux/binfmts.h>
#include <linux/personality.h>
#include <linux/ptrace.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/pe.h>
#include <linux/uaccess.h>

/*
 * Bounds we impose on a PE image.  These are sanity limits, not format
 * limits: they keep a malformed or hostile header from asking us to
 * allocate or iterate without bound.
 *
 * The MZ header's peaddr (the file offset of the PE header) is read from
 * bprm->buf, but the PE header itself is read from the file, so this bound
 * is a plausibility limit rather than a buffer limit.  Real toolchains put
 * the PE header within the first few hundred bytes; 1 KiB is generous.
 */
#define PE_MAX_HEADER_OFFSET	1024
#define PE_MAX_SECTIONS		96
#define PE_MAX_IMAGE_SIZE	(1UL << 31)	/* 2 GiB */
#define PE_MAX_DATA_DIRS	16

/*
 * The validated, digested form of a PE header.  Everything the mapping
 * step needs, with every field already range-checked.  Produced by
 * pe_parse_headers() from the raw on-disk headers; that function is pure
 * (no I/O, no current-> access) so it can be unit tested on its own.
 */
struct pe_load_info {
	unsigned long image_base;	/* preferred load address */
	unsigned long image_size;	/* size of the mapped image */
	unsigned long entry;		/* entry point, relative to image_base */
	unsigned long header_size;	/* size of the headers to map read-only */
	unsigned long code_base;	/* base of code, relative to image_base */
	u32 section_align;		/* in-memory section alignment */
	u32 file_align;			/* on-disk section alignment */
	u16 nsections;
	loff_t section_table;		/* file offset of the section table */
};

static int load_pe_binary(struct linux_binprm *bprm);

static struct linux_binfmt pe_format = {
	.module		= THIS_MODULE,
	.load_binary	= load_pe_binary,
};

/*
 * Translate a section's IMAGE_SCN_MEM_* characteristics into an mmap
 * PROT_* mask.  A section with no access bits at all is treated as
 * read-only, matching what the Windows loader does with such a section.
 */
static int pe_section_prot(u32 flags)
{
	int prot = 0;

	if (flags & IMAGE_SCN_MEM_READ)
		prot |= PROT_READ;
	if (flags & IMAGE_SCN_MEM_WRITE)
		prot |= PROT_WRITE;
	if (flags & IMAGE_SCN_MEM_EXECUTE)
		prot |= PROT_EXEC;
	if (!prot)
		prot = PROT_READ;
	return prot;
}

/**
 * pe_parse_headers - validate raw PE headers and digest them
 * @mz:		the first bytes of the file (at least the MZ header)
 * @mz_len:	how many bytes @mz points at (bprm->buf is BINPRM_BUF_SIZE)
 * @pe:		the PE/COFF header, already read from the file at mz->peaddr
 * @opt:	the PE32+ optional header, read immediately after @pe
 * @out:	filled in on success
 *
 * Returns 0 and populates @out, or -ENOEXEC if the file is not a PE image
 * this loader can handle.  This is where every "we only support ..."
 * decision is made, so it is deliberately strict and side-effect free.
 */
static int pe_parse_headers(const void *mz, size_t mz_len,
			    const struct pe_hdr *pe,
			    const struct pe32plus_opt_hdr *opt,
			    struct pe_load_info *out)
{
	const struct mz_hdr *mzh = mz;
	unsigned long image_base, image_size, entry, header_size;
	u32 section_align, file_align, opt_size;
	unsigned long table, table_end;
	u16 nsections;

	if (mz_len < sizeof(*mzh))
		return -ENOEXEC;

	/* "MZ" and a plausible PE-header pointer within the buffer we have. */
	if (mzh->magic != IMAGE_DOS_SIGNATURE)
		return -ENOEXEC;
	if (mzh->peaddr < sizeof(*mzh) || mzh->peaddr > PE_MAX_HEADER_OFFSET)
		return -ENOEXEC;

	/* "PE\0\0", AMD64, an executable image and not a DLL. */
	if (pe->magic != IMAGE_NT_SIGNATURE)
		return -ENOEXEC;
	if (pe->machine != IMAGE_FILE_MACHINE_AMD64)
		return -ENOEXEC;
	if (!(pe->flags & IMAGE_FILE_EXECUTABLE_IMAGE))
		return -ENOEXEC;
	if (pe->flags & IMAGE_FILE_DLL)
		return -ENOEXEC;

	/*
	 * The optional header must be a PE32+ (64-bit) one and at least as
	 * large as the fixed part we are about to read.  It may be larger -
	 * the data directories follow it - and opt_hdr_size tells us where
	 * the section table begins.
	 */
	opt_size = pe->opt_hdr_size;
	if (opt_size < sizeof(*opt))
		return -ENOEXEC;
	if (opt->magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		return -ENOEXEC;
	if (opt->data_dirs > PE_MAX_DATA_DIRS)
		return -ENOEXEC;

	nsections = pe->sections;
	if (nsections < 1 || nsections > PE_MAX_SECTIONS)
		return -ENOEXEC;

	image_base = opt->image_base;
	image_size = opt->image_size;
	entry = opt->entry_point;
	header_size = opt->header_size;
	section_align = opt->section_align;
	file_align = opt->file_align;

	/*
	 * Alignment.  The in-memory section alignment has to be a whole
	 * number of pages so that each section lands on its own page with
	 * its own protection.  This loader maps sections straight from the
	 * file, so it also needs the on-disk alignment to be page-aligned;
	 * an image built with a sub-page file alignment (the old 512-byte
	 * default) needs the copy-in-from-file path, which is future work.
	 */
	if (section_align < PAGE_SIZE || !IS_ALIGNED(section_align, PAGE_SIZE))
		return -ENOEXEC;
	if (file_align < PAGE_SIZE || !IS_ALIGNED(file_align, PAGE_SIZE))
		return -ENOEXEC;

	/* The image must sit page-aligned in a sane, non-empty address range. */
	if (!image_base || !IS_ALIGNED(image_base, PAGE_SIZE))
		return -ENOEXEC;
	if (!image_size || image_size > PE_MAX_IMAGE_SIZE)
		return -ENOEXEC;
	if (image_base > TASK_SIZE || image_size > TASK_SIZE - image_base)
		return -ENOEXEC;

	/* The entry point and the headers must fall inside the image. */
	if (!entry || entry >= image_size)
		return -ENOEXEC;
	if (header_size > image_size)
		return -ENOEXEC;

	/*
	 * The section table follows the PE header and the (variable length)
	 * optional header.  Make sure the whole table fits before the first
	 * section's data, i.e. within the headers region.
	 */
	table = mzh->peaddr + sizeof(*pe) + opt_size;
	if (table < mzh->peaddr)			/* wrap */
		return -ENOEXEC;
	table_end = table + (unsigned long)nsections * sizeof(struct section_header);
	if (table_end < table || table_end > header_size)
		return -ENOEXEC;

	out->image_base = image_base;
	out->image_size = image_size;
	out->entry = entry;
	out->header_size = header_size;
	out->code_base = opt->code_base;
	out->section_align = section_align;
	out->file_align = file_align;
	out->nsections = nsections;
	out->section_table = table;
	return 0;
}

/* Read exactly @len bytes from @file at @pos, or fail. */
static int pe_read_exact(struct file *file, void *buf, size_t len, loff_t pos)
{
	ssize_t n = kernel_read(file, buf, len, &pos);

	if (n < 0)
		return n;
	if ((size_t)n != len)
		return -ENOEXEC;
	return 0;
}

/*
 * Map one section into the new address space.
 *
 * The section's file data is mapped straight from @file with the
 * protection the section asks for (this is why the caller has already
 * checked that both the section and file alignments are page multiples).
 * If the section is larger in memory than on disk - a .bss-style section,
 * or the zero-filled tail of .data - the difference is provided as
 * anonymous zero pages, and the partial last page of file data is zeroed
 * from its end to the page boundary.
 */
static int pe_map_section(struct file *file, const struct section_header *s,
			  const struct pe_load_info *pi)
{
	unsigned long vaddr, vsize, filesz, mapped, prot, addr;
	unsigned long raw_off = s->data_addr;

	/* A section with no in-memory footprint (SizeOfImage padding). */
	vsize = s->virtual_size;
	if (!vsize)
		return 0;

	vaddr = pi->image_base + s->virtual_address;

	/* The section must lie page-aligned, wholly inside the image. */
	if (!IS_ALIGNED((unsigned long)s->virtual_address, PAGE_SIZE))
		return -ENOEXEC;
	if (s->virtual_address >= pi->image_size ||
	    vsize > pi->image_size - s->virtual_address)
		return -ENOEXEC;

	/* Only as many bytes as actually exist on disk are file-backed. */
	filesz = min_t(unsigned long, s->raw_data_size, vsize);
	prot = pe_section_prot(s->flags);

	if (filesz && raw_off) {
		if (!IS_ALIGNED(raw_off, PAGE_SIZE))
			return -ENOEXEC;

		mapped = PAGE_ALIGN(filesz);
		addr = vm_mmap(file, vaddr, mapped, prot,
			       MAP_PRIVATE | MAP_FIXED, raw_off);
		if (IS_ERR_VALUE(addr))
			return (int)addr;

		/*
		 * Zero the slack between the end of the file data and the end
		 * of its last page, so initialised data does not leak the
		 * bytes that followed it in the file.  Only possible when the
		 * section is writable; a read-only section has no such slack
		 * in practice.
		 */
		if (filesz < mapped && (prot & PROT_WRITE)) {
			if (clear_user((void __user *)(vaddr + filesz),
				       mapped - filesz))
				return -EFAULT;
		}
	} else {
		mapped = 0;
	}

	/* Anonymous zero pages for the in-memory tail beyond the file data. */
	if (vsize > mapped) {
		unsigned long bss = vaddr + mapped;
		unsigned long len = PAGE_ALIGN(vaddr + vsize) - bss;

		addr = vm_mmap(NULL, bss, len, prot,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, 0);
		if (IS_ERR_VALUE(addr))
			return (int)addr;
	}

	return 0;
}

static int load_pe_binary(struct linux_binprm *bprm)
{
	struct file *file = bprm->file;
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;
	struct section_header *sections = NULL;
	struct pt_regs *regs = current_pt_regs();
	struct mm_struct *mm;
	unsigned long reserve, sp;
	u32 peaddr;
	int retval, i;

	/* The MZ header is already in bprm->buf; find and read the PE header. */
	retval = -ENOEXEC;
	if (((struct mz_hdr *)bprm->buf)->magic != IMAGE_DOS_SIGNATURE)
		goto out;
	peaddr = ((struct mz_hdr *)bprm->buf)->peaddr;
	if (peaddr < sizeof(struct mz_hdr) || peaddr > PE_MAX_HEADER_OFFSET)
		goto out;

	if (!can_mmap_file(file))
		goto out;

	retval = pe_read_exact(file, &pe, sizeof(pe), peaddr);
	if (retval)
		goto out;
	retval = pe_read_exact(file, &opt, sizeof(opt),
			       (loff_t)peaddr + sizeof(pe));
	if (retval)
		goto out;

	retval = pe_parse_headers(bprm->buf, BINPRM_BUF_SIZE, &pe, &opt, &pi);
	if (retval)
		goto out;

	/* Slurp the section table now, while we can still fail cleanly. */
	sections = kvmalloc_array(pi.nsections, sizeof(*sections), GFP_KERNEL);
	if (!sections) {
		retval = -ENOMEM;
		goto out;
	}
	retval = pe_read_exact(file, sections,
			       (size_t)pi.nsections * sizeof(*sections),
			       pi.section_table);
	if (retval)
		goto out_free;

	/*
	 * Past this point the old program is gone and errors can no longer be
	 * returned to it - a failure now tears the process down, exactly as
	 * it does in the ELF loader.
	 */
	retval = begin_new_exec(bprm);
	if (retval)
		goto out_free;

	set_personality(PER_LINUX);
	setup_new_exec(bprm);

	/* Windows stacks are non-executable. */
	retval = setup_arg_pages(bprm, STACK_TOP, EXSTACK_DISABLE_X);
	if (retval < 0)
		goto out_free;

	/*
	 * Claim the whole image range at its preferred base up front.  This
	 * both reserves the address space (so a later section mapping cannot
	 * collide with an unrelated allocation) and, via MAP_FIXED_NOREPLACE,
	 * tells us if the preferred base is unavailable.  This loader does not
	 * yet apply base relocations, so an image that cannot load where it
	 * wants is refused rather than loaded at the wrong address.
	 */
	reserve = vm_mmap(NULL, pi.image_base, pi.image_size, PROT_NONE,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0);
	if (IS_ERR_VALUE(reserve) || reserve != pi.image_base) {
		if (!IS_ERR_VALUE(reserve))
			vm_munmap(reserve, pi.image_size);
		retval = IS_ERR_VALUE(reserve) ? (int)reserve : -ENOMEM;
		goto out_free;
	}

	/* Map the headers read-only at the image base, as Windows does. */
	if (pi.header_size) {
		unsigned long hlen = PAGE_ALIGN(pi.header_size);
		unsigned long addr = vm_mmap(file, pi.image_base, hlen, PROT_READ,
					     MAP_PRIVATE | MAP_FIXED, 0);

		if (IS_ERR_VALUE(addr)) {
			retval = (int)addr;
			goto out_free;
		}
	}

	/* Map each section at its virtual address with its own protection. */
	for (i = 0; i < pi.nsections; i++) {
		retval = pe_map_section(file, &sections[i], &pi);
		if (retval)
			goto out_free;
	}

	kvfree(sections);
	sections = NULL;

	/* Record the image layout for /proc, ps and core dumps. */
	mm = current->mm;
	mm->start_code = pi.image_base + pi.code_base;
	mm->end_code = pi.image_base + pi.image_size;
	mm->start_data = pi.image_base;
	mm->end_data = pi.image_base + pi.image_size;
	mm->start_stack = bprm->p;
	mm->start_brk = mm->brk = PAGE_ALIGN(pi.image_base + pi.image_size);

	set_binfmt(&pe_format);
	finalize_exec(bprm);

	/*
	 * Hand control to the entry point on a 16-byte-aligned stack.  A full
	 * Windows startup (RtlUserThreadStart, the PEB/TEB and the process
	 * parameter block) is future work; a freestanding native PE only
	 * needs a valid, aligned stack, which is what it gets here.
	 */
	sp = bprm->p & ~15UL;
	start_thread(regs, pi.image_base + pi.entry, sp);
	return 0;

out_free:
	kvfree(sections);
out:
	return retval;
}

static int __init init_pe_binfmt(void)
{
	register_binfmt(&pe_format);
	return 0;
}

static void __exit exit_pe_binfmt(void)
{
	unregister_binfmt(&pe_format);
}

core_initcall(init_pe_binfmt);
module_exit(exit_pe_binfmt);

#ifdef CONFIG_BINFMT_PE_KUNIT_TEST
#include "tests/binfmt_pe_kunit.c"
#endif

MODULE_DESCRIPTION("Loader for PE/COFF (Windows) executables");
MODULE_LICENSE("GPL");
