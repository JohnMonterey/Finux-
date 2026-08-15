// SPDX-License-Identifier: GPL-2.0
/*
 * End-to-end selftest: a dynamically-linked native PE, whose .text contains no
 * `syscall` of its own but imports its NT services from ntdll.dll, is loaded by
 * execve() through binfmt_pe, has its imports bound against a real ntdll, runs,
 * and does genuine file I/O that the ordinary Linux side then verifies.
 *
 * This is the payoff of the in-kernel dynamic linker.  Unlike its sibling
 * binfmt_pe_nt_io_test - which drives the NT service convention by hand with raw
 * `syscall` instructions - the PE here reaches the kernel the way every real
 * Windows binary does: it declares, in its import directory, that it imports
 * NtCreateFile/NtWriteFile/NtClose/NtOpenFile/NtReadFile/NtTerminateProcess from
 * ntdll.dll, and its code calls each one only through the import address table
 * with an ordinary Win64 `call qword ptr [rip+disp32]`.  There is not one
 * `syscall` byte in its .text.
 *
 * The kernel makes it run: binfmt_pe maps the image, turns it into an NT process
 * (fs/binfmt_pe.c, pe_setup_nt_process()), then binds its imports
 * (pe_bind_imports()) - it loads C:\Windows\System32\ntdll.dll (a real DLL PE
 * with a real export directory of syscall stubs), resolves each imported name
 * against ntdll's exports, and patches the IAT slot with the stub's address.  So
 * a `call [IAT]` in the image lands in an ntdll stub (`mov r10,rcx; mov eax,N;
 * syscall; ret`) whose `syscall` reaches fs/ntpers/dispatch.c.
 *
 *   NtCreateFile("C:\impdemo.dat", GENERIC_WRITE, share=0, FILE_OVERWRITE_IF)
 *     -> NtWriteFile(<known pattern>) -> NtClose
 *     -> NtOpenFile(same path, GENERIC_READ) -> NtReadFile
 *     -> compare byte-for-byte -> NtTerminateProcess(NT_CURRENT_PROCESS, 42)
 *
 * The child exits 42 on a full round trip, or a distinct small status naming the
 * step that failed (11..18).  The harness - an ordinary Linux process, never in
 * NT mode - places ntdll.dll under C:\Windows\System32, registers a C: volume
 * over the root filesystem through the personality's debugfs control file,
 * waitpid()s and asserts 42, then opens "C:\impdemo.dat" (== "/impdemo.dat" on
 * the root filesystem) from the Linux side and checks the bytes.  That the two
 * sides agree is the whole point: a Windows binary that imports its NT services
 * from a real ntdll created a real file through the kernel's NT layer.
 *
 * A C: volume must exist for "C:\..." to resolve and ntdll.dll must be present
 * for binding to succeed; the harness sets both up and skips if the debugfs
 * control interface is absent (a kernel without the NT personality).  If the
 * kernel also lacks the PE loader the execve() fails with ENOEXEC and the test
 * is skipped.
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

/* A real minimal ntdll.dll to bind against (the Phase-B asset). */
#include "ntdll_builder.h"

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
 * NT ABI constants (see <uapi/linux/nt_personality.h>).  Defined locally so the
 * test builds against a stock toolchain, the way the sibling selftests do.
 */
#define NT_OBJ_CASE_INSENSITIVE		0x00000040U

/*
 * PE geometry.  Four page-aligned pieces - a header page, a .text page (the
 * payload), a .data page (the NT structs and buffers) and a .idata page (the
 * import directory) - and RELOCS_STRIPPED, so the loader maps it at its
 * preferred ImageBase and never relocates it.  That lets the payload reference
 * its own .data by absolute addresses (ImageBase + RVA) baked into the code, and
 * lets the IAT slot addresses be known at build time so `call [rip+disp32]` can
 * target them.
 */
#define PE_PAGE			0x1000UL
#define PE_HDR_OFF		0x80		/* where the PE header sits */
#define IMP_IMAGE_BASE		0x140000000UL	/* default 64-bit EXE base */
#define IMP_TEXT_RVA		0x1000
#define IMP_DATA_RVA		0x2000
#define IMP_IDATA_RVA		0x3000
#define IMP_IMAGE_SIZE		(4 * PE_PAGE)	/* header + text + data + idata */
#define IMP_DATA_BASE		(IMP_IMAGE_BASE + IMP_DATA_RVA)

