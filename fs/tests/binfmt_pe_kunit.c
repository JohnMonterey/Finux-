// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the PE/COFF header validation in fs/binfmt_pe.c.
 *
 * pe_parse_headers() is the pure, side-effect-free core of the loader: it
 * takes already-read headers and either digests them into a pe_load_info
 * or rejects the image.  That makes it exactly the piece worth unit
 * testing - every "we only support ..." rule lives there, and each rule
 * gets a case below built from an otherwise-valid header set.
 */
#include <kunit/test.h>

/*
 * Fill @mz, @pe and @opt with a minimal but completely valid PE32+ AMD64
 * executable header set: one section, page-aligned throughout, entry and
 * headers inside the image.  Individual tests then break one field.
 */
static void pe_make_valid(char *mz, struct pe_hdr *pe,
			  struct pe32plus_opt_hdr *opt)
{
	struct mz_hdr *mzh = (struct mz_hdr *)mz;

	memset(mz, 0, BINPRM_BUF_SIZE);
	mzh->magic = IMAGE_DOS_SIGNATURE;
	mzh->peaddr = 0x80;

	memset(pe, 0, sizeof(*pe));
	pe->magic = IMAGE_NT_SIGNATURE;
	pe->machine = IMAGE_FILE_MACHINE_AMD64;
	pe->sections = 1;
	pe->opt_hdr_size = sizeof(*opt) + PE_MAX_DATA_DIRS * sizeof(struct data_dirent);
	pe->flags = IMAGE_FILE_EXECUTABLE_IMAGE;

	memset(opt, 0, sizeof(*opt));
	opt->magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
	opt->entry_point = 0x1000;
	opt->code_base = 0x1000;
	opt->image_base = 0x140000000UL;
	opt->section_align = 0x1000;
	opt->file_align = 0x1000;
	opt->image_size = 0x2000;
	opt->header_size = 0x1000;
	opt->data_dirs = PE_MAX_DATA_DIRS;
}

static void pe_parse_valid_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	pe_make_valid(mz, &pe, &opt);
	memset(&pi, 0, sizeof(pi));

	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi), 0);

	/* The digested values must match what went in. */
	KUNIT_EXPECT_EQ(test, pi.image_base, 0x140000000UL);
	KUNIT_EXPECT_EQ(test, pi.image_size, 0x2000UL);
	KUNIT_EXPECT_EQ(test, pi.entry, 0x1000UL);
	KUNIT_EXPECT_EQ(test, pi.header_size, 0x1000UL);
	KUNIT_EXPECT_EQ(test, pi.nsections, 1);
	/* section table = peaddr + sizeof(pe_hdr) + opt_hdr_size */
	KUNIT_EXPECT_EQ(test, pi.section_table,
			(loff_t)0x80 + sizeof(struct pe_hdr) + pe.opt_hdr_size);
}

static void pe_parse_bad_mz_magic_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	pe_make_valid(mz, &pe, &opt);
	((struct mz_hdr *)mz)->magic = 0x1234;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

static void pe_parse_bad_peaddr_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	/* peaddr inside the MZ header itself is nonsense. */
	pe_make_valid(mz, &pe, &opt);
	((struct mz_hdr *)mz)->peaddr = 4;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);

	/* peaddr beyond the plausibility bound. */
	pe_make_valid(mz, &pe, &opt);
	((struct mz_hdr *)mz)->peaddr = PE_MAX_HEADER_OFFSET + 1;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

static void pe_parse_bad_pe_magic_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	pe_make_valid(mz, &pe, &opt);
	pe.magic = 0xdeadbeef;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

static void pe_parse_wrong_machine_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	/* A 32-bit x86 image is not something this loader handles. */
	pe_make_valid(mz, &pe, &opt);
	pe.machine = IMAGE_FILE_MACHINE_I386;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

static void pe_parse_not_executable_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	pe_make_valid(mz, &pe, &opt);
	pe.flags &= ~IMAGE_FILE_EXECUTABLE_IMAGE;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

static void pe_parse_dll_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	/* A DLL is a PE image but not an executable we start. */
	pe_make_valid(mz, &pe, &opt);
	pe.flags |= IMAGE_FILE_DLL;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

static void pe_parse_pe32_not_plus_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	/* 32-bit PE32 (not PE32+) optional header. */
	pe_make_valid(mz, &pe, &opt);
	opt.magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

static void pe_parse_opt_hdr_too_small_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	pe_make_valid(mz, &pe, &opt);
	pe.opt_hdr_size = sizeof(opt) - 1;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

