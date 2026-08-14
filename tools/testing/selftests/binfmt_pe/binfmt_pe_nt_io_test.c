// SPDX-License-Identifier: GPL-2.0
/*
 * End-to-end selftest: a real native PE, loaded by execve() through
 * binfmt_pe, does genuine file I/O entirely through NT system calls, and the
 * file it wrote is verified from the ordinary Linux side.
 *
 * This is the flagship of the NT system-call surface.  Nothing here uses
 * prctl and nothing uses Wine: the PE is an NT process from its very first
 * instruction because the loader made it one (fs/binfmt_pe.c,
 * pe_setup_nt_process() - NT syscall mode on, NT personality set, %gs at a
 * minimal TEB/PEB).  Its entry payload, in pure position-independent machine
 * code that makes no Linux system call, drives the Windows x64 service
 * convention directly:
 *
 *   NtCreateFile("C:\ntpedemo.dat", GENERIC_WRITE, share=0, FILE_OVERWRITE_IF)
 *     -> NtWriteFile(<known pattern>) -> NtClose
 *     -> NtOpenFile(same path, GENERIC_READ) -> NtReadFile
 *     -> compare byte-for-byte -> NtTerminateProcess(NT_CURRENT_PROCESS, 42)
 *
 * The child exits 42 on a full round trip, or a distinct small status naming
 * the step that failed (11..18).  The harness - an ordinary Linux process,
 * never in NT mode - waitpid()s and asserts 42, and then opens the file the
 * NT process created from the Linux side: "C:\ntpedemo.dat" maps to
 * "/ntpedemo.dat" on the root filesystem, so a plain open()/read() sees the
 * exact bytes the Windows binary wrote.  That the two sides agree is the whole
 * point: a Windows binary created a real file through the kernel's NT layer.
 *
 * A C: volume must exist for "C:\..." to resolve; the harness registers one
 * over the root filesystem through the personality's debugfs control file,
 * exactly as the nt_syscall selftest does, and skips if that interface is
 * absent (a kernel without the NT personality).  If the kernel also lacks the
 * PE loader the execve() fails with ENOEXEC and the test is skipped.
 *
 * The same binary doubles as a UML init: built static and booted as /init it
 * runs the harness, prints its TAP result to the console and powers off.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
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

/*
 * NT ABI constants (see <uapi/linux/nt_personality.h>).  Defined locally so
 * the test builds against a stock toolchain, the way the sibling binfmt_pe and
 * nt_syscall selftests carry their own copies.
 */
#define NT_OBJ_CASE_INSENSITIVE		0x00000040U

/*
 * PE geometry.  The image is three page-aligned pieces - a header page, a
 * .text page (the payload) and a .data page (its NT structs and buffers) - and
 * carries RELOCS_STRIPPED, so the loader maps it at its preferred ImageBase and
 * never relocates it.  That is what lets the payload reference its own .data by
 * absolute addresses (ImageBase + RVA) baked into the code at build time.
 */
#define PE_PAGE			0x1000UL
#define PE_HDR_OFF		0x80		/* where the PE header sits */
#define NTIO_IMAGE_BASE		0x140000000UL	/* default 64-bit EXE base */
#define NTIO_TEXT_RVA		0x1000
#define NTIO_DATA_RVA		0x2000
#define NTIO_IMAGE_SIZE		(3 * PE_PAGE)	/* header + text + data */
#define NTIO_DATA_BASE		(NTIO_IMAGE_BASE + NTIO_DATA_RVA)

/*
 * .data field offsets, relative to the start of the .data section.  The
 * payload's machine code has the absolute address (NTIO_DATA_BASE + offset) of
 * each of these baked in, so these must match payload.S byte for byte.
 */
#define D_HANDLE1		0x00	/* HANDLE from the write open */
#define D_HANDLE2		0x08	/* HANDLE from the read open */
#define D_IOSB			0x10	/* IO_STATUS_BLOCK (reused per call) */
#define D_OA			0x20	/* OBJECT_ATTRIBUTES */
#define D_US			0x50	/* UNICODE_STRING */
#define D_WPATH			0x60	/* the path, UTF-16LE */
#define D_OFFSET		0x80	/* a zero LARGE_INTEGER ByteOffset */
#define D_PATTERN		0x88	/* the bytes written */
#define D_RDBUF			0xa8	/* where the bytes are read back */
#define NTIO_DATA_USED		0xc8	/* end of the fields above */