/*
 * .data field offsets, relative to the start of the .data section.  The payload
 * has the absolute address (IMP_DATA_BASE + offset) of each of these baked in,
 * so these must match payload.S byte for byte (see the disassembly below).
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
#define IMP_DATA_USED		0xc8	/* end of the fields above */

/*
 * .idata layout, relative to the start of the image (RVAs).  The import address
 * table sits at IMP_IAT_RVA, so IAT slot i is at ImageBase + IMP_IAT_RVA + i*8;
 * the payload's `call [rip+disp32]` disps are computed from exactly those
 * absolute addresses (see payload.S and the disassembly).  The import function
 * order below is the IAT slot order the payload calls through.
 */
#define IMP_DESC_RVA		0x3000	/* IMAGE_IMPORT_DESCRIPTOR[2] */
#define IMP_ILT_RVA		0x3040	/* OriginalFirstThunk: u64[7] */
#define IMP_IAT_RVA		0x3080	/* FirstThunk (the IAT): u64[7] */
#define IMP_HN_RVA		0x30c0	/* IMAGE_IMPORT_BY_NAME entries */
#define IMP_DLLNAME_RVA		0x3200	/* "ntdll.dll" */
#define IMP_IDATA_USED		0x300	/* end of the fields above */

/* The imported symbols, in IAT slot order (slot i at IMP_IAT_RVA + i*8). */
static const char *const imp_functions[] = {
	"NtCreateFile",		/* IAT[0] -> 0x140003080 */
	"NtWriteFile",		/* IAT[1] -> 0x140003088 */
	"NtClose",		/* IAT[2] -> 0x140003090 */
	"NtOpenFile",		/* IAT[3] -> 0x140003098 */
	"NtReadFile",		/* IAT[4] -> 0x1400030a0 */
	"NtTerminateProcess",	/* IAT[5] -> 0x1400030a8 */
};

#define IMP_NR_FUNCS \
	((unsigned int)(sizeof(imp_functions) / sizeof(imp_functions[0])))

/* The NT path the PE creates, and its Linux-visible equivalent. */
static const char nt_path[] = "C:\\impdemo.dat";
#define LINUX_PATH		"/impdemo.dat"

/*
 * The known byte pattern the PE writes and the harness verifies.  Exactly
 * IMP_PATLEN bytes; the payload's Length arguments carry IMP_PATLEN as an
 * immediate, so its size is fixed at assembly time and must not change.
 */
#define IMP_PATLEN		32
static const uint8_t imp_pattern[IMP_PATLEN] = "Finux PE via imported ntdll! 012";

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

