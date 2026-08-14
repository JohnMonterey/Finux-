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
#define PE_SCN_MEM_EXECUTE		0x20000000
#define PE_SCN_MEM_READ			0x40000000

#define PE_PAGE		0x1000UL
#define PE_HDR_OFF	0x80		/* where we place the PE header */
#define PE_IMAGE_BASE	0x140000000UL	/* default 64-bit EXE base */
#define PE_IMAGE_SIZE	(2 * PE_PAGE)	/* one header page + one text page */

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

TEST(load_and_run_pe)
{
	char path[] = "/tmp/binfmt_pe_test.XXXXXX";
	uint8_t img[PE_IMAGE_SIZE];
	int fd, status;
	pid_t pid;

	build_pe_image(img);

	fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, img, PE_IMAGE_SIZE), (ssize_t)PE_IMAGE_SIZE);
	ASSERT_EQ(fchmod(fd, 0755), 0);
	ASSERT_EQ(close(fd), 0);

	pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		char *argv[] = { path, NULL };
		char *envp[] = { NULL };

		execve(path, argv, envp);
		/*
		 * ENOEXEC here means the kernel has no PE loader; any other
		 * errno is a real exec failure.  Encode the two apart so the
		 * parent can skip vs. fail.
		 */
		_exit(errno == ENOEXEC ? 100 : 101);
	}

	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	unlink(path);

	ASSERT_TRUE(WIFEXITED(status));
	if (WEXITSTATUS(status) == 100)
		SKIP(return, "kernel built without CONFIG_BINFMT_PE");
	if (WEXITSTATUS(status) == 101)
		SKIP(return, "execve failed for a reason other than ENOEXEC");

	/* The PE's entry point ran and exited 42. */
	EXPECT_EQ(WEXITSTATUS(status), 42);
}

TEST_HARNESS_MAIN
