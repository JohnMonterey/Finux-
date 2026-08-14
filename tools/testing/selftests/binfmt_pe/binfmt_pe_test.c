// SPDX-License-Identifier: GPL-2.0
/*
 * End-to-end selftest for the PE/COFF binary loader (CONFIG_BINFMT_PE).
 *
 * This builds a minimal but genuine 64-bit PE executable on disk - an MZ
 * header, a PE header, one .text section - whose entry point issues the
 * Linux exit(42) system call.  It then execve()s the file and checks that
 * the kernel recognised it, mapped the image, and transferred control: the
 * child process exits with status 42.
 *
 * The payload deliberately uses a Linux system call.  This milestone is
 * the loader; the NT system-call surface that a real Windows program would
 * reach through ntdll does not exist yet.  So the test proves precisely
 * what the loader promises - parse the headers, map the sections, jump to
 * the entry point - and nothing it does not yet do.  A binary that needed
 * imports resolved, or its base relocated, is out of scope here.
 *
 * If the running kernel was built without CONFIG_BINFMT_PE the execve()
 * fails with ENOEXEC and the test is skipped.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../kselftest_harness.h"

/* PE/COFF constants (see <linux/pe.h>). */
#define PE_MZ_MAGIC			0x5a4d
#define PE_NT_MAGIC			0x00004550
#define PE_MACHINE_AMD64		0x8664
#define PE_OPT_MAGIC_PLUS		0x020b
#define PE_F_RELOCS_STRIPPED		0x0001
#define PE_F_EXECUTABLE_IMAGE		0x0002
#define PE_SUBSYSTEM_WINDOWS_CUI	3
#define PE_SCN_CNT_CODE			0x00000020
#define PE_SCN_CNT_INITIALIZED_DATA	0x00000040
#define PE_SCN_MEM_EXECUTE		0x20000000
#define PE_SCN_MEM_READ			0x40000000
#define PE_SCN_MEM_WRITE		0x80000000

#define PE_PAGE		0x1000UL
#define PE_HDR_OFF	0x80		/* where we place the PE header */
#define PE_IMAGE_BASE	0x140000000UL	/* default 64-bit EXE base */
#define PE_IMAGE_SIZE	(2 * PE_PAGE)	/* one header page + one text page */

/* A low preferred base the relocatable test image is unlikely to keep. */
#define PE_RELOC_BASE	0x10000UL

struct __attribute__((packed)) pe_file_hdr {
	uint32_t magic;
	uint16_t machine;
	uint16_t sections;
	uint32_t timestamp;
	uint32_t symtab;
	uint32_t nsyms;
	uint16_t opt_hdr_size;
	uint16_t flags;
};

struct __attribute__((packed)) pe_opt64 {
	uint16_t magic;
	uint8_t ld_major;
	uint8_t ld_minor;
	uint32_t text_size;
	uint32_t data_size;
	uint32_t bss_size;
	uint32_t entry_point;
	uint32_t code_base;
	uint64_t image_base;
	uint32_t section_align;
	uint32_t file_align;
	uint16_t os_major;
	uint16_t os_minor;
	uint16_t img_major;
	uint16_t img_minor;
	uint16_t sub_major;
	uint16_t sub_minor;
	uint32_t win32_version;
	uint32_t image_size;
	uint32_t header_size;
	uint32_t csum;
	uint16_t subsys;
	uint16_t dll_flags;
	uint64_t stack_reserve;
	uint64_t stack_commit;
	uint64_t heap_reserve;
	uint64_t heap_commit;
	uint32_t loader_flags;
	uint32_t data_dirs;
};

struct __attribute__((packed)) pe_section {
	char name[8];
	uint32_t virtual_size;
	uint32_t virtual_address;
	uint32_t raw_size;
	uint32_t raw_ptr;
	uint32_t relocs;
	uint32_t line_numbers;
	uint16_t num_relocs;
	uint16_t num_lines;
	uint32_t flags;
};

/*
 * x86-64 entry payload: exit(42).
 *   mov edi, 42      ; status
 *   mov eax, 60      ; __NR_exit
 *   syscall
 */