/* IMAGE_IMPORT_DESCRIPTOR (see <linux/pe.h> struct pe_import_descriptor). */
struct __attribute__((packed)) pe_import_desc {
	uint32_t lookup_table;		/* OriginalFirstThunk */
	uint32_t timestamp;
	uint32_t forwarder_chain;
	uint32_t name;			/* RVA of the imported DLL's name */
	uint32_t address_table;		/* FirstThunk (the IAT) */
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
_Static_assert(sizeof(struct pe_import_desc) == 20, "IMAGE_IMPORT_DESCRIPTOR is 20 bytes");

/*
 * The entry payload, running as an NT process from instruction one and calling
 * every NT service ONLY through the IAT - there is no `syscall` in these bytes.
 * Assembled with the host assembler from payload.S and linked at the image's
 * load address so the rip-relative call displacements resolve to the IAT slots;
 * the .text bytes are pasted here and the full disassembly kept below.
 *
 * Each call follows the ordinary Win64 convention that an ntdll stub expects:
 * arguments one to four in rcx, rdx, r8, r9; arguments five and up in the stack
 * argument area starting at [rsp+0x20], above the 0x20 four-slot shadow space
 * the caller reserves before every call.  The `call qword ptr [rip+disp32]`
 * reads the IAT slot (patched by the loader with the ntdll stub address) and
 * transfers control there; the stub does `mov r10,rcx; mov eax,N; syscall; ret`,
 * so at its `syscall` the register/stack layout is exactly what
 * fs/ntpers/dispatch.c decodes (arg five at user_sp+0x28, i.e. [rsp+0x20] as the
 * caller wrote it, once the call has pushed the return address).  The payload
 * subtracts 0x100 from rsp once at entry so the shadow space and up to seven
 * stack arguments (NtCreateFile is the widest) are fresh scratch below the
 * initial stack top.  Every .data reference is an absolute IMP_DATA_BASE +
 * offset immediate, valid because RELOCS_STRIPPED pins the image at
 * IMP_IMAGE_BASE == 0x140000000, so .data sits at 0x140002000 and the IAT at
 * 0x140003080.
 *
 *   _start:
 *     sub    rsp,0x100
 *   ; 1. NtCreateFile(&h1, GENERIC_WRITE, &oa, &iosb, NULL,
 *   ;                 NORMAL, share=0, OVERWRITE_IF, 0, NULL, 0)
 *     mov    QWORD PTR [rsp+0x20],0x0     ; AllocationSize = NULL
 *     mov    QWORD PTR [rsp+0x28],0x80    ; FileAttributes = NORMAL
 *     mov    QWORD PTR [rsp+0x30],0x0     ; ShareAccess = 0 (exclusive)
 *     mov    QWORD PTR [rsp+0x38],0x5     ; CreateDisposition = OVERWRITE_IF
 *     mov    QWORD PTR [rsp+0x40],0x0     ; CreateOptions = 0
 *     mov    QWORD PTR [rsp+0x48],0x0     ; EaBuffer = NULL
 *     mov    QWORD PTR [rsp+0x50],0x0     ; EaLength = 0
 *     movabs rcx,0x140002000              ; &handle1
 *     mov    edx,0x40000000               ; GENERIC_WRITE
 *     movabs r8,0x140002020               ; &oa
 *     movabs r9,0x140002010               ; &iosb
 *     call   QWORD PTR [rip+...]          ; -> IAT[0] NtCreateFile
 *     test   eax,eax ; jne fail_create
 *   ; 2. NtWriteFile(h1, 0,0,0, &iosb, PATTERN, 32, &offset0, NULL)
 *     movabs rax,0x140002010 ; mov [rsp+0x20],rax   ; IoStatusBlock
 *     movabs rax,0x140002088 ; mov [rsp+0x28],rax   ; Buffer = pattern
 *     mov    QWORD PTR [rsp+0x30],0x20             ; Length = 32
 *     movabs rax,0x140002080 ; mov [rsp+0x38],rax   ; ByteOffset -> 0
 *     mov    QWORD PTR [rsp+0x40],0x0              ; Key = NULL
 *     movabs rcx,0x140002000 ; mov rcx,[rcx]        ; handle value
 *     xor    edx,edx ; xor r8d,r8d ; xor r9d,r9d
 *     call   QWORD PTR [rip+...]          ; -> IAT[1] NtWriteFile
 *     test   eax,eax ; jne fail_write
 *     movabs rcx,0x140002010 ; mov rax,[rcx+0x8]    ; iosb.Information
 *     cmp    rax,0x20 ; jne fail_wshort             ; short write?
 *   ; 3. NtClose(h1)
 *     movabs rcx,0x140002000 ; mov rcx,[rcx]
 *     call   QWORD PTR [rip+...]          ; -> IAT[2] NtClose
 *     test   eax,eax ; jne fail_close
 *   ; 4. NtOpenFile(&h2, GENERIC_READ, &oa, &iosb, SHARE_RW, 0)
 *     mov    QWORD PTR [rsp+0x20],0x3              ; ShareAccess = READ|WRITE
 *     mov    QWORD PTR [rsp+0x28],0x0              ; OpenOptions = 0
 *     movabs rcx,0x140002008                        ; &handle2
 *     mov    edx,0x80000000                         ; GENERIC_READ
 *     movabs r8,0x140002020 ; movabs r9,0x140002010
 *     call   QWORD PTR [rip+...]          ; -> IAT[3] NtOpenFile
 *     test   eax,eax ; jne fail_open
 *   ; 5. NtReadFile(h2, 0,0,0, &iosb, RDBUF, 32, &offset0, NULL)
 *     movabs rax,0x140002010 ; mov [rsp+0x20],rax   ; IoStatusBlock
 *     movabs rax,0x1400020a8 ; mov [rsp+0x28],rax   ; Buffer = rdbuf
 *     mov    QWORD PTR [rsp+0x30],0x20             ; Length = 32
 *     movabs rax,0x140002080 ; mov [rsp+0x38],rax   ; ByteOffset -> 0
 *     mov    QWORD PTR [rsp+0x40],0x0              ; Key = NULL
 *     movabs rcx,0x140002008 ; mov rcx,[rcx]        ; handle value
 *     xor    edx,edx ; xor r8d,r8d ; xor r9d,r9d
 *     call   QWORD PTR [rip+...]          ; -> IAT[4] NtReadFile
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
 *     mov    rcx,-1                                 ; NT_CURRENT_PROCESS
 *     call   QWORD PTR [rip+...]          ; -> IAT[5] NtTerminateProcess(,edx)
 *     ud2
 */
static const uint8_t imp_code[] = {
	0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x20,
	0x00, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x28, 0x80, 0x00, 0x00,
	0x00, 0x48, 0xc7, 0x44, 0x24, 0x30, 0x00, 0x00, 0x00, 0x00, 0x48, 0xc7,
	0x44, 0x24, 0x38, 0x05, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x40,
	0x00, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x48, 0x00, 0x00, 0x00,
	0x00, 0x48, 0xc7, 0x44, 0x24, 0x50, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb9,
	0x00, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0xba, 0x00, 0x00, 0x00,
	0x40, 0x49, 0xb8, 0x20, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x49,
	0xb9, 0x10, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0xff, 0x15, 0x11,
	0x20, 0x00, 0x00, 0x85, 0xc0, 0x0f, 0x85, 0x7a, 0x01, 0x00, 0x00, 0x48,
	0xb8, 0x10, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44,
	0x24, 0x20, 0x48, 0xb8, 0x88, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
	0x48, 0x89, 0x44, 0x24, 0x28, 0x48, 0xc7, 0x44, 0x24, 0x30, 0x20, 0x00,
	0x00, 0x00, 0x48, 0xb8, 0x80, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
	0x48, 0x89, 0x44, 0x24, 0x38, 0x48, 0xc7, 0x44, 0x24, 0x40, 0x00, 0x00,
	0x00, 0x00, 0x48, 0xb9, 0x00, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
	0x48, 0x8b, 0x09, 0x31, 0xd2, 0x45, 0x31, 0xc0, 0x45, 0x31, 0xc9, 0xff,
	0x15, 0xb7, 0x1f, 0x00, 0x00, 0x85, 0xc0, 0x0f, 0x85, 0x1f, 0x01, 0x00,
	0x00, 0x48, 0xb9, 0x10, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x48,
	0x8b, 0x41, 0x08, 0x48, 0x83, 0xf8, 0x20, 0x0f, 0x85, 0x0e, 0x01, 0x00,
	0x00, 0x48, 0xb9, 0x00, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x48,
	0x8b, 0x09, 0xff, 0x15, 0x8c, 0x1f, 0x00, 0x00, 0x85, 0xc0, 0x0f, 0x85,
	0xfa, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x20, 0x03, 0x00, 0x00,
	0x00, 0x48, 0xc7, 0x44, 0x24, 0x28, 0x00, 0x00, 0x00, 0x00, 0x48, 0xb9,
	0x08, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0xba, 0x00, 0x00, 0x00,
	0x80, 0x49, 0xb8, 0x20, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x49,
	0xb9, 0x10, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0xff, 0x15, 0x51,
	0x1f, 0x00, 0x00, 0x85, 0xc0, 0x0f, 0x85, 0xbe, 0x00, 0x00, 0x00, 0x48,
	0xb8, 0x10, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x48, 0x89, 0x44,
	0x24, 0x20, 0x48, 0xb8, 0xa8, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
	0x48, 0x89, 0x44, 0x24, 0x28, 0x48, 0xc7, 0x44, 0x24, 0x30, 0x20, 0x00,
	0x00, 0x00, 0x48, 0xb8, 0x80, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
	0x48, 0x89, 0x44, 0x24, 0x38, 0x48, 0xc7, 0x44, 0x24, 0x40, 0x00, 0x00,
	0x00, 0x00, 0x48, 0xb9, 0x08, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
	0x48, 0x8b, 0x09, 0x31, 0xd2, 0x45, 0x31, 0xc0, 0x45, 0x31, 0xc9, 0xff,
	0x15, 0xf7, 0x1e, 0x00, 0x00, 0x85, 0xc0, 0x75, 0x67, 0x48, 0xb9, 0x10,
	0x20, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x41, 0x08, 0x48,
	0x83, 0xf8, 0x20, 0x75, 0x5a, 0x48, 0xbe, 0x88, 0x20, 0x00, 0x40, 0x01,
	0x00, 0x00, 0x00, 0x48, 0xbf, 0xa8, 0x20, 0x00, 0x40, 0x01, 0x00, 0x00,
	0x00, 0xb9, 0x20, 0x00, 0x00, 0x00, 0x8a, 0x06, 0x3a, 0x07, 0x75, 0x42,
	0x48, 0xff, 0xc6, 0x48, 0xff, 0xc7, 0xff, 0xc9, 0x75, 0xf0, 0xba, 0x2a,
	0x00, 0x00, 0x00, 0xeb, 0x36, 0xba, 0x0b, 0x00, 0x00, 0x00, 0xeb, 0x2f,
	0xba, 0x0c, 0x00, 0x00, 0x00, 0xeb, 0x28, 0xba, 0x0d, 0x00, 0x00, 0x00,
	0xeb, 0x21, 0xba, 0x0e, 0x00, 0x00, 0x00, 0xeb, 0x1a, 0xba, 0x0f, 0x00,
	0x00, 0x00, 0xeb, 0x13, 0xba, 0x10, 0x00, 0x00, 0x00, 0xeb, 0x0c, 0xba,
	0x11, 0x00, 0x00, 0x00, 0xeb, 0x05, 0xba, 0x12, 0x00, 0x00, 0x00, 0x48,
	0xc7, 0xc1, 0xff, 0xff, 0xff, 0xff, 0xff, 0x15, 0x74, 0x1e, 0x00, 0x00,
	0x0f, 0x0b,
};

/*
 * Lay out the import directory into @img at IMP_IDATA_RVA: one
 * IMAGE_IMPORT_DESCRIPTOR for "ntdll.dll" (plus the all-zero terminator), its
 * import lookup table (OriginalFirstThunk) and import address table (FirstThunk)
 * of by-name thunks, the IMAGE_IMPORT_BY_NAME { hint, name } entries, and the
 * "ntdll.dll" string.  The IAT is pre-filled with the same by-name thunk RVAs as
 * the lookup table; the loader overwrites each slot with the resolved stub
 * address.  Since file_align is a page, every RVA equals its file offset.
 */
static void build_import_dir(uint8_t *img)
{
	struct pe_import_desc *desc;
	uint32_t hn = IMP_HN_RVA;
	unsigned int i;

	for (i = 0; i < IMP_NR_FUNCS; i++) {
		size_t namelen = strlen(imp_functions[i]);

		/* IMAGE_IMPORT_BY_NAME { hint = 0, name, '\0' }. */
		*(uint16_t *)(img + hn) = 0;
		memcpy(img + hn + 2, imp_functions[i], namelen + 1);

		/* By-name thunk (bit 63 clear) in both the lookup table and IAT. */
		*(uint64_t *)(img + IMP_ILT_RVA + i * 8) = hn;
		*(uint64_t *)(img + IMP_IAT_RVA + i * 8) = hn;

		hn += 2 + (uint32_t)namelen + 1;
		hn = (hn + 1) & ~1U;		/* keep the u16 hint aligned */
	}
	/* Zero terminator thunk ends each parallel table. */
	*(uint64_t *)(img + IMP_ILT_RVA + IMP_NR_FUNCS * 8) = 0;
	*(uint64_t *)(img + IMP_IAT_RVA + IMP_NR_FUNCS * 8) = 0;

	strcpy((char *)(img + IMP_DLLNAME_RVA), "ntdll.dll");

	/* descriptor[0] names ntdll.dll and points at the two thunk tables. */
	desc = (struct pe_import_desc *)(img + IMP_DESC_RVA);
	desc[0].lookup_table = IMP_ILT_RVA;
	desc[0].name = IMP_DLLNAME_RVA;
	desc[0].address_table = IMP_IAT_RVA;
	/* desc[1] stays all-zero: the terminator. */
}

/*
 * Lay out the complete main PE image into @img (IMP_IMAGE_SIZE bytes): the MZ/PE
 * headers with export dir 0 empty and import dir 1 pointing at .idata, a .text
 * section holding the payload, a .data section holding the NT structs the payload
 * reaches by absolute address, and a .idata section holding the import
 * directory.
 */
static void build_import_pe(uint8_t *img)
{
	struct nt_object_attributes oa = {};
	struct nt_unicode_string us = {};
	struct pe_file_hdr *pe;
	struct pe_opt64 *opt;
	struct pe_section *sec;
	uint8_t *dirs;
	uint16_t opt_size = sizeof(*opt) + 16 * 8;	/* + 16 data directories */
	uint16_t wlen_bytes = (uint16_t)((sizeof(nt_path) - 1) * 2);
	uint8_t *data;
	unsigned int i;

	memset(img, 0, IMP_IMAGE_SIZE);

	/* MZ header: signature and the offset of the PE header. */
	img[0] = 'M';
	img[1] = 'Z';
	*(uint32_t *)(img + 0x3c) = PE_HDR_OFF;

	pe = (struct pe_file_hdr *)(img + PE_HDR_OFF);
	pe->magic = PE_NT_MAGIC;
	pe->machine = PE_MACHINE_AMD64;
	pe->sections = 3;
	pe->opt_hdr_size = opt_size;
	pe->flags = PE_F_EXECUTABLE_IMAGE | PE_F_RELOCS_STRIPPED;

	opt = (struct pe_opt64 *)(img + PE_HDR_OFF + sizeof(*pe));
	opt->magic = PE_OPT_MAGIC_PLUS;
	opt->text_size = sizeof(imp_code);
	opt->data_size = IMP_DATA_USED;
	opt->entry_point = IMP_TEXT_RVA;
	opt->code_base = IMP_TEXT_RVA;
	opt->image_base = IMP_IMAGE_BASE;
	opt->section_align = PE_PAGE;
	opt->file_align = PE_PAGE;
	opt->sub_major = 6;
	opt->image_size = IMP_IMAGE_SIZE;
	opt->header_size = PE_PAGE;
	opt->subsys = PE_SUBSYSTEM_WINDOWS_CUI;
	opt->stack_reserve = 0x100000;
	opt->stack_commit = 0x1000;
	opt->data_dirs = 16;

	/* Data directory [1] is the import table (dir [0], exports, stays 0). */
	dirs = (uint8_t *)opt + sizeof(*opt);
	*(uint32_t *)(dirs + 1 * 8 + 0) = IMP_DESC_RVA;
	*(uint32_t *)(dirs + 1 * 8 + 4) = 2 * sizeof(struct pe_import_desc);

	sec = (struct pe_section *)(img + PE_HDR_OFF + sizeof(*pe) + opt_size);
	memcpy(sec[0].name, ".text", 5);
	sec[0].virtual_size = sizeof(imp_code);
	sec[0].virtual_address = IMP_TEXT_RVA;
	sec[0].raw_size = PE_PAGE;
	sec[0].raw_ptr = IMP_TEXT_RVA;
	sec[0].flags = PE_SCN_CNT_CODE | PE_SCN_MEM_EXECUTE | PE_SCN_MEM_READ;

	memcpy(sec[1].name, ".data", 5);
	sec[1].virtual_size = IMP_DATA_USED;
	sec[1].virtual_address = IMP_DATA_RVA;
	sec[1].raw_size = PE_PAGE;
	sec[1].raw_ptr = IMP_DATA_RVA;
	sec[1].flags = PE_SCN_CNT_INITIALIZED_DATA | PE_SCN_MEM_READ |
		       PE_SCN_MEM_WRITE;

	/*
	 * .idata is read-only: the loader patches the IAT through its FOLL_FORCE
	 * writer (the same path base relocations use), so the IAT need not sit in
	 * a writable section, and the payload only ever reads it.
	 */
	memcpy(sec[2].name, ".idata", 6);
	sec[2].virtual_size = IMP_IDATA_USED;
	sec[2].virtual_address = IMP_IDATA_RVA;
	sec[2].raw_size = PE_PAGE;
	sec[2].raw_ptr = IMP_IDATA_RVA;
	sec[2].flags = PE_SCN_CNT_INITIALIZED_DATA | PE_SCN_MEM_READ;

	/* .text: the payload. */
	memcpy(img + IMP_TEXT_RVA, imp_code, sizeof(imp_code));

	/*
	 * .data: OBJECT_ATTRIBUTES -> UNICODE_STRING -> UTF-16LE path, all by
	 * absolute address; the handles, IO_STATUS_BLOCK, zero ByteOffset and
	 * read buffer stay zero.  The write pattern is copied in.
	 */
	data = img + IMP_DATA_RVA;

	oa.length = sizeof(oa);
	oa.object_name = IMP_DATA_BASE + D_US;
	oa.attributes = NT_OBJ_CASE_INSENSITIVE;
	memcpy(data + D_OA, &oa, sizeof(oa));

	us.length = wlen_bytes;
	us.maximum_length = wlen_bytes;
	us.buffer = IMP_DATA_BASE + D_WPATH;
	memcpy(data + D_US, &us, sizeof(us));

	for (i = 0; i + 1 < sizeof(nt_path); i++)
		*(uint16_t *)(data + D_WPATH + i * 2) = (uint8_t)nt_path[i];

	memcpy(data + D_PATTERN, imp_pattern, IMP_PATLEN);

	/* .idata: the import directory the loader binds against ntdll. */
	build_import_dir(img);
}

/*
 * Write @img to a temp file, execve() it in a child, and return the child's exit
 * status - or a negative value for a harness-level failure.  100 means the
 * kernel has no PE loader (execve gave ENOEXEC); 101 means execve failed for
 * some other reason.
 */
static int exec_pe(const uint8_t *img, size_t len)
{
	char path[] = "/tmp/binfmt_pe_import.XXXXXX";
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
 * debugfs control file, the same way the sibling selftests do.  Returns 0 on
 * success, -1 if the interface is absent (kernel without the NT personality), in
 * which case the caller skips.
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

/*
 * Place a real ntdll.dll at C:\Windows\System32\ntdll.dll (== the Linux path
 * /Windows/System32/ntdll.dll under the C: volume), which is where the loader's
 * DLL search resolves an import of "ntdll.dll".  Returns 0 on success, -1 on
 * any filesystem error.
 */
static int install_ntdll(void)
{
	mkdir("/Windows", 0755);
	mkdir("/Windows/System32", 0755);
	return write_ntdll_dll("/Windows/System32/ntdll.dll");
}

/* Read LINUX_PATH and confirm it holds exactly the pattern the PE wrote. */
static void verify_from_linux(struct __test_metadata *_metadata)
{
	uint8_t buf[IMP_PATLEN + 16];
	ssize_t n;
	int fd;

	fd = open(LINUX_PATH, O_RDONLY);
	ASSERT_GE(fd, 0)
		TH_LOG("the NT process did not leave a file at %s", LINUX_PATH);

	n = read(fd, buf, sizeof(buf));
	close(fd);

	ASSERT_EQ(n, IMP_PATLEN);
	EXPECT_EQ(0, memcmp(buf, imp_pattern, IMP_PATLEN));
}

/*
 * A dynamically-linked native PE, whose code calls NT services only through its
 * import address table, is loaded by execve() through binfmt_pe, has its imports
 * bound against a real ntdll.dll, runs a file round trip, and then this ordinary
 * Linux process reads the file back and checks the bytes.
 */
TEST(pe_import_roundtrip)
{
	uint8_t img[IMP_IMAGE_SIZE];
	int rc;

	if (ensure_c_drive() != 0)
		SKIP(return, "NT personality debugfs unavailable; no C: volume");

	if (install_ntdll() != 0)
		SKIP(return, "could not install C:\\Windows\\System32\\ntdll.dll");

	unlink(LINUX_PATH);

	build_import_pe(img);
	rc = exec_pe(img, IMP_IMAGE_SIZE);

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
 * the machine off after the harness has run.  test_harness_run() prints the full
 * TAP result and exits the process itself, so this shutdown is a best-effort
 * tail.  As an ordinary selftest it is just test_harness_run().
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