/* The NT path the PE creates, and its Linux-visible equivalent. */
static const char nt_path[] = "C:\\ntpedemo.dat";
#define LINUX_PATH		"/ntpedemo.dat"

/*
 * The known byte pattern the PE writes and the harness verifies.  Exactly
 * NTIO_PATLEN bytes; the payload's Length arguments carry NTIO_PATLEN as an
 * immediate, so its size is fixed at assembly time and must not change.
 */
#define NTIO_PATLEN		32
static const uint8_t ntio_pattern[NTIO_PATLEN] = "Finux PE via NT syscalls! 012345";

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

/* The NtCreateFile marshalling structs, byte-for-byte (see fs/ntpers). */
struct nt_unicode_string {
	uint16_t length;
	uint16_t maximum_length;
	uint32_t pad;
	uint64_t buffer;
};

struct nt_object_attributes {
	uint32_t length;
	uint32_t pad0;
	uint64_t root_directory;
	uint64_t object_name;
	uint32_t attributes;
	uint32_t pad1;
	uint64_t security_descriptor;
	uint64_t security_qos;
};

_Static_assert(sizeof(struct nt_unicode_string) == 16, "UNICODE_STRING is 16 bytes");
_Static_assert(sizeof(struct nt_object_attributes) == 48, "OBJECT_ATTRIBUTES is 48 bytes");

