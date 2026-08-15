/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Build a real minimal ntdll.dll - the NT services DLL a native PE imports.
 *
 * This is the Phase-B asset: a hand-assembled 64-bit DLL PE whose .text is one
 * syscall stub per NT service and whose export directory publishes the seven
 * Nt* names those stubs implement.  The in-kernel dynamic linker
 * (fs/binfmt_pe.c, nt_load_dll()) loads it from C:\Windows\System32\ntdll.dll,
 * reads this export directory, and - in phase C - patches an importing PE's IAT
 * with the stub addresses so a `call [IAT]` lands in the matching stub.
 *
 * A stub is exactly what a real ntdll stub is:
 *
 *     mov r10, rcx        ; 49 89 ca   - NT arg1 goes in r10, not rcx
 *     mov eax, <service>  ; b8 NN 00 00 00
 *     syscall             ; 0f 05
 *     ret                 ; c3
 *
 * so a caller using the ordinary Win64 call convention (arg1 in rcx) reaches
 * the NT service convention (arg1 in r10, service number in eax) that
 * fs/ntpers/dispatch.c decodes.  The bytes below were assembled with the host
 * assembler; the disassembly is the comment above.
 *
 * The DLL carries IMAGE_FILE_DLL and IMAGE_FILE_RELOCS_STRIPPED and a preferred
 * base of 0x180000000, clear of the 0x140000000 the test executables use, so
 * the loader maps it at its preferred base and never has to relocate it.
 *
 * The layout constants match fs/tests/binfmt_pe_kunit.c (pe_ntdll_exports_test)
 * byte for byte: the KUnit case builds the same export shape in memory and
 * proves the Phase-A export parser resolves every name to its stub RVA, so the
 * asset this header ships and the parser that will read it are checked together.
 *
 * This header is self-contained (its PE structs are named uniquely so a test
 * can also build a separate main image alongside it) and depends only on the
 * C library, so phase C's end-to-end selftest can include it, drop the file at
 * C:\Windows\System32\ntdll.dll in the test rootfs, and bind against it.
 */
#ifndef SELFTESTS_BINFMT_PE_NTDLL_BUILDER_H
#define SELFTESTS_BINFMT_PE_NTDLL_BUILDER_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* PE/COFF constants (see <linux/pe.h>); named locally for a stock toolchain. */
#define NTDLL_MZ_MAGIC			0x5a4d
#define NTDLL_NT_MAGIC			0x00004550
#define NTDLL_MACHINE_AMD64		0x8664
#define NTDLL_OPT_MAGIC_PLUS		0x020b
#define NTDLL_F_RELOCS_STRIPPED		0x0001
#define NTDLL_F_EXECUTABLE_IMAGE	0x0002
#define NTDLL_F_DLL			0x2000
#define NTDLL_SUBSYSTEM_WINDOWS_CUI	3
#define NTDLL_SCN_CNT_CODE		0x00000020
#define NTDLL_SCN_CNT_INITIALIZED_DATA	0x00000040
#define NTDLL_SCN_MEM_EXECUTE		0x20000000
#define NTDLL_SCN_MEM_READ		0x40000000

/*
 * NT service numbers (see <uapi/linux/nt_personality.h>).  Defined locally so
 * the header builds against a stock toolchain, exactly as the sibling selftests
 * carry their own copies of the NT ABI constants.
 */
#define NTDLL_SYS_NtClose			0
#define NTDLL_SYS_NtCreateFile			1
#define NTDLL_SYS_NtOpenFile			2
#define NTDLL_SYS_NtReadFile			3
#define NTDLL_SYS_NtWriteFile			4
#define NTDLL_SYS_NtQueryInformationFile	5
#define NTDLL_SYS_NtTerminateProcess		6

/*
 * Geometry.  Three page-aligned pieces - a header page, a .text page of stubs,
 * and a .edata page of the export directory - with file alignment equal to
 * section alignment, so every file offset equals its RVA.  These offsets match
 * fs/tests/binfmt_pe_kunit.c.
 */