static void pe_parse_section_count_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	pe_make_valid(mz, &pe, &opt);
	pe.sections = 0;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);

	pe_make_valid(mz, &pe, &opt);
	pe.sections = PE_MAX_SECTIONS + 1;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

static void pe_parse_bad_alignment_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	/* Sub-page in-memory section alignment. */
	pe_make_valid(mz, &pe, &opt);
	opt.section_align = 0x200;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);

	/* File alignment below the PE minimum of 512. */
	pe_make_valid(mz, &pe, &opt);
	opt.file_align = 256;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);

	/* File alignment that is not a power of two. */
	pe_make_valid(mz, &pe, &opt);
	opt.file_align = 0x600;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);

	/* Section alignment smaller than file alignment (PE forbids this). */
	pe_make_valid(mz, &pe, &opt);
	opt.section_align = 0x1000;
	opt.file_align = 0x10000;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

static void pe_parse_subpage_file_align_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	/*
	 * The old 512-byte file alignment with page section alignment is a
	 * perfectly valid PE; the loader accepts it and copies such sections
	 * in rather than mapping them from the file.
	 */
	pe_make_valid(mz, &pe, &opt);
	opt.file_align = 512;
	opt.section_align = 0x1000;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi), 0);
	KUNIT_EXPECT_EQ(test, pi.file_align, 512);
}

static void pe_parse_bad_image_base_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	/* Unaligned preferred base. */
	pe_make_valid(mz, &pe, &opt);
	opt.image_base = 0x140000123UL;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);

	/* Zero base. */
	pe_make_valid(mz, &pe, &opt);
	opt.image_base = 0;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

static void pe_parse_bad_image_size_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	pe_make_valid(mz, &pe, &opt);
	opt.image_size = 0;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);

	/* Larger than the sanity cap. */
	pe_make_valid(mz, &pe, &opt);
	opt.image_size = PE_MAX_IMAGE_SIZE + 0x1000;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

static void pe_parse_bad_entry_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	/* Entry point of zero. */
	pe_make_valid(mz, &pe, &opt);
	opt.entry_point = 0;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);

	/* Entry point past the end of the image. */
	pe_make_valid(mz, &pe, &opt);
	opt.entry_point = opt.image_size;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

static void pe_parse_header_too_big_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	/* SizeOfHeaders larger than the whole image. */
	pe_make_valid(mz, &pe, &opt);
	opt.header_size = opt.image_size + 0x1000;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

static void pe_parse_section_table_overrun_test(struct kunit *test)
{
	char mz[BINPRM_BUF_SIZE];
	struct pe_hdr pe;
	struct pe32plus_opt_hdr opt;
	struct pe_load_info pi;

	/*
	 * A tiny header region cannot hold the section table.  With the
	 * headers only one page but many sections placed after a large
	 * optional header, the table would spill past SizeOfHeaders.
	 */
	pe_make_valid(mz, &pe, &opt);
	pe.sections = PE_MAX_SECTIONS;
	opt.header_size = 0x1000;
	((struct mz_hdr *)mz)->peaddr = 0xE0;
	KUNIT_EXPECT_EQ(test, pe_parse_headers(mz, sizeof(mz), &pe, &opt, &pi),
			-ENOEXEC);
}

/*
 * Export/import parser tests
 * --------------------------
 * The parsers reach a PE image only through a struct pe_image_reader, so these
 * build a small flat image by hand, wrap it in the flat-buffer reader, and
 * drive the same code the loader will.  The image is heap-allocated (it is far
 * larger than the header structs above) and freed with the test.  Layout
 * constants place the export arrays and the import thunk tables at known RVAs;
 * fields are written little-endian so the tests do not depend on host byte
 * order even though the parsers only ever run on x86_64.
 */
#define PET_IMAGE_SIZE		0x2000

/* Export directory layout. */
#define PET_EXP_DIR		0x100
#define PET_EXP_DIR_SIZE	0x100		/* covers [0x100, 0x200) */
#define PET_EXP_FWD_STR		0x128		/* forwarder string, in the dir */
#define PET_EXP_FUNCS		0x200		/* u32[3] function RVAs */
#define PET_EXP_NAMES		0x220		/* u32[3] name RVAs */
#define PET_EXP_ORDS		0x240		/* u16[3] name ordinals */
#define PET_EXP_STR_FOO		0x300
#define PET_EXP_STR_BAZ		0x310
#define PET_EXP_STR_FWD		0x320
#define PET_EXP_FUNC_FOO	0x1000
#define PET_EXP_FUNC_BAZ	0x1200
#define PET_EXP_BASE		1

