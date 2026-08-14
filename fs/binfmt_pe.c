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
 * the uninitialised tail (.bss), applying base relocations when the image
 * cannot get its preferred address, and starting the thread at the entry
 * point.
 *
 * When CONFIG_NT_FS_PERSONALITY is on it then turns that image into an NT
 * process (see pe_setup_nt_process()): the task enters NT syscall mode so
 * its `syscall` instructions dispatch through the NT service table rather
 * than the Linux one, it resolves pathnames by NT rules, and it starts with
 * a minimal TEB/PEB laid out in memory and %gs pointing at the TEB - which
 * is what a freestanding native PE needs to reach the kernel the way every
 * real Windows binary does, through NT services rather than Linux ones.
 *
 * It deliberately stops there.  A real Windows program additionally needs
 *
 *   - its imports resolved against ntdll/kernel32 and friends,
 *   - the full RtlUserThreadStart entry protocol and the process-parameter
 *     block the PEB points at (this loader lays out only the handful of
 *     TEB/PEB fields ntdll and the CRT read to find themselves).
 *
 * Neither is needed to *load and start* the image, so neither lives here.
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
#include <linux/log2.h>
#include <linux/pe.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/nt_syscall.h>
#include <linux/nt_personality.h>
#ifdef CONFIG_NT_FS_PERSONALITY
#include <asm/prctl.h>		/* ARCH_SET_GS */
#ifdef CONFIG_UML
#include <asm/ptrace.h>		/* arch_prctl() */
#else
#include <asm/proto.h>		/* do_arch_prctl_64() */
#endif
#endif

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
#define PE_MIN_FILE_ALIGN	512		/* smallest FileAlignment PE allows */
#define PE_MAX_FILE_ALIGN	65536		/* largest FileAlignment PE allows */

/* Data directory index of the base relocation table. */
#define PE_DIR_BASERELOC	5

/*
 * Base relocation types.  For AMD64 only two ever occur: ABSOLUTE, which is
 * padding and does nothing, and DIR64, a 64-bit fixup.  Anything else in an
 * AMD64 image is unexpected and refused.
 */
#define PE_REL_ABSOLUTE		0
#define PE_REL_DIR64		10

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
	bool relocs_stripped;		/* image carries no base relocations */
	loff_t section_table;		/* file offset of the section table */
	u32 reloc_rva;			/* base relocation table, relative to base */
	u32 reloc_size;			/* its size in bytes (0 if none) */
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
	 * its own protection.  The on-disk file alignment is only bounded by
	 * what PE itself allows: a section whose file data happens to start
	 * on a page boundary is mapped straight from the file; one that does
	 * not (the old 512-byte alignment) is copied in instead, so both are
	 * fine here.  PE also requires the memory alignment to be at least
	 * the file alignment.
	 */
	if (section_align < PAGE_SIZE || !IS_ALIGNED(section_align, PAGE_SIZE))
		return -ENOEXEC;
	if (file_align < PE_MIN_FILE_ALIGN || file_align > PE_MAX_FILE_ALIGN ||
	    !is_power_of_2(file_align))
		return -ENOEXEC;
	if (section_align < file_align)
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
	out->relocs_stripped = pe->flags & IMAGE_FILE_RELOCS_STRIPPED;
	out->section_table = table;
	out->reloc_rva = 0;
	out->reloc_size = 0;
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
 * Write into the image being loaded, even where the destination page is
 * mapped read-only or execute-only.
 *
 * Two things need this: copying a section's file data into an
 * anonymous mapping that already carries its final protection, and
 * applying a base relocation to a fixup that may sit in read-only .text or
 * .rdata.  FOLL_FORCE is the mechanism ptrace uses to poke read-only text;
 * on a private mapping it breaks copy-on-write and leaves the page's
 * protection unchanged, so the section stays W^X once the loader is done.
 */
static int pe_write_image(unsigned long uaddr, const void *buf, size_t len)
{
	int n = access_process_vm(current, uaddr, (void *)buf, len,
				  FOLL_WRITE | FOLL_FORCE);

	return (size_t)n == len ? 0 : -EFAULT;
}

/* Read @len bytes back out of the mapped image at @uaddr. */
static int pe_read_image(unsigned long uaddr, void *buf, size_t len)
{
	int n = access_process_vm(current, uaddr, buf, len, 0);

	return (size_t)n == len ? 0 : -EFAULT;
}