#define NTDLL_PAGE		0x1000UL
#define NTDLL_HDR_OFF		0x80		/* where the PE header sits */
#define NTDLL_DLL_IMAGE_BASE	0x180000000UL	/* clear of 0x140000000 EXEs */
#define NTDLL_TEXT_RVA		0x1000
#define NTDLL_STUB_STRIDE	0x10		/* one 16-byte slot per stub */
#define NTDLL_EDATA_RVA		0x2000
#define NTDLL_EAT_RVA		0x2028		/* u32[7] function RVAs */
#define NTDLL_ENPT_RVA		0x2048		/* u32[7] name RVAs */
#define NTDLL_ORD_RVA		0x2068		/* u16[7] name ordinals */
#define NTDLL_DLLNAME_RVA	0x2080		/* "ntdll.dll" */
#define NTDLL_NAMES_RVA		0x2090		/* packed name strings */
#define NTDLL_EXP_BASE		1		/* ordinal of the first export */
#define NTDLL_DLL_IMAGE_SIZE	(3 * NTDLL_PAGE)

struct __attribute__((packed)) ntdll_file_hdr {
	uint32_t magic;
	uint16_t machine;
	uint16_t sections;
	uint32_t timestamp;
	uint32_t symtab;
	uint32_t nsyms;
	uint16_t opt_hdr_size;
	uint16_t flags;
};