/* Import directory layout. */
#define PET_IMP_DIR		0x400
#define PET_IMP_DIR_SIZE	0x40
#define PET_IMP_DLLNAME		0x480		/* clear of the thunk tables */
#define PET_IMP_INT		0x500		/* OriginalFirstThunk table */
#define PET_IMP_IAT		0x520		/* FirstThunk table */
#define PET_IMP_BYNAME		0x700		/* IMAGE_IMPORT_BY_NAME */
#define PET_IMP_HINT		0x7
#define PET_IMP_ORDINAL		0x123

static void pet_write_str(u8 *img, u32 rva, const char *s)
{
	strscpy((char *)(img + rva), s, PET_IMAGE_SIZE - rva);
}

/*
 * Build an image with three exports: NtFoo (ordinal 1), a forwarder (ordinal
 * 2, whose function RVA points back into the directory) and NtBaz (ordinal 3).
 * All three are also named, so the forwarder can be probed by name too.
 */
static void pet_build_exports(u8 *img)
{
	memset(img, 0, PET_IMAGE_SIZE);

	put_unaligned_le32(PET_EXP_BASE, img + PET_EXP_DIR + 16);
	put_unaligned_le32(3, img + PET_EXP_DIR + 20);	/* num_functions */
	put_unaligned_le32(3, img + PET_EXP_DIR + 24);	/* num_names */
	put_unaligned_le32(PET_EXP_FUNCS, img + PET_EXP_DIR + 28);
	put_unaligned_le32(PET_EXP_NAMES, img + PET_EXP_DIR + 32);
	put_unaligned_le32(PET_EXP_ORDS, img + PET_EXP_DIR + 36);

	/* Forwarder target string, deliberately inside the directory region. */
	pet_write_str(img, PET_EXP_FWD_STR, "OTHER.Bar");

	/* Function RVAs: [0]=Foo, [1]=forwarder, [2]=Baz. */
	put_unaligned_le32(PET_EXP_FUNC_FOO, img + PET_EXP_FUNCS + 0);
	put_unaligned_le32(PET_EXP_FWD_STR, img + PET_EXP_FUNCS + 4);
	put_unaligned_le32(PET_EXP_FUNC_BAZ, img + PET_EXP_FUNCS + 8);

	/* Name RVAs and the names they point at. */
	put_unaligned_le32(PET_EXP_STR_FOO, img + PET_EXP_NAMES + 0);
	put_unaligned_le32(PET_EXP_STR_BAZ, img + PET_EXP_NAMES + 4);
	put_unaligned_le32(PET_EXP_STR_FWD, img + PET_EXP_NAMES + 8);
	pet_write_str(img, PET_EXP_STR_FOO, "NtFoo");
	pet_write_str(img, PET_EXP_STR_BAZ, "NtBaz");
	pet_write_str(img, PET_EXP_STR_FWD, "NtFwd");

	/* name index -> function index. */
	put_unaligned_le16(0, img + PET_EXP_ORDS + 0);	/* NtFoo -> func[0] */
	put_unaligned_le16(2, img + PET_EXP_ORDS + 2);	/* NtBaz -> func[2] */
	put_unaligned_le16(1, img + PET_EXP_ORDS + 4);	/* NtFwd -> func[1] */
}

/* One import descriptor (ntdll.dll) with a by-name and a by-ordinal thunk. */
static void pet_build_imports(u8 *img)
{
	memset(img, 0, PET_IMAGE_SIZE);

	put_unaligned_le32(PET_IMP_INT, img + PET_IMP_DIR + 0);	/* OFT */
	put_unaligned_le32(PET_IMP_DLLNAME, img + PET_IMP_DIR + 12); /* Name */
	put_unaligned_le32(PET_IMP_IAT, img + PET_IMP_DIR + 16);	/* FirstThunk */
	/* descriptor[1] at PET_IMP_DIR + 20 stays all-zero: the terminator. */

	pet_write_str(img, PET_IMP_DLLNAME, "ntdll.dll");

	/* Import name table: [0] by name, [1] by ordinal, [2] terminator. */
	put_unaligned_le64(PET_IMP_BYNAME, img + PET_IMP_INT + 0);
	put_unaligned_le64(IMAGE_ORDINAL_FLAG64 | PET_IMP_ORDINAL,
			   img + PET_IMP_INT + 8);

	/* IMAGE_IMPORT_BY_NAME { hint, "NtCreateFile" }. */
	put_unaligned_le16(PET_IMP_HINT, img + PET_IMP_BYNAME + 0);
	pet_write_str(img, PET_IMP_BYNAME + 2, "NtCreateFile");
}

