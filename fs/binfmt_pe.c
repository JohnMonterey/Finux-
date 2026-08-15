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
#include <linux/file.h>
#include <linux/binfmts.h>
#include <linux/personality.h>
#include <linux/ptrace.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/log2.h>
#include <linux/string.h>
#include <linux/pe.h>
#include <linux/cred.h>
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

/* Data directory indices this loader reads. */
#define PE_DIR_EXPORT		0
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
 * __pe_parse_headers - validate raw PE headers and digest them
 * @mz:		the first bytes of the file (at least the MZ header)
 * @mz_len:	how many bytes @mz points at (bprm->buf is BINPRM_BUF_SIZE)
 * @pe:		the PE/COFF header, already read from the file at mz->peaddr
 * @opt:	the PE32+ optional header, read immediately after @pe
 * @allow_dll:	accept a DLL (IMAGE_FILE_DLL, possibly no entry point) instead
 *		of an executable; the EXE path passes false, the dynamic
 *		linker's DLL path passes true
 * @out:	filled in on success
 *
 * Returns 0 and populates @out, or -ENOEXEC if the file is not a PE image
 * this loader can handle.  This is where every "we only support ..."
 * decision is made, so it is deliberately strict and side-effect free.
 * pe_parse_headers() and pe_parse_dll_headers() are the two entry points.
 */
static int __pe_parse_headers(const void *mz, size_t mz_len,
			      const struct pe_hdr *pe,
			      const struct pe32plus_opt_hdr *opt,
			      bool allow_dll, struct pe_load_info *out)
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

	/* "PE\0\0", AMD64, an executable image. */
	if (pe->magic != IMAGE_NT_SIGNATURE)
		return -ENOEXEC;
	if (pe->machine != IMAGE_FILE_MACHINE_AMD64)
		return -ENOEXEC;
	if (!(pe->flags & IMAGE_FILE_EXECUTABLE_IMAGE))
		return -ENOEXEC;
	/*
	 * A DLL is a PE image but not a program execve() starts.  The EXE path
	 * (!allow_dll) refuses one here - the rejection the KUnit suite locks
	 * in - while the DLL path requires the flag to be set instead, so an
	 * executable can never be loaded as a dependency by mistake.
	 */
	if (allow_dll) {
		if (!(pe->flags & IMAGE_FILE_DLL))
			return -ENOEXEC;
	} else if (pe->flags & IMAGE_FILE_DLL) {
		return -ENOEXEC;
	}

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

	/*
	 * The entry point and the headers must fall inside the image.  An
	 * executable must have an entry point; a DLL need not - its DllMain
	 * entry, which this loader does not call, may be absent - so a zero
	 * entry is tolerated only on the DLL path.  A nonzero entry is
	 * bounds-checked either way.
	 */
	if (entry >= image_size)
		return -ENOEXEC;
	if (!allow_dll && !entry)
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

/*
 * Validate the headers of a main executable.  Rejects a DLL and requires an
 * entry point; this is the entry point the EXE loader and the KUnit suite use.
 */
static int pe_parse_headers(const void *mz, size_t mz_len,
			    const struct pe_hdr *pe,
			    const struct pe32plus_opt_hdr *opt,
			    struct pe_load_info *out)
{
	return __pe_parse_headers(mz, mz_len, pe, opt, false, out);
}

/*
 * ===========================================================================
 * PE dynamic-linking parsers
 * ===========================================================================
 *
 * Everything from here to the end of this banner turns the export and import
 * data directories of a PE image into something an in-kernel dynamic linker
 * can act on.  Like pe_parse_headers() above it is pure PE-format logic: it
 * does no I/O and touches no task state, reaching the image only through a
 * struct pe_image_reader.  That keeps it unit-testable against a flat buffer
 * and lets the loader later drive the same code over a mapped image with a
 * copy_from_user-backed reader, without a line of this changing.
 *
 * Every access to the image goes through pe_reader_read(), which bounds-checks
 * the RVA and length against the image size before the backing reader ever
 * runs.  A malformed directory can therefore misdescribe the image, but can
 * never make the parser read outside it.
 */