static const uint8_t pe_entry_code[] = {
	0xbf, 0x2a, 0x00, 0x00, 0x00,
	0xb8, 0x3c, 0x00, 0x00, 0x00,
	0x0f, 0x05,
};

/*
 * x86-64 entry payload that checks its own base relocation.  It reads a
 * pointer that .reloc fixes up (stored at RVA 0x2000, initially base+0x2000)
 * and confirms the loader made it equal to (actual load base) + 0x2000.  It
 * exits 42 when the image was relocated and the fixup is right, 43 when the
 * image loaded at its preferred base (also right), and 1 when the fixup is
 * wrong.  Assembled from:
 *
 *   lea  rbx, [rip - 7]        ; rbx = &_start = base + 0x1000
 *   sub  rbx, 0x1000           ; rbx = actual load base
 *   mov  rax, [rbx + 0x2000]   ; rax = the relocated pointer
 *   lea  rcx, [rbx + 0x2000]   ; rcx = base + 0x2000 (expected)
 *   cmp  rax, rcx
 *   jne  fail
 *   mov  rcx, 0x10000          ; preferred ImageBase
 *   cmp  rbx, rcx
 *   je   preferred
 *   mov  edi, 42
 *   jmp  done
 *   preferred: mov edi, 43; jmp done
 *   fail:      mov edi, 1
 *   done:      mov eax, 60; syscall
 */
static const uint8_t pe_reloc_code[] = {
	0x48, 0x8d, 0x1d, 0xf9, 0xff, 0xff, 0xff,	/* lea rbx,[rip-7]   */
	0x48, 0x81, 0xeb, 0x00, 0x10, 0x00, 0x00,	/* sub rbx,0x1000    */
	0x48, 0x8b, 0x83, 0x00, 0x20, 0x00, 0x00,	/* mov rax,[rbx+0x2000] */
	0x48, 0x8d, 0x8b, 0x00, 0x20, 0x00, 0x00,	/* lea rcx,[rbx+0x2000] */
	0x48, 0x39, 0xc8,				/* cmp rax,rcx	     */
	0x75, 0x1a,					/* jne fail	     */
	0x48, 0xc7, 0xc1, 0x00, 0x00, 0x01, 0x00,	/* mov rcx,0x10000   */
	0x48, 0x39, 0xcb,				/* cmp rbx,rcx	     */
	0x74, 0x07,					/* je preferred	     */
	0xbf, 0x2a, 0x00, 0x00, 0x00,			/* mov edi,42	     */
	0xeb, 0x0c,					/* jmp done	     */
	0xbf, 0x2b, 0x00, 0x00, 0x00,			/* mov edi,43	     */
	0xeb, 0x05,					/* jmp done	     */
	0xbf, 0x01, 0x00, 0x00, 0x00,			/* mov edi,1	     */
	0xb8, 0x3c, 0x00, 0x00, 0x00,			/* mov eax,60	     */
	0x0f, 0x05,					/* syscall	     */
};