struct pet_import_log {
	int n;
	struct {
		char dll[32];
		char name[32];
		u32 iat_slot_rva;
		u16 ordinal;
		u16 hint;
		bool by_ordinal;
	} e[8];
};

/* Tolerant collector: always counts, only stores while there is room. */
static int pet_import_collect(void *ctx, const struct pe_import *imp)
{
	struct pet_import_log *log = ctx;

	if (log->n < (int)ARRAY_SIZE(log->e)) {
		strscpy(log->e[log->n].dll, imp->dll,
			sizeof(log->e[log->n].dll));
		if (imp->name)
			strscpy(log->e[log->n].name, imp->name,
				sizeof(log->e[log->n].name));
		else
			log->e[log->n].name[0] = '\0';
		log->e[log->n].iat_slot_rva = imp->iat_slot_rva;
		log->e[log->n].ordinal = imp->ordinal;
		log->e[log->n].hint = imp->hint;
		log->e[log->n].by_ordinal = imp->by_ordinal;
	}
	log->n++;
	return 0;
}

static void pe_export_lookup_test(struct kunit *test)
{
	u8 *img = kunit_kzalloc(test, PET_IMAGE_SIZE, GFP_KERNEL);
	struct pe_image_reader r;
	struct pe_flat_image flat;
	struct pe_exports exp;

	KUNIT_ASSERT_NOT_NULL(test, img);
	pet_build_exports(img);
	pe_flat_image_reader(&r, &flat, img, PET_IMAGE_SIZE);

	KUNIT_ASSERT_EQ(test, pe_parse_exports(&r, PET_EXP_DIR,
					       PET_EXP_DIR_SIZE, &exp), 0);

	/* By name: two hits and a miss. */
	KUNIT_EXPECT_EQ(test, pe_export_by_name(&exp, "NtFoo"), PET_EXP_FUNC_FOO);
	KUNIT_EXPECT_EQ(test, pe_export_by_name(&exp, "NtBaz"), PET_EXP_FUNC_BAZ);
	KUNIT_EXPECT_EQ(test, pe_export_by_name(&exp, "NtNope"), 0);

	/* By ordinal: base is 1, so ordinal 1 -> func[0], ordinal 3 -> func[2]. */
	KUNIT_EXPECT_EQ(test, pe_export_by_ordinal(&exp, 1), PET_EXP_FUNC_FOO);
	KUNIT_EXPECT_EQ(test, pe_export_by_ordinal(&exp, 3), PET_EXP_FUNC_BAZ);

	/* Out of range: below Base and past the end of the function array. */
	KUNIT_EXPECT_EQ(test, pe_export_by_ordinal(&exp, 0), 0);
	KUNIT_EXPECT_EQ(test, pe_export_by_ordinal(&exp, 100), 0);
}

static void pe_export_forwarder_test(struct kunit *test)
{
	u8 *img = kunit_kzalloc(test, PET_IMAGE_SIZE, GFP_KERNEL);
	struct pe_image_reader r;
	struct pe_flat_image flat;
	struct pe_exports exp;

	KUNIT_ASSERT_NOT_NULL(test, img);
	pet_build_exports(img);
	pe_flat_image_reader(&r, &flat, img, PET_IMAGE_SIZE);

	KUNIT_ASSERT_EQ(test, pe_parse_exports(&r, PET_EXP_DIR,
					       PET_EXP_DIR_SIZE, &exp), 0);

	/*
	 * The forwarder export (ordinal 2, name "NtFwd") resolves to an RVA
	 * inside the export directory, so it is reported unresolved by both
	 * lookups rather than returned as a code address.
	 */
	KUNIT_EXPECT_EQ(test, pe_export_by_ordinal(&exp, 2), 0);
	KUNIT_EXPECT_EQ(test, pe_export_by_name(&exp, "NtFwd"), 0);
}