/*
 * The entry payload, running as an NT process from instruction one.  Assembled
 * with the host assembler (as) from payload.S and pasted here as raw bytes, the
 * way the sibling binfmt_pe payloads are; the full disassembly is kept below.
 *
 * Every argument-register and stack-argument load follows the Windows x64
 * service convention: service number in eax; arguments one to four in r10, rdx,
 * r8, r9; arguments five and up on the stack, argument five at [rsp+0x28] (an
 * 8-byte return-address slot plus the four-slot 0x20 shadow space below it).
 * NtCreateFile is the widest at eleven arguments, so its seven stack arguments
 * fill [rsp+0x28] through [rsp+0x58]; the payload subtracts 0x100 from rsp once
 * at entry so those slots are fresh scratch below the initial stack top.  Every
 * .data reference (rax:, rcx:, r10:, rsi:, rdi: below) is an absolute
 * NTIO_DATA_BASE + offset immediate, valid because RELOCS_STRIPPED pins the
 * image at NTIO_IMAGE_BASE == 0x140000000, so .data sits at 0x140002000.
 *
 *   _start:
 *     sub    rsp,0x100
 *   ; 1. NtCreateFile(&h1, GENERIC_WRITE, &oa, &iosb, NULL,
 *   ;                 NORMAL, share=0, OVERWRITE_IF, 0, NULL, 0)
 *     mov    QWORD PTR [rsp+0x28],0x0     ; AllocationSize = NULL
 *     mov    QWORD PTR [rsp+0x30],0x80    ; FileAttributes = NORMAL
 *     mov    QWORD PTR [rsp+0x38],0x0     ; ShareAccess = 0 (exclusive)
 *     mov    QWORD PTR [rsp+0x40],0x5     ; CreateDisposition = FILE_OVERWRITE_IF
 *     mov    QWORD PTR [rsp+0x48],0x0     ; CreateOptions = 0
 *     mov    QWORD PTR [rsp+0x50],0x0     ; EaBuffer = NULL
 *     mov    QWORD PTR [rsp+0x58],0x0     ; EaLength = 0
 *     movabs r10,0x140002000              ; &handle1
 *     mov    edx,0x40000000               ; GENERIC_WRITE
 *     movabs r8,0x140002020               ; &oa
 *     movabs r9,0x140002010               ; &iosb
 *     mov    eax,0x1                       ; NtCreateFile
 *     syscall
 *     test   eax,eax
 *     jne    fail_create
 *   ; 2. NtWriteFile(h1, 0,0,0, &iosb, PATTERN, 32, &offset0, NULL)
 *     movabs rax,0x140002010 ; mov [rsp+0x28],rax   ; IoStatusBlock
 *     movabs rax,0x140002088 ; mov [rsp+0x30],rax   ; Buffer = pattern
 *     mov    QWORD PTR [rsp+0x38],0x20             ; Length = 32
 *     movabs rax,0x140002080 ; mov [rsp+0x40],rax   ; ByteOffset -> 0
 *     mov    QWORD PTR [rsp+0x48],0x0              ; Key = NULL
 *     movabs rcx,0x140002000 ; mov r10,[rcx]        ; handle value
 *     xor    edx,edx ; xor r8d,r8d ; xor r9d,r9d
 *     mov    eax,0x4                                ; NtWriteFile
 *     syscall
 *     test   eax,eax ; jne fail_write
 *     movabs rcx,0x140002010 ; mov rax,[rcx+0x8]    ; iosb.Information
 *     cmp    rax,0x20 ; jne fail_wshort             ; short write?
 *   ; 3. NtClose(h1)
 *     movabs rcx,0x140002000 ; mov r10,[rcx]
 *     mov    eax,0x0                                ; NtClose
 *     syscall
 *     test   eax,eax ; jne fail_close
 *   ; 4. NtOpenFile(&h2, GENERIC_READ, &oa, &iosb, SHARE_RW, 0)
 *     mov    QWORD PTR [rsp+0x28],0x3              ; ShareAccess = READ|WRITE
 *     mov    QWORD PTR [rsp+0x30],0x0              ; OpenOptions = 0
 *     movabs r10,0x140002008                        ; &handle2
 *     mov    edx,0x80000000                         ; GENERIC_READ
 *     movabs r8,0x140002020 ; movabs r9,0x140002010
 *     mov    eax,0x2                                ; NtOpenFile
 *     syscall
 *     test   eax,eax ; jne fail_open
 *   ; 5. NtReadFile(h2, 0,0,0, &iosb, RDBUF, 32, &offset0, NULL)
 *     movabs rax,0x140002010 ; mov [rsp+0x28],rax   ; IoStatusBlock
 *     movabs rax,0x1400020a8 ; mov [rsp+0x30],rax   ; Buffer = rdbuf
 *     mov    QWORD PTR [rsp+0x38],0x20             ; Length = 32
 *     movabs rax,0x140002080 ; mov [rsp+0x40],rax   ; ByteOffset -> 0
 *     mov    QWORD PTR [rsp+0x48],0x0              ; Key = NULL
 *     movabs rcx,0x140002008 ; mov r10,[rcx]        ; handle value
 *     xor    edx,edx ; xor r8d,r8d ; xor r9d,r9d
 *     mov    eax,0x3                                ; NtReadFile
 *     syscall
 *     test   eax,eax ; jne fail_read
 *     movabs rcx,0x140002010 ; mov rax,[rcx+0x8]
 *     cmp    rax,0x20 ; jne fail_rshort             ; short read?
 *   ; 6. compare rdbuf vs pattern, 32 bytes
 *     movabs rsi,0x140002088 ; movabs rdi,0x1400020a8 ; mov ecx,0x20
 *   cmp_loop:
 *     mov    al,[rsi] ; cmp al,[rdi] ; jne fail_cmp
 *     inc    rsi ; inc rdi ; dec ecx ; jne cmp_loop
 *     mov    edx,0x2a                               ; status 42: round-trip ok
 *     jmp    term
 *   fail_create:  mov edx,0xb  ; jmp term           ; 11
 *   fail_write:   mov edx,0xc  ; jmp term           ; 12
 *   fail_wshort:  mov edx,0xd  ; jmp term           ; 13
 *   fail_close:   mov edx,0xe  ; jmp term           ; 14
 *   fail_open:    mov edx,0xf  ; jmp term           ; 15
 *   fail_read:    mov edx,0x10 ; jmp term           ; 16
 *   fail_rshort:  mov edx,0x11 ; jmp term           ; 17
 *   fail_cmp:     mov edx,0x12                        ; 18
 *   term:
 *     mov    eax,0x6                                ; NtTerminateProcess
 *     mov    r10,-1                                 ; NT_CURRENT_PROCESS
 *     syscall
 *     hlt
 */