/*
 * Sanity caps on the tables below.  These are not format limits - PE permits
 * more - but bounds that keep a hostile or corrupt directory from making the
 * parser iterate or copy without limit.  They sit far above anything a real
 * image needs (ntdll exports ~2500 symbols; nothing imports from thousands of
 * DLLs).
 */
#define PE_MAX_EXPORTS		(64 * 1024)	/* export ordinals are 16-bit */
#define PE_MAX_IMPORT_DLLS	4096		/* import descriptors */
#define PE_MAX_IMPORTS_PER_DLL	(64 * 1024)	/* thunks in one descriptor */
#define PE_MAX_SYM_NAME		1024		/* symbol-name comparison cap */
#define PE_MAX_DLL_NAME		256		/* imported DLL name cap */

/*
 * An abstract, bounds-checked view of a PE image addressed by RVA.  @read
 * copies @len bytes at @rva into @buf and returns 0 or a negative errno; it
 * may assume [@rva, @rva+@len) already lies within @image_size, because
 * pe_reader_read() checks that before calling it.  The flat-buffer reader
 * below backs the unit tests; the loader supplies a copy_from_user-backed one.
 */
struct pe_image_reader {
	int (*read)(void *ctx, u32 rva, void *buf, u32 len);
	void *ctx;
	u32 image_size;		/* RVAs must satisfy rva + len <= image_size */
};

/*
 * The single choke point for every image access.  Rejects a length larger
 * than the image and any [rva, rva+len) that would run past the end, with the
 * subtraction ordered so it can never wrap, then defers to the backing reader.
 */
static int pe_reader_read(const struct pe_image_reader *r, u32 rva,
			  void *buf, u32 len)
{
	if (len > r->image_size || rva > r->image_size - len)
		return -EFAULT;
	return r->read(r->ctx, rva, buf, len);
}

/* A pe_image_reader backed by a flat in-memory buffer, used by the tests. */
struct pe_flat_image {
	const void *base;
	u32 size;
};

static int pe_flat_read(void *ctx, u32 rva, void *buf, u32 len)
{
	const struct pe_flat_image *img = ctx;

	/* pe_reader_read() has already bounded rva/len against image_size. */
	memcpy(buf, (const u8 *)img->base + rva, len);
	return 0;
}

static void __maybe_unused pe_flat_image_reader(struct pe_image_reader *r,
						struct pe_flat_image *img,
						const void *base, u32 size)
{
	img->base = base;
	img->size = size;
	r->read = pe_flat_read;
	r->ctx = img;
	r->image_size = size;
}

/*
 * Copy a NUL-terminated string from @rva into @buf (always NUL-terminated),
 * reading one byte at a time so a string that runs off the end of the image is
 * refused by pe_reader_read() rather than read past it.  Returns 0 on success,
 * -ENAMETOOLONG if no terminator appears within @bufsz, or the reader's error.
 */
static int pe_read_cstr(const struct pe_image_reader *r, u32 rva,
			char *buf, u32 bufsz)
{
	u32 i;

	if (!bufsz)
		return -EINVAL;

	for (i = 0; i < bufsz; i++) {
		char c;
		int err = pe_reader_read(r, rva + i, &c, 1);

		if (err)
			return err;
		buf[i] = c;
		if (!c)
			return 0;
	}
	buf[bufsz - 1] = '\0';
	return -ENAMETOOLONG;
}

/*
 * Compare the NUL-terminated string at @rva against @want, at most @cap bytes.
 * Any read that would leave the image, or a string longer than @cap, counts as
 * "not equal" - a safe miss, never an out-of-bounds read.  (The first byte's
 * read validates @rva, so @rva + @i below cannot wrap for the bytes that
 * follow.)
 */
static bool pe_streq_at(const struct pe_image_reader *r, u32 rva,
			const char *want, u32 cap)
{
	u32 i;

	for (i = 0; i < cap; i++) {
		char c;

		if (pe_reader_read(r, rva + i, &c, 1))
			return false;
		if (c != want[i])
			return false;
		if (!c)
			return true;
	}
	return false;
}