static void pe_export_malformed_test(struct kunit *test)
{
	u8 *img = kunit_kzalloc(test, PET_IMAGE_SIZE, GFP_KERNEL);
	struct pe_image_reader r;
	struct pe_flat_image flat;
	struct pe_exports exp;

	KUNIT_ASSERT_NOT_NULL(test, img);
	pet_build_exports(img);
	pe_flat_image_reader(&r, &flat, img, PET_IMAGE_SIZE);

	/* No export directory at all. */
	KUNIT_EXPECT_EQ(test, pe_parse_exports(&r, 0, 0, &exp), -ENOENT);

	/* Directory size smaller than the fixed header. */
	KUNIT_EXPECT_EQ(test, pe_parse_exports(&r, PET_EXP_DIR, 8, &exp),
			-ENOEXEC);

	/* NumberOfFunctions past the sanity cap. */
	put_unaligned_le32(PE_MAX_EXPORTS + 1, img + PET_EXP_DIR + 20);
	KUNIT_EXPECT_EQ(test, pe_parse_exports(&r, PET_EXP_DIR,
					       PET_EXP_DIR_SIZE, &exp), -ENOEXEC);

	/* Function array running off the end of the image. */
	pet_build_exports(img);
	put_unaligned_le32(PET_IMAGE_SIZE - 4, img + PET_EXP_DIR + 28);
	KUNIT_EXPECT_EQ(test, pe_parse_exports(&r, PET_EXP_DIR,
					       PET_EXP_DIR_SIZE, &exp), -ENOEXEC);
}

static void pe_export_name_off_end_test(struct kunit *test)
{
	u8 *img = kunit_kzalloc(test, PET_IMAGE_SIZE, GFP_KERNEL);
	struct pe_image_reader r;
	struct pe_flat_image flat;
	struct pe_exports exp;
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, img);
	pet_build_exports(img);

	/*
	 * Point a name entry at an unterminated run of bytes at the very end of
	 * the image.  The comparison must stop at the image edge and report a
	 * miss instead of reading past it.
	 */
	for (i = PET_IMAGE_SIZE - 8; i < PET_IMAGE_SIZE; i++)
		img[i] = 'A';
	put_unaligned_le32(PET_IMAGE_SIZE - 8, img + PET_EXP_NAMES + 0);

	pe_flat_image_reader(&r, &flat, img, PET_IMAGE_SIZE);
	KUNIT_ASSERT_EQ(test, pe_parse_exports(&r, PET_EXP_DIR,
					       PET_EXP_DIR_SIZE, &exp), 0);
	KUNIT_EXPECT_EQ(test, pe_export_by_name(&exp, "AAAAAAAAAAAA"), 0);
}

static void pe_import_enumerate_test(struct kunit *test)
{
	u8 *img = kunit_kzalloc(test, PET_IMAGE_SIZE, GFP_KERNEL);
	struct pe_image_reader r;
	struct pe_flat_image flat;
	struct pet_import_log log = { };

	KUNIT_ASSERT_NOT_NULL(test, img);
	pet_build_imports(img);
	pe_flat_image_reader(&r, &flat, img, PET_IMAGE_SIZE);

	KUNIT_ASSERT_EQ(test, pe_parse_imports(&r, PET_IMP_DIR, PET_IMP_DIR_SIZE,
					       pet_import_collect, &log), 0);
	KUNIT_ASSERT_EQ(test, log.n, 2);

	/* Entry 0: import by name, IAT slot at FirstThunk + 0. */
	KUNIT_EXPECT_STREQ(test, log.e[0].dll, "ntdll.dll");
	KUNIT_EXPECT_FALSE(test, log.e[0].by_ordinal);
	KUNIT_EXPECT_STREQ(test, log.e[0].name, "NtCreateFile");
	KUNIT_EXPECT_EQ(test, log.e[0].hint, PET_IMP_HINT);
	KUNIT_EXPECT_EQ(test, log.e[0].iat_slot_rva, PET_IMP_IAT + 0);

	/* Entry 1: import by ordinal, IAT slot at FirstThunk + 8. */
	KUNIT_EXPECT_STREQ(test, log.e[1].dll, "ntdll.dll");
	KUNIT_EXPECT_TRUE(test, log.e[1].by_ordinal);
	KUNIT_EXPECT_EQ(test, log.e[1].ordinal, PET_IMP_ORDINAL);
	KUNIT_EXPECT_EQ(test, log.e[1].iat_slot_rva, PET_IMP_IAT + 8);
}