static const uint8_t nt_io_code[] = {
	0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x28,
	0x00, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x30, 0x80, 0x00, 0x00,
	0x00, 0x48, 0xc7, 0x44, 0x24, 0x38, 0x00, 0x00, 0x00, 0x00, 0x48, 0xc7,
	0x44, 0x24, 0x40, 0x05, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x48,
	0x00, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x50, 0x00, 0x00, 0x00,
	0x00, 0x48, 0xc7, 0x44, 0x24, 0x58, 0x00, 0x00, 0x00, 0x00, 0x49, 0xba,
	0x00, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0xba, 0x00, 0x00, 0x00,
	0x40, 0x49, 0xb8, 0x20, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x49,
	0xb9, 0x10, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0xb8, 0x01, 0x00,
	0x00, 0x00, 0x0f, 0x05, 0x85, 0xc0, 0x0f, 0x85, 0x7e, 0x01, 0x00, 0x00,
	0x48, 0xb8, 0x10, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x48, 0x89,
	0x44, 0x24, 0x28, 0x48, 0xb8, 0x88, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00,
	0x00, 0x48, 0x89, 0x44, 0x24, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x38, 0x20,
	0x00, 0x00, 0x00, 0x48, 0xb8, 0x80, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00,
	0x00, 0x48, 0x89, 0x44, 0x24, 0x40, 0x48, 0xc7, 0x44, 0x24, 0x48, 0x00,
	0x00, 0x00, 0x00, 0x48, 0xb9, 0x00, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00,
	0x00, 0x4c, 0x8b, 0x11, 0x31, 0xd2, 0x45, 0x31, 0xc0, 0x45, 0x31, 0xc9,
	0xb8, 0x04, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x85, 0xc0, 0x0f, 0x85, 0x22,
	0x01, 0x00, 0x00, 0x48, 0xb9, 0x10, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00,
	0x00, 0x48, 0x8b, 0x41, 0x08, 0x48, 0x83, 0xf8, 0x20, 0x0f, 0x85, 0x11,
	0x01, 0x00, 0x00, 0x48, 0xb9, 0x00, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00,
	0x00, 0x4c, 0x8b, 0x11, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x85,
	0xc0, 0x0f, 0x85, 0xfc, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x28,
	0x03, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x30, 0x00, 0x00, 0x00,
	0x00, 0x49, 0xba, 0x08, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0xba,
	0x00, 0x00, 0x00, 0x80, 0x49, 0xb8, 0x20, 0x20, 0x00, 0x40, 0x01, 0x00,
	0x00, 0x00, 0x49, 0xb9, 0x10, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
	0xb8, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x85, 0xc0, 0x0f, 0x85, 0xbf,
	0x00, 0x00, 0x00, 0x48, 0xb8, 0x10, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00,
	0x00, 0x48, 0x89, 0x44, 0x24, 0x28, 0x48, 0xb8, 0xa8, 0x20, 0x00, 0x40,
	0x01, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24, 0x30, 0x48, 0xc7, 0x44,
	0x24, 0x38, 0x20, 0x00, 0x00, 0x00, 0x48, 0xb8, 0x80, 0x20, 0x00, 0x40,
	0x01, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44, 0x24, 0x40, 0x48, 0xc7, 0x44,
	0x24, 0x48, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb9, 0x08, 0x20, 0x00, 0x40,
	0x01, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x11, 0x31, 0xd2, 0x45, 0x31, 0xc0,
	0x45, 0x31, 0xc9, 0xb8, 0x03, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x85, 0xc0,
	0x75, 0x67, 0x48, 0xb9, 0x10, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
	0x48, 0x8b, 0x41, 0x08, 0x48, 0x83, 0xf8, 0x20, 0x75, 0x5a, 0x48, 0xbe,
	0x88, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x48, 0xbf, 0xa8, 0x20,
	0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0xb9, 0x20, 0x00, 0x00, 0x00, 0x8a,
	0x06, 0x3a, 0x07, 0x75, 0x42, 0x48, 0xff, 0xc6, 0x48, 0xff, 0xc7, 0xff,
	0xc9, 0x75, 0xf0, 0xba, 0x2a, 0x00, 0x00, 0x00, 0xeb, 0x36, 0xba, 0x0b,
	0x00, 0x00, 0x00, 0xeb, 0x2f, 0xba, 0x0c, 0x00, 0x00, 0x00, 0xeb, 0x28,
	0xba, 0x0d, 0x00, 0x00, 0x00, 0xeb, 0x21, 0xba, 0x0e, 0x00, 0x00, 0x00,
	0xeb, 0x1a, 0xba, 0x0f, 0x00, 0x00, 0x00, 0xeb, 0x13, 0xba, 0x10, 0x00,
	0x00, 0x00, 0xeb, 0x0c, 0xba, 0x11, 0x00, 0x00, 0x00, 0xeb, 0x05, 0xba,
	0x12, 0x00, 0x00, 0x00, 0xb8, 0x06, 0x00, 0x00, 0x00, 0x49, 0xc7, 0xc2,
	0xff, 0xff, 0xff, 0xff, 0x0f, 0x05, 0xf4,
};