/*
 * Copy @len bytes of @file at @off into the mapped image at @uaddr, through
 * a page-sized bounce buffer.  Used for sections whose file data does not
 * start on a page boundary and so cannot be mapped straight from the file.
 */
static int pe_copy_from_file(struct file *file, loff_t off,
			     unsigned long uaddr, size_t len)
{
	void *page;
	int err = 0;

	page = (void *)__get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	while (len) {
		size_t chunk = min(len, PAGE_SIZE);
		ssize_t n = kernel_read(file, page, chunk, &off);

		if (n <= 0) {
			err = n < 0 ? n : -ENOEXEC;
			break;
		}
		err = pe_write_image(uaddr, page, n);
		if (err)
			break;
		uaddr += n;
		len -= n;
	}

	free_page((unsigned long)page);
	return err;
}

/*
 * Map one section into the new address space at @load_base.
 *
 * A section whose file data begins on a page boundary is mapped straight
 * from the file with its final protection, copy-on-write - the cheap path.
 * A section whose file data does not (an image built with the old 512-byte
 * file alignment), or one with no file data at all (.bss), is backed by
 * anonymous memory with its final protection and the file bytes are copied
 * in.  Either way the in-memory tail beyond the file data is zero.
 */
static int pe_map_section(struct file *file, const struct section_header *s,
			  const struct pe_load_info *pi, unsigned long load_base)
{
	unsigned long vaddr, vsize, filesz, prot, addr;
	unsigned long raw_off = s->data_addr;

	/* A section with no in-memory footprint (SizeOfImage padding). */
	vsize = s->virtual_size;
	if (!vsize)
		return 0;

	/* The section must lie page-aligned, wholly inside the image. */
	if (!IS_ALIGNED((unsigned long)s->virtual_address, PAGE_SIZE))
		return -ENOEXEC;
	if (s->virtual_address >= pi->image_size ||
	    vsize > pi->image_size - s->virtual_address)
		return -ENOEXEC;

	vaddr = load_base + s->virtual_address;
	prot = pe_section_prot(s->flags);

	/* File data exists only when there is a file pointer to it. */
	filesz = raw_off ? min_t(unsigned long, s->raw_data_size, vsize) : 0;

	if (filesz && IS_ALIGNED(raw_off, PAGE_SIZE)) {
		unsigned long mapped = PAGE_ALIGN(filesz);

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

		/* Anonymous zero pages for the in-memory tail (.bss). */
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

	/*
	 * Copy-in path: back the whole section with anonymous zero memory at
	 * its final protection, then copy the file data (if any) in.  The
	 * FOLL_FORCE copy writes even into a read-only or exec-only section,
	 * leaving its protection intact.
	 */
	addr = vm_mmap(NULL, vaddr, PAGE_ALIGN(vsize), prot,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, 0);
	if (IS_ERR_VALUE(addr))
		return (int)addr;

	if (filesz)
		return pe_copy_from_file(file, raw_off, vaddr, filesz);

	return 0;
}

/*
 * Apply the base relocation table to an image that was loaded somewhere
 * other than its preferred address.
 *
 * The table (the .reloc section) is a series of blocks, each fixing up one
 * 4 KiB page: a header of the page's RVA and the block's byte length,
 * followed by 16-bit entries of a 4-bit type and a 12-bit offset into the
 * page.  For AMD64 the only meaningful type is DIR64 - add @delta to a
 * 64-bit pointer - alongside ABSOLUTE padding entries that do nothing.
 * Any other type is refused rather than misapplied.
 *
 * The table is read back out of the image we just mapped; each fixup is a
 * read-modify-write through pe_read_image()/pe_write_image(), which reach
 * even the read-only sections a relocation can legitimately land in.
 */
static int pe_apply_relocations(const struct pe_load_info *pi,
				unsigned long load_base, unsigned long delta)
{
	unsigned char *table;
	u32 pos = 0;
	int err;

	if (!pi->reloc_size)
		return 0;

	table = kvmalloc(pi->reloc_size, GFP_KERNEL);
	if (!table)
		return -ENOMEM;

	err = pe_read_image(load_base + pi->reloc_rva, table, pi->reloc_size);
	if (err)
		goto out;

	while (pos + 8 <= pi->reloc_size) {
		u32 page_rva = get_unaligned_le32(table + pos);
		u32 block_size = get_unaligned_le32(table + pos + 4);
		u32 nentries, i;

		if (block_size < 8 || block_size > pi->reloc_size - pos) {
			err = -ENOEXEC;
			goto out;
		}
		if (page_rva >= pi->image_size) {
			err = -ENOEXEC;
			goto out;
		}

		nentries = (block_size - 8) / 2;
		for (i = 0; i < nentries; i++) {
			u16 e = get_unaligned_le16(table + pos + 8 + i * 2);
			u32 type = e >> 12;
			u32 rva = page_rva + (e & 0xfff);
			u64 val;

			if (type == PE_REL_ABSOLUTE)
				continue;
			if (type != PE_REL_DIR64) {
				err = -ENOEXEC;
				goto out;
			}
			if (rva > pi->image_size - sizeof(u64)) {
				err = -ENOEXEC;
				goto out;
			}

			err = pe_read_image(load_base + rva, &val, sizeof(val));
			if (err)
				goto out;
			val += delta;
			err = pe_write_image(load_base + rva, &val, sizeof(val));
			if (err)
				goto out;
		}
		pos += block_size;
	}
out:
	kvfree(table);
	return err;
}

#ifdef CONFIG_NT_FS_PERSONALITY

/*
 * 64-bit NT thread environment block, as ntdll and the MSVC CRT read it
 * through %gs.  These are the real Windows x64 field offsets: NT_TIB and the
 * TEB from winnt.h, the PEB from winternl.h.  A native thread finds its own
 * TEB at gs:[0x30] (NT_TIB.Self), its stack bounds at gs:[0x08]/gs:[0x10],
 * and its PEB at gs:[0x60]; the PEB in turn names the image base at PEB+0x10.
 *
 * We lay out a minimal TEB and PEB in a single user page and leave every
 * other field zero, which is a valid (empty) value for all of them.  The
 * PEB is placed well clear of the TEB fields we populate so the two never
 * overlap; both assertions below hold that layout.
 */
#define NT_TEB_STACK_BASE	0x08	/* NT_TIB.StackBase  - top of the stack */
#define NT_TEB_STACK_LIMIT	0x10	/* NT_TIB.StackLimit - bottom of the stack */
#define NT_TEB_SELF		0x30	/* NT_TIB.Self       - the TEB itself */
#define NT_TEB_PEB		0x60	/* TEB.ProcessEnvironmentBlock */
#define NT_PEB_OFFSET		0x800	/* where the PEB sits within the page */
#define NT_PEB_IMAGE_BASE	0x10	/* PEB.ImageBaseAddress */

static_assert(NT_TEB_PEB + sizeof(u64) < NT_PEB_OFFSET,
	      "the PEB overlaps the populated TEB fields");
static_assert(NT_PEB_OFFSET + NT_PEB_IMAGE_BASE + sizeof(u64) <= PAGE_SIZE,
	      "the TEB and PEB do not fit in one page");

/*
 * Point this task's %gs at its TEB.
 *
 * On native x86-64 the user GS base is installed with do_arch_prctl_64(),
 * which for task == current loads a null GS selector and writes the inactive
 * (user) GS-base MSR that becomes active on return to user mode.  UML has no
 * MSR to write; it records the guest GS base in the task's ptrace register
 * set (arch/x86/um/syscalls_64.c) and restores it into the host process
 * whenever the guest runs - via PTRACE_SETREGS, or the seccomp stub's own
 * arch_prctl (arch/x86/um/os-Linux/mcontext.c, arch/x86/um/shared/sysdep/
 * stub_64.h) - so the native PE really does see gs:[...] resolve against the
 * TEB there too.  The TEB address came from vm_mmap() in this mm, so it is a
 * valid user address neither setter can reject.
 */
static int pe_set_gs_base(unsigned long teb)
{
#ifdef CONFIG_UML
	return arch_prctl(current, ARCH_SET_GS, (unsigned long __user *)teb);
#else
	return do_arch_prctl_64(current, ARCH_SET_GS, teb);
#endif
}

/*
 * Turn the just-mapped image into an NT process and lay out its thread
 * environment.  Enters NT syscall mode, sets the NT personality so pathnames
 * resolve by Windows rules, allocates one user page for the TEB and PEB, and
 * fills in the minimal set of fields a native PE reads to find itself.  The
 * TEB address is returned in @teb_out for pe_set_gs_base(), which must run
 * after start_thread() (see the call site).
 *
 * Runs past the point of no return, so a failure returns a negative errno
 * and the caller tears the process down, exactly as the mapping failures do.
 */
static int pe_setup_nt_process(unsigned long load_base, unsigned long sp,
			       unsigned long *teb_out)
{
	unsigned long teb, peb, stack_base, stack_limit;
	int retval;

	/* Dispatch this task's `syscall` through the NT service table. */
	nt_syscall_mode_set(current);

	/* Resolve its pathnames the way Windows does (case-insensitive). */
	retval = nt_personality_set(NT_PERSONALITY_ENABLED |
				    NT_PERSONALITY_CASE_INSENSITIVE);
	if (retval)
		return retval;

	/* One anonymous, zero-filled page holds both the TEB and the PEB. */
	teb = vm_mmap(NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (IS_ERR_VALUE(teb))
		return (int)teb;
	peb = teb + NT_PEB_OFFSET;

	/*
	 * The stack was set up growing down from STACK_TOP; NT_TIB records its
	 * high bound as StackBase and the lowest committed page as StackLimit.
	 */
	stack_base = STACK_TOP;
	stack_limit = sp & PAGE_MASK;

	/*
	 * The page is all zero already, so only the non-zero fields are
	 * written.  put_user() faults the page in; it can fail only on an
	 * unmapped address, which this freshly mapped page is not.
	 */
	if (put_user(stack_base,
		     (unsigned long __user *)(teb + NT_TEB_STACK_BASE)) ||
	    put_user(stack_limit,
		     (unsigned long __user *)(teb + NT_TEB_STACK_LIMIT)) ||
	    put_user(teb, (unsigned long __user *)(teb + NT_TEB_SELF)) ||
	    put_user(peb, (unsigned long __user *)(teb + NT_TEB_PEB)) ||
	    put_user(load_base,
		     (unsigned long __user *)(peb + NT_PEB_IMAGE_BASE)))
		return -EFAULT;

	*teb_out = teb;
	return 0;
}

#endif /* CONFIG_NT_FS_PERSONALITY */

static int load_pe_binary(struct linux_binprm *bprm)
{
	struct file *file = bprm->file;
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;
	struct section_header *sections = NULL;
	struct pt_regs *regs = current_pt_regs();
	struct mm_struct *mm;
	unsigned long reserve, load_base, delta, sp;
#ifdef CONFIG_NT_FS_PERSONALITY
	unsigned long teb;
#endif
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

	/*
	 * The base relocation table is one of the data directories that follow
	 * the optional header.  Read its entry if the image has that many; it
	 * decides whether an image that cannot get its preferred base can be
	 * relocated or has to be refused.
	 */
	if (opt.data_dirs > PE_DIR_BASERELOC) {
		struct data_dirent reloc_dd;
		loff_t dd_off = (loff_t)peaddr + sizeof(pe) + sizeof(opt) +
				PE_DIR_BASERELOC * sizeof(reloc_dd);

		retval = pe_read_exact(file, &reloc_dd, sizeof(reloc_dd), dd_off);
		if (retval)
			goto out;
		pi.reloc_rva = reloc_dd.virtual_address;
		pi.reloc_size = reloc_dd.size;

		retval = -ENOEXEC;
		if (pi.reloc_size &&
		    (pi.reloc_rva >= pi.image_size ||
		     pi.reloc_size > pi.image_size - pi.reloc_rva))
			goto out;
	}

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
	if (!(current->personality & ADDR_NO_RANDOMIZE) &&
	    READ_ONCE(randomize_va_space))
		current->flags |= PF_RANDOMIZE;

	setup_new_exec(bprm);

	/* Windows stacks are non-executable. */
	retval = setup_arg_pages(bprm, STACK_TOP, EXSTACK_DISABLE_X);
	if (retval < 0)
		goto out_free;

	/*
	 * Choose where the image goes.
	 *
	 * An image that carries base relocations can run anywhere, so when
	 * address-space randomisation is in effect it is loaded at a
	 * kernel-chosen base and relocated to it, exactly as a PIE ELF is -
	 * the preferred base is only a hint.  Otherwise the preferred base is
	 * tried first with MAP_FIXED_NOREPLACE (which both reserves the whole
	 * range so a later section mapping cannot collide with an unrelated
	 * allocation, and reports whether the base is already taken); if it is
	 * taken, a relocatable image falls back to a kernel-chosen base and a
	 * non-relocatable one is refused rather than loaded at the wrong
	 * address.
	 */
	if (!pi.relocs_stripped && pi.reloc_size &&
	    (current->flags & PF_RANDOMIZE)) {
		reserve = vm_mmap(NULL, 0, pi.image_size, PROT_NONE,
				  MAP_PRIVATE | MAP_ANONYMOUS, 0);
		if (IS_ERR_VALUE(reserve)) {
			retval = (int)reserve;
			goto out_free;
		}
		load_base = reserve;
		delta = load_base - pi.image_base;
	} else {
		reserve = vm_mmap(NULL, pi.image_base, pi.image_size, PROT_NONE,
				  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
				  0);
		if (!IS_ERR_VALUE(reserve) && reserve == pi.image_base) {
			load_base = pi.image_base;
			delta = 0;
		} else {
			if (!IS_ERR_VALUE(reserve))
				vm_munmap(reserve, pi.image_size);
			if (pi.relocs_stripped || !pi.reloc_size) {
				retval = -ENOMEM;
				goto out_free;
			}
			reserve = vm_mmap(NULL, 0, pi.image_size, PROT_NONE,
					  MAP_PRIVATE | MAP_ANONYMOUS, 0);
			if (IS_ERR_VALUE(reserve)) {
				retval = (int)reserve;
				goto out_free;
			}
			load_base = reserve;
			delta = load_base - pi.image_base;
		}
	}

	/* Map the headers read-only at the load base, as Windows does. */
	if (pi.header_size) {
		unsigned long hlen = PAGE_ALIGN(pi.header_size);
		unsigned long addr = vm_mmap(file, load_base, hlen, PROT_READ,
					     MAP_PRIVATE | MAP_FIXED, 0);

		if (IS_ERR_VALUE(addr)) {
			retval = (int)addr;
			goto out_free;
		}
	}

	/* Map each section at its virtual address with its own protection. */
	for (i = 0; i < pi.nsections; i++) {
		retval = pe_map_section(file, &sections[i], &pi, load_base);
		if (retval)
			goto out_free;
	}

	/* Fix up the image if it did not land at its preferred base. */
	if (delta) {
		retval = pe_apply_relocations(&pi, load_base, delta);
		if (retval)
			goto out_free;
	}

	kvfree(sections);
	sections = NULL;

	/* Record the image layout for /proc, ps and core dumps. */
	mm = current->mm;
	mm->start_code = load_base + pi.code_base;
	mm->end_code = load_base + pi.image_size;
	mm->start_data = load_base;
	mm->end_data = load_base + pi.image_size;
	mm->start_stack = bprm->p;
	mm->start_brk = mm->brk = PAGE_ALIGN(load_base + pi.image_size);

	sp = bprm->p & ~15UL;

#ifdef CONFIG_NT_FS_PERSONALITY
	/*
	 * Turn the loaded image into an NT process before it runs: enter NT
	 * syscall mode, set the NT personality, and lay out its TEB/PEB.  Every
	 * real Windows PE reaches the kernel only through the NT services in
	 * ntdll, so from its first instruction this task must speak the NT ABI,
	 * not the Linux one.  Done here, once the address space is fully built
	 * and past the point of no return - a failure now tears the process
	 * down, the same as the mapping failures above.
	 */
	retval = pe_setup_nt_process(load_base, sp, &teb);
	if (retval)
		goto out_free;
#endif

	set_binfmt(&pe_format);
	finalize_exec(bprm);

	/*
	 * Hand control to the entry point on a 16-byte-aligned stack.  A full
	 * Windows startup (the RtlUserThreadStart entry protocol and the
	 * process-parameter block the PEB points at) is future work; a
	 * freestanding native PE needs only a valid, aligned stack, its TEB/PEB
	 * (laid out above), and %gs pointing at the TEB (set just below).
	 */
	start_thread(regs, load_base + pi.entry, sp);

#ifdef CONFIG_NT_FS_PERSONALITY
	/*
	 * Install the TEB as this thread's GS base.  It has to happen after
	 * start_thread(), which resets the GS base while giving the new thread
	 * a clean segment state; doing it here makes gs:[0x30] resolve to the
	 * TEB from the PE's first instruction.  The address is a valid user
	 * mapping, so this cannot fail; a failure all the same tears the
	 * process down, since control has already left the old program.
	 */
	retval = pe_set_gs_base(teb);
	if (retval)
		goto out_free;
#endif
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