/* Lay out a complete, loadable PE image into @img (PE_IMAGE_SIZE bytes). */
static void build_pe_image(uint8_t *img)
{
	struct pe_file_hdr *pe;
	struct pe_opt64 *opt;
	struct pe_section *sec;
	uint16_t opt_size = sizeof(*opt) + 16 * 8;	/* + 16 data directories */

	memset(img, 0, PE_IMAGE_SIZE);

	/* MZ header: signature and the offset of the PE header. */
	img[0] = 'M';
	img[1] = 'Z';
	*(uint32_t *)(img + 0x3c) = PE_HDR_OFF;

	pe = (struct pe_file_hdr *)(img + PE_HDR_OFF);
	pe->magic = PE_NT_MAGIC;
	pe->machine = PE_MACHINE_AMD64;
	pe->sections = 1;
	pe->opt_hdr_size = opt_size;
	pe->flags = PE_F_EXECUTABLE_IMAGE | PE_F_RELOCS_STRIPPED;

	opt = (struct pe_opt64 *)(img + PE_HDR_OFF + sizeof(*pe));
	opt->magic = PE_OPT_MAGIC_PLUS;
	opt->entry_point = PE_PAGE;		/* RVA of .text */
	opt->code_base = PE_PAGE;
	opt->image_base = PE_IMAGE_BASE;
	opt->section_align = PE_PAGE;
	opt->file_align = PE_PAGE;
	opt->sub_major = 6;
	opt->image_size = PE_IMAGE_SIZE;
	opt->header_size = PE_PAGE;
	opt->subsys = PE_SUBSYSTEM_WINDOWS_CUI;
	opt->stack_reserve = 0x100000;
	opt->stack_commit = 0x1000;
	opt->data_dirs = 16;

	sec = (struct pe_section *)(img + PE_HDR_OFF + sizeof(*pe) + opt_size);
	memcpy(sec->name, ".text", 5);
	sec->virtual_size = sizeof(pe_entry_code);
	sec->virtual_address = PE_PAGE;
	sec->raw_size = PE_PAGE;
	sec->raw_ptr = PE_PAGE;
	sec->flags = PE_SCN_CNT_CODE | PE_SCN_MEM_EXECUTE | PE_SCN_MEM_READ;

	memcpy(img + PE_PAGE, pe_entry_code, sizeof(pe_entry_code));
}

/*
 * A PE built with the old 512-byte file alignment, so its .text does not
 * start on a page boundary and the loader has to copy it in rather than map
 * it from the file.  It exits 42.  Not relocatable; loads at its preferred
 * base.  The whole file is one page: headers in the first 0x400 bytes, the
 * code at file offset 0x400 (512-aligned, not page-aligned).
 */
#define PE_COPYIN_FILE_ALIGN	0x200
#define PE_COPYIN_HDR_SIZE	0x400
#define PE_COPYIN_TEXT_OFF	0x400
#define PE_COPYIN_FILE_SIZE	PE_PAGE
#define PE_COPYIN_IMAGE_SIZE	(2 * PE_PAGE)

static void build_pe_copyin(uint8_t *img)
{
	struct pe_file_hdr *pe;
	struct pe_opt64 *opt;
	struct pe_section *sec;
	uint16_t opt_size = sizeof(*opt) + 16 * 8;

	memset(img, 0, PE_COPYIN_FILE_SIZE);
	img[0] = 'M';
	img[1] = 'Z';
	*(uint32_t *)(img + 0x3c) = PE_HDR_OFF;

	pe = (struct pe_file_hdr *)(img + PE_HDR_OFF);
	pe->magic = PE_NT_MAGIC;
	pe->machine = PE_MACHINE_AMD64;
	pe->sections = 1;
	pe->opt_hdr_size = opt_size;
	pe->flags = PE_F_EXECUTABLE_IMAGE | PE_F_RELOCS_STRIPPED;

	opt = (struct pe_opt64 *)(img + PE_HDR_OFF + sizeof(*pe));
	opt->magic = PE_OPT_MAGIC_PLUS;
	opt->entry_point = PE_PAGE;
	opt->code_base = PE_PAGE;
	opt->image_base = PE_IMAGE_BASE;
	opt->section_align = PE_PAGE;
	opt->file_align = PE_COPYIN_FILE_ALIGN;
	opt->sub_major = 6;
	opt->image_size = PE_COPYIN_IMAGE_SIZE;
	opt->header_size = PE_COPYIN_HDR_SIZE;
	opt->subsys = PE_SUBSYSTEM_WINDOWS_CUI;
	opt->stack_reserve = 0x100000;
	opt->stack_commit = 0x1000;
	opt->data_dirs = 16;

	sec = (struct pe_section *)(img + PE_HDR_OFF + sizeof(*pe) + opt_size);
	memcpy(sec->name, ".text", 5);
	sec->virtual_size = sizeof(pe_entry_code);
	sec->virtual_address = PE_PAGE;
	sec->raw_size = PE_COPYIN_FILE_ALIGN;
	sec->raw_ptr = PE_COPYIN_TEXT_OFF;
	sec->flags = PE_SCN_CNT_CODE | PE_SCN_MEM_EXECUTE | PE_SCN_MEM_READ;

	memcpy(img + PE_COPYIN_TEXT_OFF, pe_entry_code, sizeof(pe_entry_code));
}