static void pe_import_oft_fallback_test(struct kunit *test)
{
	u8 *img = kunit_kzalloc(test, PET_IMAGE_SIZE, GFP_KERNEL);
	struct pe_image_reader r;
	struct pe_flat_image flat;
	struct pet_import_log log = { };

	KUNIT_ASSERT_NOT_NULL(test, img);
	pet_build_imports(img);

	/* Clear OriginalFirstThunk; the lookup must fall back to FirstThunk. */
	put_unaligned_le32(0, img + PET_IMP_DIR + 0);
	put_unaligned_le64(PET_IMP_BYNAME, img + PET_IMP_IAT + 0);
	put_unaligned_le64(IMAGE_ORDINAL_FLAG64 | PET_IMP_ORDINAL,
			   img + PET_IMP_IAT + 8);

	pe_flat_image_reader(&r, &flat, img, PET_IMAGE_SIZE);
	KUNIT_ASSERT_EQ(test, pe_parse_imports(&r, PET_IMP_DIR, PET_IMP_DIR_SIZE,
					       pet_import_collect, &log), 0);
	KUNIT_ASSERT_EQ(test, log.n, 2);
	KUNIT_EXPECT_STREQ(test, log.e[0].name, "NtCreateFile");
	/* IAT slot RVAs are still measured from FirstThunk. */
	KUNIT_EXPECT_EQ(test, log.e[0].iat_slot_rva, PET_IMP_IAT + 0);
	KUNIT_EXPECT_EQ(test, log.e[1].iat_slot_rva, PET_IMP_IAT + 8);
}

static void pe_import_malformed_test(struct kunit *test)
{
	u8 *img = kunit_kzalloc(test, PET_IMAGE_SIZE, GFP_KERNEL);
	struct pe_image_reader r;
	struct pet_import_log log;
	struct pe_flat_image flat;
	u32 rva;

	KUNIT_ASSERT_NOT_NULL(test, img);

	/* No import directory at all. */
	pet_build_imports(img);
	pe_flat_image_reader(&r, &flat, img, PET_IMAGE_SIZE);
	memset(&log, 0, sizeof(log));
	KUNIT_EXPECT_EQ(test, pe_parse_imports(&r, 0, 0, pet_import_collect,
					       &log), -ENOENT);

	/* First descriptor only partly inside the image (unterminated array). */
	memset(&log, 0, sizeof(log));
	KUNIT_EXPECT_LT(test, pe_parse_imports(&r, PET_IMAGE_SIZE - 8,
					       PET_IMP_DIR_SIZE,
					       pet_import_collect, &log), 0);

	/* By-name thunk whose symbol name has no terminator before the end. */
	pet_build_imports(img);
	for (rva = PET_IMAGE_SIZE - 4; rva < PET_IMAGE_SIZE; rva++)
		img[rva] = 'x';
	/* Name entry: hint occupies the last two readable bytes... */
	put_unaligned_le64(PET_IMAGE_SIZE - 6, img + PET_IMP_INT + 0);
	pe_flat_image_reader(&r, &flat, img, PET_IMAGE_SIZE);
	memset(&log, 0, sizeof(log));
	KUNIT_EXPECT_LT(test, pe_parse_imports(&r, PET_IMP_DIR, PET_IMP_DIR_SIZE,
					       pet_import_collect, &log), 0);

	/* Unterminated thunk table: nonzero thunks all the way to the end. */
	memset(img, 0, PET_IMAGE_SIZE);
	put_unaligned_le32(PET_IMP_INT, img + PET_IMP_DIR + 0);
	put_unaligned_le32(PET_IMP_DLLNAME, img + PET_IMP_DIR + 12);
	put_unaligned_le32(PET_IMP_IAT, img + PET_IMP_DIR + 16);
	pet_write_str(img, PET_IMP_DLLNAME, "ntdll.dll");
	for (rva = PET_IMP_INT; rva + 8 <= PET_IMAGE_SIZE; rva += 8)
		put_unaligned_le64(IMAGE_ORDINAL_FLAG64 | 1, img + rva);
	pe_flat_image_reader(&r, &flat, img, PET_IMAGE_SIZE);
	memset(&log, 0, sizeof(log));
	KUNIT_EXPECT_LT(test, pe_parse_imports(&r, PET_IMP_DIR, PET_IMP_DIR_SIZE,
					       pet_import_collect, &log), 0);
}

/*
 * ntdll.dll export asset
 * ----------------------
 * Phase B ships a real minimal ntdll.dll (a hand-assembled DLL PE whose .text
 * is one syscall stub per NT service and whose export directory publishes the
 * seven Nt* names).  This case builds an in-memory image with that exact export
 * shape and the exact same stub bytes, then drives the Phase-A export parser
 * over it with the flat reader: every name must resolve to its stub's RVA, the
 * stub bytes at that RVA must be the "mov r10,rcx; mov eax,<svc>; syscall; ret"
 * for the right service number, ordinals must resolve too, and a name the DLL
 * does not export must miss.  That proves the asset and the parse together,
 * without a running process; the file-based nt_load_dll() path is proven end to
 * end in phase C.  The layout constants below match the userspace builder in
 * tools/testing/selftests/binfmt_pe/ntdll_builder.h byte for byte.
 */