/*
 * Lay out the complete PE image into @img (NTIO_IMAGE_SIZE bytes): the MZ/PE
 * headers, a .text section holding the payload, and a .data section holding the
 * NT structs and buffers the payload reaches by absolute address.  Every
 * pointer stored in .data is computed here as NTIO_DATA_BASE + offset, the same
 * absolute value the payload's machine code carries.
 */
static void build_nt_io_pe(uint8_t *img)
{
	struct nt_object_attributes oa = {};
	struct nt_unicode_string us = {};
	struct pe_file_hdr *pe;
	struct pe_opt64 *opt;
	struct pe_section *sec;
	uint16_t opt_size = sizeof(*opt) + 16 * 8;	/* + 16 data directories */
	uint16_t wlen_bytes = (uint16_t)((sizeof(nt_path) - 1) * 2);
	uint8_t *data;
	unsigned int i;

	memset(img, 0, NTIO_IMAGE_SIZE);

	/* MZ header: signature and the offset of the PE header. */
	img[0] = 'M';
	img[1] = 'Z';
	*(uint32_t *)(img + 0x3c) = PE_HDR_OFF;

	pe = (struct pe_file_hdr *)(img + PE_HDR_OFF);
	pe->magic = PE_NT_MAGIC;
	pe->machine = PE_MACHINE_AMD64;
	pe->sections = 2;
	pe->opt_hdr_size = opt_size;
	pe->flags = PE_F_EXECUTABLE_IMAGE | PE_F_RELOCS_STRIPPED;

	opt = (struct pe_opt64 *)(img + PE_HDR_OFF + sizeof(*pe));
	opt->magic = PE_OPT_MAGIC_PLUS;
	opt->text_size = sizeof(nt_io_code);
	opt->data_size = NTIO_DATA_USED;
	opt->entry_point = NTIO_TEXT_RVA;
	opt->code_base = NTIO_TEXT_RVA;
	opt->image_base = NTIO_IMAGE_BASE;
	opt->section_align = PE_PAGE;
	opt->file_align = PE_PAGE;
	opt->sub_major = 6;
	opt->image_size = NTIO_IMAGE_SIZE;
	opt->header_size = PE_PAGE;
	opt->subsys = PE_SUBSYSTEM_WINDOWS_CUI;
	opt->stack_reserve = 0x100000;
	opt->stack_commit = 0x1000;
	opt->data_dirs = 16;

	sec = (struct pe_section *)(img + PE_HDR_OFF + sizeof(*pe) + opt_size);
	memcpy(sec[0].name, ".text", 5);
	sec[0].virtual_size = sizeof(nt_io_code);
	sec[0].virtual_address = NTIO_TEXT_RVA;
	sec[0].raw_size = PE_PAGE;
	sec[0].raw_ptr = NTIO_TEXT_RVA;
	sec[0].flags = PE_SCN_CNT_CODE | PE_SCN_MEM_EXECUTE | PE_SCN_MEM_READ;

	memcpy(sec[1].name, ".data", 5);
	sec[1].virtual_size = NTIO_DATA_USED;
	sec[1].virtual_address = NTIO_DATA_RVA;
	sec[1].raw_size = PE_PAGE;
	sec[1].raw_ptr = NTIO_DATA_RVA;
	sec[1].flags = PE_SCN_CNT_INITIALIZED_DATA | PE_SCN_MEM_READ |
		       PE_SCN_MEM_WRITE;

	/* .text: the payload. */
	memcpy(img + NTIO_TEXT_RVA, nt_io_code, sizeof(nt_io_code));

	/*
	 * .data: the OBJECT_ATTRIBUTES points at the UNICODE_STRING, which
	 * points at the UTF-16LE path, both by absolute address; the handle
	 * slots, the IO_STATUS_BLOCK, the zero ByteOffset and the read buffer
	 * stay zero.  The write pattern is copied in.  Since file_align is a
	 * page, .data's file offset equals its RVA.
	 */
	data = img + NTIO_DATA_RVA;

	oa.length = sizeof(oa);
	oa.object_name = NTIO_DATA_BASE + D_US;
	oa.attributes = NT_OBJ_CASE_INSENSITIVE;
	memcpy(data + D_OA, &oa, sizeof(oa));

	us.length = wlen_bytes;
	us.maximum_length = wlen_bytes;
	us.buffer = NTIO_DATA_BASE + D_WPATH;
	memcpy(data + D_US, &us, sizeof(us));

	for (i = 0; i + 1 < sizeof(nt_path); i++)
		*(uint16_t *)(data + D_WPATH + i * 2) = (uint8_t)nt_path[i];

	memcpy(data + D_PATTERN, ntio_pattern, NTIO_PATLEN);
}