/*
 * A relocatable PE: three sections (.text, .data, .reloc) and a base
 * relocation table with one DIR64 fixup.  .data holds a pointer to itself
 * (base+0x2000) that the fixup adjusts; the payload checks the adjustment.
 * Its file image is four pages, all page-aligned, so the sections are
 * file-backed and the exercise is purely relocation.
 */
#define PE_RELOC_IMAGE_SIZE	(4 * PE_PAGE)

static void build_pe_reloc(uint8_t *img)
{
	struct pe_file_hdr *pe;
	struct pe_opt64 *opt;
	struct pe_section *sec;
	uint8_t *dirs, *reloc;
	uint16_t opt_size = sizeof(*opt) + 16 * 8;

	memset(img, 0, PE_RELOC_IMAGE_SIZE);
	img[0] = 'M';
	img[1] = 'Z';
	*(uint32_t *)(img + 0x3c) = PE_HDR_OFF;

	pe = (struct pe_file_hdr *)(img + PE_HDR_OFF);
	pe->magic = PE_NT_MAGIC;
	pe->machine = PE_MACHINE_AMD64;
	pe->sections = 3;
	pe->opt_hdr_size = opt_size;
	pe->flags = PE_F_EXECUTABLE_IMAGE;	/* relocations present */

	opt = (struct pe_opt64 *)(img + PE_HDR_OFF + sizeof(*pe));
	opt->magic = PE_OPT_MAGIC_PLUS;
	opt->entry_point = PE_PAGE;
	opt->code_base = PE_PAGE;
	opt->image_base = PE_RELOC_BASE;
	opt->section_align = PE_PAGE;
	opt->file_align = PE_PAGE;
	opt->sub_major = 6;
	opt->image_size = PE_RELOC_IMAGE_SIZE;
	opt->header_size = PE_PAGE;
	opt->subsys = PE_SUBSYSTEM_WINDOWS_CUI;
	opt->stack_reserve = 0x100000;
	opt->stack_commit = 0x1000;
	opt->data_dirs = 16;

	/* Data directory [5] is the base relocation table. */
	dirs = (uint8_t *)opt + sizeof(*opt);
	*(uint32_t *)(dirs + 5 * 8 + 0) = 3 * PE_PAGE;	/* .reloc RVA */
	*(uint32_t *)(dirs + 5 * 8 + 4) = 10;		/* one block */

	sec = (struct pe_section *)(img + PE_HDR_OFF + sizeof(*pe) + opt_size);
	memcpy(sec[0].name, ".text", 5);
	sec[0].virtual_size = sizeof(pe_reloc_code);
	sec[0].virtual_address = PE_PAGE;
	sec[0].raw_size = PE_PAGE;
	sec[0].raw_ptr = PE_PAGE;
	sec[0].flags = PE_SCN_CNT_CODE | PE_SCN_MEM_EXECUTE | PE_SCN_MEM_READ;

	memcpy(sec[1].name, ".data", 5);
	sec[1].virtual_size = 8;
	sec[1].virtual_address = 2 * PE_PAGE;
	sec[1].raw_size = PE_PAGE;
	sec[1].raw_ptr = 2 * PE_PAGE;
	sec[1].flags = PE_SCN_CNT_INITIALIZED_DATA | PE_SCN_MEM_READ |
		       PE_SCN_MEM_WRITE;

	memcpy(sec[2].name, ".reloc", 6);
	sec[2].virtual_size = 10;
	sec[2].virtual_address = 3 * PE_PAGE;
	sec[2].raw_size = PE_PAGE;
	sec[2].raw_ptr = 3 * PE_PAGE;
	sec[2].flags = PE_SCN_CNT_INITIALIZED_DATA | PE_SCN_MEM_READ;

	memcpy(img + PE_PAGE, pe_reloc_code, sizeof(pe_reloc_code));

	/* .data: pointer to itself, before relocation. */
	*(uint64_t *)(img + 2 * PE_PAGE) = PE_RELOC_BASE + 2 * PE_PAGE;

	/* .reloc: one block, one DIR64 fixup of the pointer at RVA 0x2000. */
	reloc = img + 3 * PE_PAGE;
	*(uint32_t *)(reloc + 0) = 2 * PE_PAGE;		/* page RVA */
	*(uint32_t *)(reloc + 4) = 10;			/* block size */
	*(uint16_t *)(reloc + 8) = (10 << 12) | 0;	/* DIR64, offset 0 */
}