/*
 * The digested export directory: the reader plus the array locations and
 * counts the lookups need.  dir_rva/dir_size bound the directory itself and are
 * kept so a resolved RVA that lands back inside it can be recognised as a
 * forwarder.
 */
struct pe_exports {
	const struct pe_image_reader *reader;
	u32 dir_rva;
	u32 dir_size;
	u32 base;
	u32 num_functions;
	u32 num_names;
	u32 functions;		/* RVA of the u32 function-RVA array */
	u32 names;		/* RVA of the u32 name-RVA array */
	u32 name_ordinals;	/* RVA of the u16 name-ordinal array */
};

/*
 * Digest the export directory at [@dir_rva, @dir_rva+@dir_size) into @out.
 * Validates the fixed directory header and that each of the three parallel
 * arrays lies wholly within the image (the per-entry reads in the lookups are
 * bounds-checked again by the reader).  Returns 0, -ENOENT if the image has no
 * export directory, or -ENOEXEC if the directory is malformed.
 */
static int __maybe_unused pe_parse_exports(const struct pe_image_reader *reader,
					   u32 dir_rva, u32 dir_size,
					   struct pe_exports *out)
{
	struct pe_export_directory dir;
	int err;

	if (!dir_rva || !dir_size)
		return -ENOENT;
	if (dir_size < sizeof(dir))
		return -ENOEXEC;

	err = pe_reader_read(reader, dir_rva, &dir, sizeof(dir));
	if (err)
		return err;

	if (dir.num_functions > PE_MAX_EXPORTS || dir.num_names > PE_MAX_EXPORTS)
		return -ENOEXEC;

	/* Each parallel array must fit inside the image (computed in u64). */
	if ((u64)dir.functions + (u64)dir.num_functions * sizeof(u32) >
	    reader->image_size)
		return -ENOEXEC;
	if ((u64)dir.names + (u64)dir.num_names * sizeof(u32) >
	    reader->image_size)
		return -ENOEXEC;
	if ((u64)dir.name_ordinals + (u64)dir.num_names * sizeof(u16) >
	    reader->image_size)
		return -ENOEXEC;

	out->reader = reader;
	out->dir_rva = dir_rva;
	out->dir_size = dir_size;
	out->base = dir.base;
	out->num_functions = dir.num_functions;
	out->num_names = dir.num_names;
	out->functions = dir.functions;
	out->names = dir.names;
	out->name_ordinals = dir.name_ordinals;
	return 0;
}

/*
 * Map a zero-based index into the function-RVA array to its target RVA,
 * applying the "not resolvable here" rules: an out-of-range index or an empty
 * (zero) slot yields 0, and a target that lands inside the export directory is
 * a forwarder string ("OTHER.dll.Symbol"), which this loader does not chase -
 * it is reported as unresolved (0) too, for phase B to handle separately.
 */
static u32 pe_export_function_rva(const struct pe_exports *e, u32 index)
{
	u32 rva;

	if (index >= e->num_functions)
		return 0;
	if (pe_reader_read(e->reader, e->functions + index * sizeof(u32),
			   &rva, sizeof(rva)))
		return 0;
	if (!rva)
		return 0;
	/* Forwarder: an RVA within the export directory is a string, not code. */
	if (rva >= e->dir_rva && (u64)rva < (u64)e->dir_rva + e->dir_size)
		return 0;
	return rva;
}

/*
 * Resolve an export by name to its RVA, or 0 if the DLL does not export that
 * name (or exports it only as a forwarder).  Walks the name array comparing
 * each name string, then maps the matching slot through the name-ordinal array
 * to a function index.  Linear search; the name comparison length is capped.
 */
static u32 __maybe_unused pe_export_by_name(const struct pe_exports *e,
					    const char *name)
{
	u32 i;

	for (i = 0; i < e->num_names; i++) {
		u32 name_rva;
		u16 index;

		if (pe_reader_read(e->reader, e->names + i * sizeof(u32),
				   &name_rva, sizeof(name_rva)))
			return 0;
		if (!pe_streq_at(e->reader, name_rva, name, PE_MAX_SYM_NAME))
			continue;
		if (pe_reader_read(e->reader, e->name_ordinals + i * sizeof(u16),
				   &index, sizeof(index)))
			return 0;
		return pe_export_function_rva(e, index);
	}
	return 0;
}

