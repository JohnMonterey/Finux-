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
	{},
};

static struct kunit_suite binfmt_pe_test_suite = {
	.name = "binfmt_pe",
	.test_cases = binfmt_pe_test_cases,
};

kunit_test_suite(binfmt_pe_test_suite);