/*
 * Write @img to a temp file, execve() it in a child, and return the child's
 * exit status - or a negative value for a harness-level failure, so a broken
 * setup is not mistaken for a loader result.  100 means the kernel has no PE
 * loader (execve gave ENOEXEC); 101 means execve failed for some other reason.
 */
static int exec_pe(const uint8_t *img, size_t len)
{
	char path[] = "/tmp/binfmt_pe_nt_io.XXXXXX";
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

/*
 * Register a C: volume over the root filesystem through the personality's
 * debugfs control file - the same way the nt_syscall selftest does.  Returns 0
 * on success, -1 if the interface is absent (kernel without the NT
 * personality), in which case the caller skips.
 */
static int ensure_c_drive(void)
{
	ssize_t n;
	int fd;

	mkdir("/dbg", 0755);
	if (mount("none", "/dbg", "debugfs", 0, NULL) != 0 && errno != EBUSY)
		return -1;

	fd = open("/dbg/ntpers/control", O_WRONLY);
	if (fd < 0)
		return -1;

	/* One command per write(): bind C: to "/", then make it the system volume. */
	if (write(fd, "mount C /", 9) < 0 && errno != EEXIST) {
		close(fd);
		return -1;
	}
	n = write(fd, "system C", 8);
	(void)n;
	close(fd);
	return 0;
}

/* Read LINUX_PATH and confirm it holds exactly the pattern the PE wrote. */
static void verify_from_linux(struct __test_metadata *_metadata)
{
	uint8_t buf[NTIO_PATLEN + 16];
	ssize_t n;
	int fd;

	fd = open(LINUX_PATH, O_RDONLY);
	ASSERT_GE(fd, 0)
		TH_LOG("the NT process did not leave a file at %s", LINUX_PATH);

	n = read(fd, buf, sizeof(buf));
	close(fd);

	ASSERT_EQ(n, NTIO_PATLEN);
	EXPECT_EQ(0, memcmp(buf, ntio_pattern, NTIO_PATLEN));
}

/*
 * A real native PE, loaded by execve() through binfmt_pe, creates a file and
 * round-trips a byte pattern through it using only NT system calls; then this
 * ordinary Linux process reads that same file back and checks the bytes.
 */
TEST(pe_nt_file_roundtrip)
{
	uint8_t img[NTIO_IMAGE_SIZE];
	int rc;

	if (ensure_c_drive() != 0)
		SKIP(return, "NT personality debugfs unavailable; no C: volume");

	unlink(LINUX_PATH);

	build_nt_io_pe(img);
	rc = exec_pe(img, NTIO_IMAGE_SIZE);

	ASSERT_GE(rc, 0);
	if (rc == 100)
		SKIP(return, "kernel built without CONFIG_BINFMT_PE");
	if (rc == 101)
		SKIP(return, "execve failed for a reason other than ENOEXEC");
	if (rc != 42)
		TH_LOG("NT child exited %d (step code; 42 == round-trip ok)", rc);
	ASSERT_EQ(42, rc);

	/* The point of the whole exercise: the Linux side sees the same bytes. */
	verify_from_linux(_metadata);

	unlink(LINUX_PATH);
}

/*
 * A custom main so the same binary can serve as a UML init: as PID 1 it powers
 * the machine off after the harness has run, rather than leaving a dead init.
 * test_harness_run() prints the full TAP result and then exits the process
 * itself, so this shutdown is a best-effort tail; booted as init the machine
 * powers off (or, failing that, panics on the exiting init) only once the
 * result has already been reported.  As an ordinary selftest it is just
 * test_harness_run().
 */
int main(int argc, char **argv)
{
	int ret = test_harness_run(argc, argv);

	if (getpid() == 1) {
		sync();
		reboot(RB_POWER_OFF);
	}
	return ret;
}