#define NTDLL_IMAGE_SIZE	0x3000
#define NTDLL_TEXT_RVA		0x1000	/* .text: the syscall stubs */
#define NTDLL_STUB_STRIDE	0x10	/* one 16-byte slot per stub */
#define NTDLL_EDATA_RVA		0x2000	/* .edata: the export directory */
#define NTDLL_EAT		0x2028	/* u32[7] function RVAs */
#define NTDLL_ENPT		0x2048	/* u32[7] name RVAs */
#define NTDLL_ORD		0x2068	/* u16[7] name ordinals */
#define NTDLL_DLLNAME		0x2080	/* "ntdll.dll" */
#define NTDLL_NAMES		0x2090	/* packed name strings */
#define NTDLL_EXP_BASE		1	/* ordinal of the first export */

/*
 * The exported names, alphabetically sorted (as a real export name table is),
 * each paired with the NT service number its stub invokes.  The i-th entry's
 * stub sits at NTDLL_TEXT_RVA + i * NTDLL_STUB_STRIDE.
 */
static const struct ntdll_sym {
	const char *name;
	u8 svc;
} ntdll_syms[] = {
	{ "NtClose",			NT_SYS_NtClose },
	{ "NtCreateFile",		NT_SYS_NtCreateFile },
	{ "NtOpenFile",			NT_SYS_NtOpenFile },
	{ "NtQueryInformationFile",	NT_SYS_NtQueryInformationFile },
	{ "NtReadFile",			NT_SYS_NtReadFile },
	{ "NtTerminateProcess",		NT_SYS_NtTerminateProcess },
	{ "NtWriteFile",		NT_SYS_NtWriteFile },
};

/*
 * Lay out the ntdll image into @img and return the export directory's size.
 * Each stub is "mov r10,rcx; mov eax,<svc>; syscall; ret" (49 89 ca / b8 imm32
 * / 0f 05 / c3), the same eleven bytes the shipped ntdll.dll carries.
 */
static u32 pet_build_ntdll(u8 *img)
{
	u32 name_rva = NTDLL_NAMES;
	unsigned int i;

	memset(img, 0, NTDLL_IMAGE_SIZE);

	/* IMAGE_EXPORT_DIRECTORY at NTDLL_EDATA_RVA. */
	put_unaligned_le32(NTDLL_DLLNAME, img + NTDLL_EDATA_RVA + 12);	/* Name */
	put_unaligned_le32(NTDLL_EXP_BASE, img + NTDLL_EDATA_RVA + 16);	/* Base */
	put_unaligned_le32(ARRAY_SIZE(ntdll_syms), img + NTDLL_EDATA_RVA + 20);
	put_unaligned_le32(ARRAY_SIZE(ntdll_syms), img + NTDLL_EDATA_RVA + 24);
	put_unaligned_le32(NTDLL_EAT, img + NTDLL_EDATA_RVA + 28);
	put_unaligned_le32(NTDLL_ENPT, img + NTDLL_EDATA_RVA + 32);
	put_unaligned_le32(NTDLL_ORD, img + NTDLL_EDATA_RVA + 36);

	strscpy((char *)(img + NTDLL_DLLNAME), "ntdll.dll",
		NTDLL_IMAGE_SIZE - NTDLL_DLLNAME);

	for (i = 0; i < ARRAY_SIZE(ntdll_syms); i++) {
		u32 stub = NTDLL_TEXT_RVA + i * NTDLL_STUB_STRIDE;
		u8 *code = img + stub;

		code[0] = 0x49;			/* mov r10,rcx */
		code[1] = 0x89;
		code[2] = 0xca;
		code[3] = 0xb8;			/* mov eax,<svc> */
		put_unaligned_le32(ntdll_syms[i].svc, code + 4);
		code[8] = 0x0f;			/* syscall */
		code[9] = 0x05;
		code[10] = 0xc3;		/* ret */

		put_unaligned_le32(stub, img + NTDLL_EAT + i * sizeof(u32));
		put_unaligned_le32(name_rva, img + NTDLL_ENPT + i * sizeof(u32));
		put_unaligned_le16(i, img + NTDLL_ORD + i * sizeof(u16));
		strscpy((char *)(img + name_rva), ntdll_syms[i].name,
			NTDLL_IMAGE_SIZE - name_rva);
		name_rva += strlen(ntdll_syms[i].name) + 1;
	}

	return name_rva - NTDLL_EDATA_RVA;
}