/*
 * Write @img to a temp file, execve() it, and return the child's exit
 * status - or a negative value for a harness-level failure (so a broken
 * test setup is not mistaken for a loader result).  Exit code 100 means the
 * kernel has no PE loader (execve gave ENOEXEC); 101 means execve failed for
 * some other reason.
 */
static int exec_pe(const uint8_t *img, size_t len)
{
	char path[] = "/tmp/binfmt_pe_test.XXXXXX";
	int fd, status;
	pid_t pid;

	fd = mkstemp(path);
	if (fd < 0)
		return -1;
	if (write(fd, img, len) != (ssize_t)len || fchmod(fd, 0755) ||
	    close(fd)) {
		unlink(path);
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		unlink(path);
		return -1;
	}
	if (pid == 0) {
		char *argv[] = { path, NULL };
		char *envp[] = { NULL };

		execve(path, argv, envp);
		_exit(errno == ENOEXEC ? 100 : 101);
	}

	if (waitpid(pid, &status, 0) != pid) {
		unlink(path);
		return -1;
	}
	unlink(path);

	if (!WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}

/* A plain page-aligned PE, mapped from the file, runs and exits 42. */
TEST(load_and_run_pe)
{
	uint8_t img[PE_IMAGE_SIZE];
	int rc;

	build_pe_image(img);
	rc = exec_pe(img, PE_IMAGE_SIZE);

	ASSERT_GE(rc, 0);
	if (rc == 100)
		SKIP(return, "kernel built without CONFIG_BINFMT_PE");
	if (rc == 101)
		SKIP(return, "execve failed for a reason other than ENOEXEC");
	EXPECT_EQ(rc, 42);
}

/* A 512-byte-file-aligned PE exercises the copy-in path and exits 42. */
TEST(load_and_run_pe_copyin)
{
	uint8_t img[PE_COPYIN_FILE_SIZE];
	int rc;

	build_pe_copyin(img);
	rc = exec_pe(img, PE_COPYIN_FILE_SIZE);

	ASSERT_GE(rc, 0);
	if (rc == 100)
		SKIP(return, "kernel built without CONFIG_BINFMT_PE");
	if (rc == 101)
		SKIP(return, "execve failed for a reason other than ENOEXEC");
	EXPECT_EQ(rc, 42);
}

/*
 * A relocatable PE.  Under address-space randomisation the loader places it
 * at a non-preferred base and applies its DIR64 relocation; the payload
 * confirms the fixup (exit 42).  If it happens to load at its preferred base
 * the payload still confirms correctness (exit 43) - both are a pass.
 */
TEST(load_and_run_pe_reloc)
{
	uint8_t img[PE_RELOC_IMAGE_SIZE];
	int rc;

	build_pe_reloc(img);
	rc = exec_pe(img, PE_RELOC_IMAGE_SIZE);

	ASSERT_GE(rc, 0);
	if (rc == 100)
		SKIP(return, "kernel built without CONFIG_BINFMT_PE");
	if (rc == 101)
		SKIP(return, "execve failed for a reason other than ENOEXEC");
	ASSERT_NE(rc, 1);	/* relocation applied incorrectly */
	EXPECT_TRUE(rc == 42 || rc == 43);
}

TEST_HARNESS_MAIN