/*
 * Resolve an export by ordinal to its RVA, or 0 if the ordinal is out of range
 * or names a forwarder.  The ordinal is biased by the directory's Base to give
 * a zero-based index into the function-RVA array.
 */
static u32 __maybe_unused pe_export_by_ordinal(const struct pe_exports *e,
					       u16 ordinal)
{
	if (ordinal < e->base)
		return 0;
	return pe_export_function_rva(e, ordinal - e->base);
}

/*
 * One import, handed to the pe_parse_imports() callback.  @dll and @name point
 * at buffers valid only for the duration of the call; a callback that needs to
 * keep them must copy.  @iat_slot_rva is FirstThunk + i*8 - the IAT slot the
 * binder will later overwrite with the resolved address.  For an import by
 * ordinal @by_ordinal is true and @ordinal holds it; otherwise @hint and @name
 * describe the import by name.
 */
struct pe_import {
	const char *dll;
	const char *name;	/* NULL when by_ordinal */
	u32 iat_slot_rva;
	u16 ordinal;		/* valid when by_ordinal */
	u16 hint;		/* valid when !by_ordinal */
	bool by_ordinal;
};

/*
 * Callback invoked once per imported symbol.  Returns 0 to continue; a nonzero
 * return stops the walk and becomes pe_parse_imports()'s return value, letting
 * a binder abort on the first symbol it cannot resolve.
 */
typedef int (*pe_import_cb)(void *ctx, const struct pe_import *imp);

/*
 * Walk the import directory, invoking @cb once per imported symbol.
 *
 * The descriptor array at @dir_rva runs until an all-zero terminator (or the
 * PE_MAX_IMPORT_DLLS cap).  For each descriptor the lookup table
 * (OriginalFirstThunk, or FirstThunk when it is zero) is walked until a zero
 * thunk: bit 63 marks an import by ordinal (low 16 bits), otherwise the low 31
 * bits are the RVA of an IMAGE_IMPORT_BY_NAME { hint, name }.  The IAT slot the
 * binder will patch is always FirstThunk + i*8, reported regardless of which
 * table was read.  Returns 0 when every descriptor is consumed, a nonzero
 * callback return, -ENOENT if the image has no import directory, or
 * -ENOEXEC/-EFAULT on a malformed or out-of-range directory.
 */