static void pe_ntdll_exports_test(struct kunit *test)
{
	u8 *img = kunit_kzalloc(test, NTDLL_IMAGE_SIZE, GFP_KERNEL);
	struct pe_image_reader r;
	struct pe_flat_image flat;
	struct pe_exports exp;
	u32 dir_size;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, img);
	dir_size = pet_build_ntdll(img);
	pe_flat_image_reader(&r, &flat, img, NTDLL_IMAGE_SIZE);

	KUNIT_ASSERT_EQ(test, pe_parse_exports(&r, NTDLL_EDATA_RVA, dir_size,
					       &exp), 0);
	KUNIT_EXPECT_EQ(test, exp.num_names, ARRAY_SIZE(ntdll_syms));

	for (i = 0; i < ARRAY_SIZE(ntdll_syms); i++) {
		u32 want = NTDLL_TEXT_RVA + i * NTDLL_STUB_STRIDE;
		const u8 *code = img + want;

		/* Name resolves to the stub RVA... */
		KUNIT_EXPECT_EQ_MSG(test, pe_export_by_name(&exp, ntdll_syms[i].name),
				    want, "name %s", ntdll_syms[i].name);
		/* ...and by ordinal (Base is 1, so ordinal i+1 -> function[i]). */
		KUNIT_EXPECT_EQ(test,
				pe_export_by_ordinal(&exp, NTDLL_EXP_BASE + i),
				want);

		/* The stub at that RVA is the syscall stub for its service. */
		KUNIT_EXPECT_EQ(test, code[0], 0x49);
		KUNIT_EXPECT_EQ(test, code[1], 0x89);
		KUNIT_EXPECT_EQ(test, code[2], 0xca);
		KUNIT_EXPECT_EQ(test, code[3], 0xb8);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(code + 4),
				(u32)ntdll_syms[i].svc);
		KUNIT_EXPECT_EQ(test, code[8], 0x0f);
		KUNIT_EXPECT_EQ(test, code[9], 0x05);
		KUNIT_EXPECT_EQ(test, code[10], 0xc3);
	}

	/* A name the DLL does not export misses. */
	KUNIT_EXPECT_EQ(test, pe_export_by_name(&exp, "NtNotAThing"), 0);
	/* An ordinal below Base and one past the last export miss. */
	KUNIT_EXPECT_EQ(test, pe_export_by_ordinal(&exp, 0), 0);
	KUNIT_EXPECT_EQ(test,
			pe_export_by_ordinal(&exp,
					     NTDLL_EXP_BASE + ARRAY_SIZE(ntdll_syms)),
			0);
}

static struct kunit_case binfmt_pe_test_cases[] = {
	KUNIT_CASE(pe_parse_valid_test),
	KUNIT_CASE(pe_parse_bad_mz_magic_test),
	KUNIT_CASE(pe_parse_bad_peaddr_test),
	KUNIT_CASE(pe_parse_bad_pe_magic_test),
	KUNIT_CASE(pe_parse_wrong_machine_test),
	KUNIT_CASE(pe_parse_not_executable_test),
	KUNIT_CASE(pe_parse_dll_test),
	KUNIT_CASE(pe_parse_pe32_not_plus_test),
	KUNIT_CASE(pe_parse_opt_hdr_too_small_test),
	KUNIT_CASE(pe_parse_section_count_test),
	KUNIT_CASE(pe_parse_bad_alignment_test),
	KUNIT_CASE(pe_parse_subpage_file_align_test),
	KUNIT_CASE(pe_parse_bad_image_base_test),
	KUNIT_CASE(pe_parse_bad_image_size_test),
	KUNIT_CASE(pe_parse_bad_entry_test),
	KUNIT_CASE(pe_parse_header_too_big_test),
	KUNIT_CASE(pe_parse_section_table_overrun_test),
	KUNIT_CASE(pe_export_lookup_test),
	KUNIT_CASE(pe_export_forwarder_test),
	KUNIT_CASE(pe_export_malformed_test),
	KUNIT_CASE(pe_export_name_off_end_test),
	KUNIT_CASE(pe_import_enumerate_test),
	KUNIT_CASE(pe_import_oft_fallback_test),
	KUNIT_CASE(pe_import_malformed_test),
	KUNIT_CASE(pe_ntdll_exports_test),
	{},
};

static struct kunit_suite binfmt_pe_test_suite = {
	.name = "binfmt_pe",
	.test_cases = binfmt_pe_test_cases,
};

kunit_test_suite(binfmt_pe_test_suite);