struct __attribute__((packed)) ntdll_opt64 {
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

struct __attribute__((packed)) ntdll_section {
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

/* An IMAGE_EXPORT_DIRECTORY, laid out here field by field. */
struct __attribute__((packed)) ntdll_export_dir {
	uint32_t flags;
	uint32_t timestamp;
	uint16_t major_version;
	uint16_t minor_version;
	uint32_t name;
	uint32_t base;
	uint32_t num_functions;
	uint32_t num_names;
	uint32_t functions;
	uint32_t names;
	uint32_t name_ordinals;
};

/* The exported names, alphabetically sorted, each with its NT service number. */
static const struct {
	const char *name;
	uint8_t svc;
} ntdll_exports[] = {
	{ "NtClose",			NTDLL_SYS_NtClose },
	{ "NtCreateFile",		NTDLL_SYS_NtCreateFile },
	{ "NtOpenFile",			NTDLL_SYS_NtOpenFile },
	{ "NtQueryInformationFile",	NTDLL_SYS_NtQueryInformationFile },
	{ "NtReadFile",			NTDLL_SYS_NtReadFile },
	{ "NtTerminateProcess",		NTDLL_SYS_NtTerminateProcess },
	{ "NtWriteFile",		NTDLL_SYS_NtWriteFile },
};

#define NTDLL_NR_EXPORTS \
	((unsigned int)(sizeof(ntdll_exports) / sizeof(ntdll_exports[0])))

/*
 * Lay out the whole ntdll.dll image into @img (NTDLL_DLL_IMAGE_SIZE bytes) and
 * return the export directory's size in bytes (what a caller stores in the
 * export data directory, already done here).  @img must be little-endian host
 * memory (the loader only ever runs this on AMD64).
 */
static inline uint32_t build_ntdll_dll(uint8_t *img)
{
	struct ntdll_file_hdr *pe;
	struct ntdll_opt64 *opt;
	struct ntdll_section *sec;
	struct ntdll_export_dir *ed;
	uint8_t *dirs;
	uint32_t name_rva = NTDLL_NAMES_RVA;
	uint32_t dir_size;
	unsigned int i;
	uint16_t opt_size = sizeof(*opt) + 16 * 8;	/* + 16 data directories */

	memset(img, 0, NTDLL_DLL_IMAGE_SIZE);

	/* MZ header: signature and the offset of the PE header. */
	img[0] = 'M';
	img[1] = 'Z';
	*(uint32_t *)(img + 0x3c) = NTDLL_HDR_OFF;

	pe = (struct ntdll_file_hdr *)(img + NTDLL_HDR_OFF);
	pe->magic = NTDLL_NT_MAGIC;
	pe->machine = NTDLL_MACHINE_AMD64;
	pe->sections = 2;
	pe->opt_hdr_size = opt_size;
	pe->flags = NTDLL_F_EXECUTABLE_IMAGE | NTDLL_F_DLL |
		    NTDLL_F_RELOCS_STRIPPED;

	opt = (struct ntdll_opt64 *)(img + NTDLL_HDR_OFF + sizeof(*pe));
	opt->magic = NTDLL_OPT_MAGIC_PLUS;
	opt->text_size = NTDLL_NR_EXPORTS * NTDLL_STUB_STRIDE;
	opt->entry_point = 0;			/* no DllMain; never called */
	opt->code_base = NTDLL_TEXT_RVA;
	opt->image_base = NTDLL_DLL_IMAGE_BASE;
	opt->section_align = NTDLL_PAGE;
	opt->file_align = NTDLL_PAGE;
	opt->sub_major = 6;
	opt->image_size = NTDLL_DLL_IMAGE_SIZE;
	opt->header_size = NTDLL_PAGE;
	opt->subsys = NTDLL_SUBSYSTEM_WINDOWS_CUI;
	opt->stack_reserve = 0x100000;
	opt->stack_commit = 0x1000;
	opt->data_dirs = 16;

	/* .text stubs, and the parallel export tables that point at them. */
	for (i = 0; i < NTDLL_NR_EXPORTS; i++) {
		uint32_t stub = NTDLL_TEXT_RVA + i * NTDLL_STUB_STRIDE;
		uint8_t *code = img + stub;

		code[0] = 0x49;			/* mov r10,rcx */
		code[1] = 0x89;
		code[2] = 0xca;
		code[3] = 0xb8;			/* mov eax,<service> */
		*(uint32_t *)(code + 4) = ntdll_exports[i].svc;
		code[8] = 0x0f;			/* syscall */
		code[9] = 0x05;
		code[10] = 0xc3;		/* ret */

		*(uint32_t *)(img + NTDLL_EAT_RVA + i * 4) = stub;
		*(uint32_t *)(img + NTDLL_ENPT_RVA + i * 4) = name_rva;
		*(uint16_t *)(img + NTDLL_ORD_RVA + i * 2) = (uint16_t)i;
		strcpy((char *)(img + name_rva), ntdll_exports[i].name);
		name_rva += (uint32_t)strlen(ntdll_exports[i].name) + 1;
	}

	strcpy((char *)(img + NTDLL_DLLNAME_RVA), "ntdll.dll");

	/* The export directory header. */
	ed = (struct ntdll_export_dir *)(img + NTDLL_EDATA_RVA);
	ed->name = NTDLL_DLLNAME_RVA;
	ed->base = NTDLL_EXP_BASE;
	ed->num_functions = NTDLL_NR_EXPORTS;
	ed->num_names = NTDLL_NR_EXPORTS;
	ed->functions = NTDLL_EAT_RVA;
	ed->names = NTDLL_ENPT_RVA;
	ed->name_ordinals = NTDLL_ORD_RVA;

	dir_size = name_rva - NTDLL_EDATA_RVA;

	/* Data directory [0] is the export table. */
	dirs = (uint8_t *)opt + sizeof(*opt);
	*(uint32_t *)(dirs + 0 * 8 + 0) = NTDLL_EDATA_RVA;
	*(uint32_t *)(dirs + 0 * 8 + 4) = dir_size;

	sec = (struct ntdll_section *)(img + NTDLL_HDR_OFF + sizeof(*pe) +
				       opt_size);
	memcpy(sec[0].name, ".text", 5);
	sec[0].virtual_size = NTDLL_NR_EXPORTS * NTDLL_STUB_STRIDE;
	sec[0].virtual_address = NTDLL_TEXT_RVA;
	sec[0].raw_size = NTDLL_PAGE;
	sec[0].raw_ptr = NTDLL_TEXT_RVA;
	sec[0].flags = NTDLL_SCN_CNT_CODE | NTDLL_SCN_MEM_EXECUTE |
		       NTDLL_SCN_MEM_READ;

	memcpy(sec[1].name, ".edata", 6);
	sec[1].virtual_size = dir_size;
	sec[1].virtual_address = NTDLL_EDATA_RVA;
	sec[1].raw_size = NTDLL_PAGE;
	sec[1].raw_ptr = NTDLL_EDATA_RVA;
	sec[1].flags = NTDLL_SCN_CNT_INITIALIZED_DATA | NTDLL_SCN_MEM_READ;

	return dir_size;
}

/*
 * Build ntdll.dll and write it to @path (creating or truncating).  Returns 0 on
 * success or -1 on any I/O error.  Phase C uses this to place the DLL at
 * C:\Windows\System32\ntdll.dll (i.e. /Windows/System32/ntdll.dll under the C:
 * volume) in the test rootfs before execve()ing an importing PE.
 */
static inline int write_ntdll_dll(const char *path)
{
	uint8_t img[NTDLL_DLL_IMAGE_SIZE];
	FILE *f;

	build_ntdll_dll(img);

	f = fopen(path, "wb");
	if (!f)
		return -1;
	if (fwrite(img, 1, sizeof(img), f) != sizeof(img)) {
		fclose(f);
		return -1;
	}
	if (fclose(f) != 0)
		return -1;
	return 0;
}

#endif /* SELFTESTS_BINFMT_PE_NTDLL_BUILDER_H */