static int __maybe_unused pe_parse_imports(const struct pe_image_reader *reader,
					   u32 dir_rva, u32 dir_size,
					   pe_import_cb cb, void *ctx)
{
	char dll[PE_MAX_DLL_NAME];
	u32 d;

	if (!dir_rva || !dir_size)
		return -ENOENT;

	for (d = 0; d < PE_MAX_IMPORT_DLLS; d++) {
		struct pe_import_descriptor desc;
		u32 lookup, i;
		int err;

		err = pe_reader_read(reader, dir_rva + d * sizeof(desc),
				     &desc, sizeof(desc));
		if (err)
			return err;

		/* An all-zero descriptor terminates the array. */
		if (!desc.lookup_table && !desc.timestamp &&
		    !desc.forwarder_chain && !desc.name && !desc.address_table)
			return 0;

		/* A real descriptor must name its DLL and have an IAT. */
		if (!desc.name || !desc.address_table)
			return -ENOEXEC;

		err = pe_read_cstr(reader, desc.name, dll, sizeof(dll));
		if (err)
			return err;

		/* OriginalFirstThunk is the lookup table; fall back to the IAT. */
		lookup = desc.lookup_table ? desc.lookup_table :
					     desc.address_table;

		for (i = 0; i < PE_MAX_IMPORTS_PER_DLL; i++) {
			struct pe_import imp = { .dll = dll };
			char name[PE_MAX_SYM_NAME];
			u64 thunk;

			err = pe_reader_read(reader, lookup + i * sizeof(u64),
					     &thunk, sizeof(thunk));
			if (err)
				return err;
			if (!thunk)
				break;		/* end of this DLL's imports */

			/* IAT slot the binder patches: FirstThunk + i*8. */
			if ((u64)desc.address_table + (u64)i * sizeof(u64) +
			    sizeof(u64) > reader->image_size)
				return -ENOEXEC;
			imp.iat_slot_rva = desc.address_table + i * sizeof(u64);

			if (thunk & IMAGE_ORDINAL_FLAG64) {
				imp.by_ordinal = true;
				imp.ordinal = thunk & 0xffff;
			} else {
				u32 name_rva = thunk & 0x7fffffff;
				u16 hint;

				err = pe_reader_read(reader, name_rva,
						     &hint, sizeof(hint));
				if (err)
					return err;
				err = pe_read_cstr(reader, name_rva + sizeof(hint),
						   name, sizeof(name));
				if (err)
					return err;
				imp.hint = hint;
				imp.name = name;
			}

			err = cb(ctx, &imp);
			if (err)
				return err;
		}
		if (i == PE_MAX_IMPORTS_PER_DLL)
			return -ENOEXEC;	/* unterminated thunk table */
	}
	return -ENOEXEC;			/* unterminated descriptor array */
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

/*
 * Map a whole PE image - the main executable or a DLL dependency - into the
 * current task's address space and report where it landed.
 *
 * This is the shared core of image loading, lifted out of load_pe_binary() so
 * a DLL is mapped by exactly the same steps: reserve the SizeOfImage range at
 * a base, map the headers read-only, map each section at its RVA with its own
 * protection (zero-filling the .bss tail), and apply base relocations if the
 * image did not land at its preferred base.
 *
 * Base selection matches what the ELF loader does for a PIE.  @randomize asks
 * for a kernel-chosen base (address-space randomisation): a relocatable image
 * is placed at one and relocated to it.  Otherwise the preferred ImageBase is
 * reserved with MAP_FIXED_NOREPLACE - which both claims the whole range so a
 * later section mapping cannot collide with an unrelated allocation, and
 * reports whether the base is already taken; if it is, a relocatable image
 * falls back to a kernel-chosen base and a non-relocatable one is refused
 * rather than loaded at the wrong address.  A DLL passes @randomize false, so
 * it loads at its preferred base and a RELOCS_STRIPPED DLL whose base is taken
 * is refused (it cannot be moved).
 *
 * On success @load_base_out holds the base and the whole image is mapped.  On
 * any failure the reserved range is torn back down so no partial mapping is
 * left behind, and a negative errno is returned; the caller need not unmap.
 */
static int pe_map_image(struct file *file, const struct pe_load_info *pi,
			const struct section_header *sections, bool randomize,
			unsigned long *load_base_out)
{
	unsigned long reserve, load_base, delta;
	int retval, i;

	if (!pi->relocs_stripped && pi->reloc_size && randomize) {
		reserve = vm_mmap(NULL, 0, pi->image_size, PROT_NONE,
				  MAP_PRIVATE | MAP_ANONYMOUS, 0);
		if (IS_ERR_VALUE(reserve))
			return (int)reserve;
		load_base = reserve;
		delta = load_base - pi->image_base;
	} else {
		reserve = vm_mmap(NULL, pi->image_base, pi->image_size, PROT_NONE,
				  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
				  0);
		if (!IS_ERR_VALUE(reserve) && reserve == pi->image_base) {
			load_base = pi->image_base;
			delta = 0;
		} else {
			if (!IS_ERR_VALUE(reserve))
				vm_munmap(reserve, pi->image_size);
			if (pi->relocs_stripped || !pi->reloc_size)
				return -ENOMEM;
			reserve = vm_mmap(NULL, 0, pi->image_size, PROT_NONE,
					  MAP_PRIVATE | MAP_ANONYMOUS, 0);
			if (IS_ERR_VALUE(reserve))
				return (int)reserve;
			load_base = reserve;
			delta = load_base - pi->image_base;
		}
	}

	/* Map the headers read-only at the load base, as Windows does. */
	if (pi->header_size) {
		unsigned long hlen = PAGE_ALIGN(pi->header_size);
		unsigned long addr = vm_mmap(file, load_base, hlen, PROT_READ,
					     MAP_PRIVATE | MAP_FIXED, 0);

		if (IS_ERR_VALUE(addr)) {
			retval = (int)addr;
			goto out_unmap;
		}
	}

	/* Map each section at its virtual address with its own protection. */
	for (i = 0; i < pi->nsections; i++) {
		retval = pe_map_section(file, &sections[i], pi, load_base);
		if (retval)
			goto out_unmap;
	}

	/* Fix up the image if it did not land at its preferred base. */
	if (delta) {
		retval = pe_apply_relocations(pi, load_base, delta);
		if (retval)
			goto out_unmap;
	}

	*load_base_out = load_base;
	return 0;

out_unmap:
	vm_munmap(load_base, pi->image_size);
	return retval;
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

/*
 * ===========================================================================
 * DLL dependency loading
 * ===========================================================================
 *
 * A native PE reaches the kernel through NT services that live in ntdll.dll:
 * its import table names them and its IAT is patched, at load time, with their
 * addresses inside a loaded ntdll.  nt_load_dll() is the half of that a
 * dynamic linker needs first - find the DLL, map it, and read its export
 * table - so a later binding stage (phase C) can resolve each import against
 * it and write the IAT.  It runs in the exec'ing task's context, so it maps
 * into the process being built and reads the mapped image with copy_from_user.
 */

/*
 * The one directory the DLL search looks in.
 *
 * A real Windows loader walks an ordered search path (the application
 * directory, the system directory, the %PATH%, and so on).  This first cut
 * resolves a dependency only from the system directory - C:\Windows\System32,
 * where ntdll and the other NT DLLs live - so a name that is not there fails
 * rather than being searched for elsewhere.  Widening the search to more
 * directories is future work, called out here so this is not mistaken for a
 * complete loader search order.  The name is joined onto this prefix and
 * resolved by the ordinary NT path rules (case-insensitive, over the C:
 * volume), so it is subject to the same volume and mount setup as any other
 * NT path.
 */
#define NT_DLL_SEARCH_DIR	"C:\\Windows\\System32\\"

/*
 * A copy_from_user-backed pe_image_reader over an image already mapped into
 * the current task.  The dynamic-linking parsers reach an image only through a
 * struct pe_image_reader; because the loader runs in the exec'ing task's
 * context, copy_from_user reads the mapped DLL directly.  This is the reader
 * Phase A anticipated phase B would supply.  pe_reader_read() has already
 * bounded [rva, rva+len) against image_size before this is called, so the
 * address computed here cannot run past the mapped image.
 */
struct pe_user_image {
	unsigned long base;	/* image base as a user address */
	u32 image_size;
};

static int pe_user_read(void *ctx, u32 rva, void *buf, u32 len)
{
	const struct pe_user_image *img = ctx;

	if (copy_from_user(buf, (const void __user *)(img->base + rva), len))
		return -EFAULT;
	return 0;
}

/**
 * struct nt_loaded_dll - a DLL mapped into the process for import binding
 * @base:	user address the DLL is mapped at (its ImageBase, unless it was
 *		relocated)
 * @image_size:	its SizeOfImage
 * @entry:	DllMain RVA, or 0; recorded but never called (no DllMain support)
 * @img:	the copy_from_user reader context over the mapped image
 * @reader:	the pe_image_reader @exports reads through; points at @img
 * @exports:	the digested export directory; points at @reader
 *
 * This object owns the reader and reader context that @exports refers to, so
 * it must outlive any use of @exports.  For import binding that is the life of
 * the process: the IAT slots the binder writes point straight into the
 * mapping, so both the mapping and this bookkeeping have to persist.  There is
 * no unload path yet - the mapping and the struct go away with the mm at
 * process exit.  Phase C resolves an export with
 * pe_export_by_name(&dll->exports, "Nt..."), which returns an RVA; the address
 * to store in an IAT slot is @base + that RVA, written with pe_write_image().
 */
struct nt_loaded_dll {
	unsigned long base;
	unsigned long image_size;
	unsigned long entry;
	struct pe_user_image img;
	struct pe_image_reader reader;
	struct pe_exports exports;
};

/*
 * Validate the headers of a DLL dependency.  Permits (indeed requires)
 * IMAGE_FILE_DLL and tolerates a zero entry point; otherwise identical to the
 * executable validation.
 */
static int pe_parse_dll_headers(const void *mz, size_t mz_len,
				const struct pe_hdr *pe,
				const struct pe32plus_opt_hdr *opt,
				struct pe_load_info *out)
{
	return __pe_parse_headers(mz, mz_len, pe, opt, true, out);
}

/*
 * Load a DLL dependency by name into the current process and parse its
 * exports.
 *
 * Resolves @name in C:\Windows\System32 through the NT path resolver, opens
 * it read-only, validates it as a DLL, maps it with pe_map_image() (at its
 * preferred base; a RELOCS_STRIPPED DLL that cannot get that base is refused,
 * since it cannot be relocated), and digests its export directory into a
 * copy_from_user-backed struct pe_exports.  On success *@out owns the export
 * view and the mapping stays for the life of the process; on any failure
 * nothing is left mapped or allocated and a negative errno is returned.
 *
 * Deliberately unsupported and refused or ignored rather than faked: a DLL
 * that itself imports from another DLL (single level only - the caller does
 * not recurse), DllMain and TLS callbacks (never run), and forwarder exports
 * (pe_export_by_name() reports them unresolved).  It is marked __maybe_unused
 * because the import-binding caller arrives in phase C; nothing invokes it in
 * this phase.
 */
static int __maybe_unused nt_load_dll(const char *name,
				      struct nt_loaded_dll **out)
{
	struct nt_loaded_dll *dll = NULL;
	struct section_header *sections = NULL;
	struct nt_path ntp = {};
	struct file *file;
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;
	struct data_dirent exp_dd = {};
	struct mz_hdr mz;
	unsigned long load_base;
	char *path;
	u32 peaddr;
	int retval;

	if (!name || !out)
		return -EINVAL;

	/* Build C:\Windows\System32\<name> and resolve it by NT path rules. */
	path = kasprintf(GFP_KERNEL, NT_DLL_SEARCH_DIR "%s", name);
	if (!path)
		return -ENOMEM;
	retval = nt_kern_path(path,
			      NT_RESOLVE_FOLLOW | NT_RESOLVE_CASE_INSENSITIVE,
			      &ntp);
	kfree(path);
	if (retval)
		return retval;

	file = dentry_open(&ntp.path, O_RDONLY | O_LARGEFILE, current_cred());
	nt_path_put(&ntp);
	if (IS_ERR(file))
		return PTR_ERR(file);

	if (!can_mmap_file(file)) {
		retval = -ENOEXEC;
		goto out_fput;
	}

	/* Read and validate the headers, in DLL mode. */
	retval = pe_read_exact(file, &mz, sizeof(mz), 0);
	if (retval)
		goto out_fput;
	retval = -ENOEXEC;
	if (mz.magic != IMAGE_DOS_SIGNATURE)
		goto out_fput;
	peaddr = mz.peaddr;
	if (peaddr < sizeof(mz) || peaddr > PE_MAX_HEADER_OFFSET)
		goto out_fput;

	retval = pe_read_exact(file, &pe, sizeof(pe), peaddr);
	if (retval)
		goto out_fput;
	retval = pe_read_exact(file, &opt, sizeof(opt),
			       (loff_t)peaddr + sizeof(pe));
	if (retval)
		goto out_fput;

	retval = pe_parse_dll_headers(&mz, sizeof(mz), &pe, &opt, &pi);
	if (retval)
		goto out_fput;

	/* The export directory is data directory 0; a DLL without one is useless. */
	retval = -ENOEXEC;
	if (opt.data_dirs <= PE_DIR_EXPORT)
		goto out_fput;
	retval = pe_read_exact(file, &exp_dd, sizeof(exp_dd),
			       (loff_t)peaddr + sizeof(pe) + sizeof(opt) +
			       PE_DIR_EXPORT * sizeof(exp_dd));
	if (retval)
		goto out_fput;
	retval = -ENOEXEC;
	if (!exp_dd.virtual_address || !exp_dd.size ||
	    exp_dd.virtual_address >= pi.image_size ||
	    exp_dd.size > pi.image_size - exp_dd.virtual_address)
		goto out_fput;

	/*
	 * The base relocation table, read the same way the EXE path reads it, so
	 * that a relocatable DLL whose preferred base is taken can still be
	 * placed.  A RELOCS_STRIPPED DLL leaves these zero and pe_map_image()
	 * refuses to move it, which is fine for a DLL pinned at a free base.
	 */
	if (opt.data_dirs > PE_DIR_BASERELOC) {
		struct data_dirent reloc_dd;

		retval = pe_read_exact(file, &reloc_dd, sizeof(reloc_dd),
				       (loff_t)peaddr + sizeof(pe) + sizeof(opt) +
				       PE_DIR_BASERELOC * sizeof(reloc_dd));
		if (retval)
			goto out_fput;
		pi.reloc_rva = reloc_dd.virtual_address;
		pi.reloc_size = reloc_dd.size;
		retval = -ENOEXEC;
		if (pi.reloc_size &&
		    (pi.reloc_rva >= pi.image_size ||
		     pi.reloc_size > pi.image_size - pi.reloc_rva))
			goto out_fput;
	}

	/* Slurp the section table. */
	sections = kvmalloc_array(pi.nsections, sizeof(*sections), GFP_KERNEL);
	if (!sections) {
		retval = -ENOMEM;
		goto out_fput;
	}
	retval = pe_read_exact(file, sections,
			       (size_t)pi.nsections * sizeof(*sections),
			       pi.section_table);
	if (retval)
		goto out_free;

	dll = kzalloc_obj(*dll, GFP_KERNEL);
	if (!dll) {
		retval = -ENOMEM;
		goto out_free;
	}

	/* Map the DLL at its preferred base (a dependency is never randomised). */
	retval = pe_map_image(file, &pi, sections, false, &load_base);
	if (retval)
		goto out_free_dll;

	dll->base = load_base;
	dll->image_size = pi.image_size;
	dll->entry = pi.entry;
	dll->img.base = load_base;
	dll->img.image_size = pi.image_size;
	dll->reader.read = pe_user_read;
	dll->reader.ctx = &dll->img;
	dll->reader.image_size = pi.image_size;

	/*
	 * Digest the export directory through the mapped-image reader.  The RVAs
	 * are relative to the image, so the reader adds @load_base for each
	 * access - which is why this works whether or not the DLL kept its
	 * preferred base.
	 */
	retval = pe_parse_exports(&dll->reader, exp_dd.virtual_address,
				  exp_dd.size, &dll->exports);
	if (retval)
		goto out_unmap;

	kvfree(sections);
	fput(file);
	*out = dll;
	return 0;

out_unmap:
	vm_munmap(load_base, pi.image_size);
out_free_dll:
	kfree(dll);
out_free:
	kvfree(sections);
out_fput:
	fput(file);
	return retval;
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
	unsigned long load_base, sp;
#ifdef CONFIG_NT_FS_PERSONALITY
	unsigned long teb;
#endif
	u32 peaddr;
	int retval;

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
	 * Reserve the image range, map the headers and sections, and relocate
	 * if the image did not land at its preferred base.  An executable that
	 * carries base relocations is loaded at a kernel-chosen base when
	 * address-space randomisation is in effect, exactly as a PIE ELF is; the
	 * shared mapper handles the rest.  On failure it leaves nothing mapped.
	 */
	retval = pe_map_image(file, &pi, sections,
			      !!(current->flags & PF_RANDOMIZE), &load_base);
	if (retval)
		goto out_free;

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
