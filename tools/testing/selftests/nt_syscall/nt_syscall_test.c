// SPDX-License-Identifier: GPL-2.0
/*
 * End-to-end selftest for real NT system-call dispatch (CONFIG_NT_SYSCALL).
 *
 * This proves the actual `syscall` path, not a mock.  A child process enters
 * NT syscall mode with prctl(PR_SET_NT_SYSCALL_MODE) and then, from a pure
 * assembly thunk that makes no libc or Linux system call, drives the
 * `syscall` instruction exactly the way 64-bit Windows ntdll does - the NT
 * service number in EAX, arguments one through four in R10, RDX, R8, R9, and
 * the remaining arguments on the stack above the return address and the
 * four-slot register shadow space.  It runs a genuine sequence:
 *
 *   NtCreateFile (create)  ->  NtWriteFile  ->  NtClose
 *     ->  NtOpenFile (reopen)  ->  NtQueryInformationFile  ->  NtReadFile
 *     ->  compare the bytes  ->  NtTerminateProcess(status)
 *
 * The child terminates through NtTerminateProcess - itself an NT service, so
 * the process exits without ever making a Linux system call - with status 42
 * when the bytes read back match what was written (and the queried size
 * agrees), or a small step code otherwise.  The parent, an ordinary Linux
 * process, waitpid()s and asserts 42.
 *
 * The OBJECT_ATTRIBUTES / UNICODE_STRING / IO_STATUS_BLOCK and every buffer
 * are built in memory before the child enters NT mode, because after that
 * point the child may touch nothing that would issue a Linux system call.
 *
 * A C: volume must exist for the NT path "C:\..." to resolve; the test
 * registers one over the root filesystem through the personality's debugfs
 * control file before forking the child.  If that debugfs interface is not
 * present the running kernel has no NT personality and the test is skipped.
 *
 * The same binary doubles as a UML init: built static and booted as /init it
 * mounts debugfs, runs the test, prints its TAP result to the console and
 * powers the machine off.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../kselftest_harness.h"

/*
 * prctl() options for the NT personality (see <uapi/linux/prctl.h>).  Defined
 * locally so the test builds against a stock toolchain, the way the binfmt_pe
 * selftest carries its own PE constants.
 */
#define PR_SET_NT_PERSONALITY		82
#define PR_SET_NT_SYSCALL_MODE		86

#define NT_PERSONALITY_ENABLED		(1U << 0)
#define NT_PERSONALITY_CASE_INSENSITIVE	(1U << 1)

/* NT service numbers (Finux assignment; see <uapi/linux/nt_personality.h>). */
#define NT_SYS_NtClose			0
#define NT_SYS_NtCreateFile		1
#define NT_SYS_NtOpenFile		2
#define NT_SYS_NtReadFile		3
#define NT_SYS_NtWriteFile		4
#define NT_SYS_NtQueryInformationFile	5
#define NT_SYS_NtTerminateProcess	6

/* The current-process pseudo-handle GetCurrentProcess() == (HANDLE)-1. */
#define NT_CURRENT_PROCESS		0xffffffffffffffffULL

/* CreateDisposition / access / share / attribute constants (NT numbering). */
#define NT_FILE_OPEN			1
#define NT_FILE_OVERWRITE_IF		5
#define NT_ACCESS_GENERIC_WRITE		0x40000000U
#define NT_ACCESS_GENERIC_READ		0x80000000U
#define NT_SHARE_READ			0x00000001U
#define NT_SHARE_WRITE			0x00000002U
#define NT_FILE_ATTRIBUTE_NORMAL	0x00000080U
#define NT_OBJ_CASE_INSENSITIVE		0x00000040U
#define NT_FILEINFO_STANDARD		5

#define STATUS_SUCCESS			0x00000000U

/* Windows x64 ABI structures, byte-for-byte (see fs/ntpers/syscall.c). */
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

struct nt_io_status_block {
	uint64_t status;	/* union { Status; Pointer } - kept 8 bytes */
	uint64_t information;
};

struct nt_file_standard_information {
	int64_t allocation_size;
	int64_t end_of_file;
	uint32_t number_of_links;
	uint8_t delete_pending;
	uint8_t directory;
	uint16_t pad;
};

/*
 * A gathered NT system call: the service number, the four register arguments,
 * and a pointer to any stack arguments.  Its field offsets are hard-coded in
 * the assembly thunk below, so the layout must not change.
 */
struct nt_call {
	uint64_t nr;		/* 0x00 */
	uint64_t a1;		/* 0x08 - R10 */
	uint64_t a2;		/* 0x10 - RDX */
	uint64_t a3;		/* 0x18 - R8  */
	uint64_t a4;		/* 0x20 - R9  */
	const uint64_t *stk;	/* 0x28 - arguments 5.. */
	uint64_t nstk;		/* 0x30 - count of stack arguments */
};

/*
 * The pure-assembly NT syscall thunk.  A naked function: the argument pointer
 * arrives in RDI (SysV) and everything is done by hand, so nothing the
 * compiler emits can slip a libc or Linux system call in.  It reshapes the
 * stack so that, at the `syscall` instruction, argument five sits at [RSP+0x28]
 * (above an 8-byte return-address slot and the 0x20 Win64 shadow space), loads
 * the register arguments and the service number, executes `syscall`, and
 * returns RAX.  RBX and RBP are preserved; the rest are caller-clobbered
 * exactly as a normal SysV call, and the kernel restores the frame on return.
 */
static __attribute__((naked)) uint64_t nt_syscall_raw(const struct nt_call *c)
{
	__asm__ volatile(
		"pushq %rbx\n\t"
		"pushq %rbp\n\t"
		"movq %rsp, %rbp\n\t"		/* frame anchor to restore RSP */
		"movq 0x30(%rdi), %rax\n\t"	/* nstk */
		"leaq 0x28(,%rax,8), %rax\n\t"	/* bytes = 0x28 + 8*nstk */
		"addq $15, %rax\n\t"
		"andq $-16, %rax\n\t"		/* round up to 16 */
		"subq %rax, %rsp\n\t"		/* reserve the NT argument frame */
		"movq 0x30(%rdi), %rcx\n\t"	/* count */
		"testq %rcx, %rcx\n\t"
		"jz 2f\n\t"
		"movq 0x28(%rdi), %rsi\n\t"	/* stack-argument source */
		"xorq %rbx, %rbx\n"
		"1:\n\t"
		"movq (%rsi,%rbx,8), %rdx\n\t"
		"movq %rdx, 0x28(%rsp,%rbx,8)\n\t"
		"incq %rbx\n\t"
		"cmpq %rcx, %rbx\n\t"
		"jb 1b\n"
		"2:\n\t"
		"movq 0x08(%rdi), %r10\n\t"	/* argument 1 */
		"movq 0x10(%rdi), %rdx\n\t"	/* argument 2 */
		"movq 0x18(%rdi), %r8\n\t"	/* argument 3 */
		"movq 0x20(%rdi), %r9\n\t"	/* argument 4 */
		"movl 0x00(%rdi), %eax\n\t"	/* service number */
		"syscall\n\t"
		"movq %rbp, %rsp\n\t"		/* drop the argument frame */
		"popq %rbp\n\t"
		"popq %rbx\n\t"
		"ret\n\t");
}

/* --- NT service wrappers, each pure (no libc, no Linux syscall) --------- */

static uint32_t nt_create(uint64_t *handle, uint32_t access,
			  struct nt_object_attributes *oa,
			  struct nt_io_status_block *iosb,
			  uint32_t disposition, uint32_t options,
			  uint32_t attributes, uint32_t share)
{
	uint64_t stk[7] = { 0, attributes, share, disposition, options, 0, 0 };
	struct nt_call c = { NT_SYS_NtCreateFile, (uint64_t)(uintptr_t)handle,
			     access, (uint64_t)(uintptr_t)oa,
			     (uint64_t)(uintptr_t)iosb, stk, 7 };

	return (uint32_t)nt_syscall_raw(&c);
}

static uint32_t nt_open(uint64_t *handle, uint32_t access,
			struct nt_object_attributes *oa,
			struct nt_io_status_block *iosb,
			uint32_t share, uint32_t options)
{
	uint64_t stk[2] = { share, options };
	struct nt_call c = { NT_SYS_NtOpenFile, (uint64_t)(uintptr_t)handle,
			     access, (uint64_t)(uintptr_t)oa,
			     (uint64_t)(uintptr_t)iosb, stk, 2 };

	return (uint32_t)nt_syscall_raw(&c);
}

static uint32_t nt_write(uint64_t handle, struct nt_io_status_block *iosb,
			 const void *buf, uint32_t len, int64_t *offset)
{
	uint64_t stk[5] = { (uint64_t)(uintptr_t)iosb, (uint64_t)(uintptr_t)buf,
			    len, (uint64_t)(uintptr_t)offset, 0 };
	struct nt_call c = { NT_SYS_NtWriteFile, handle, 0, 0, 0, stk, 5 };

	return (uint32_t)nt_syscall_raw(&c);
}

static uint32_t nt_read(uint64_t handle, struct nt_io_status_block *iosb,
			void *buf, uint32_t len, int64_t *offset)
{
	uint64_t stk[5] = { (uint64_t)(uintptr_t)iosb, (uint64_t)(uintptr_t)buf,
			    len, (uint64_t)(uintptr_t)offset, 0 };
	struct nt_call c = { NT_SYS_NtReadFile, handle, 0, 0, 0, stk, 5 };

	return (uint32_t)nt_syscall_raw(&c);
}

static uint32_t nt_query(uint64_t handle, struct nt_io_status_block *iosb,
			 void *info, uint32_t len, uint32_t info_class)
{
	uint64_t stk[1] = { info_class };
	struct nt_call c = { NT_SYS_NtQueryInformationFile, handle,
			     (uint64_t)(uintptr_t)iosb, (uint64_t)(uintptr_t)info,
			     len, stk, 1 };

	return (uint32_t)nt_syscall_raw(&c);
}

static uint32_t nt_close(uint64_t handle)
{
	struct nt_call c = { NT_SYS_NtClose, handle, 0, 0, 0, 0, 0 };

	return (uint32_t)nt_syscall_raw(&c);
}

static _Noreturn void nt_terminate(uint32_t status)
{
	struct nt_call c = { NT_SYS_NtTerminateProcess, NT_CURRENT_PROCESS,
			     status, 0, 0, 0, 0 };

	nt_syscall_raw(&c);
	__builtin_unreachable();
}

/* Pure byte compare - no libc call, safe in NT mode. */
static int bytes_equal(const void *a, const void *b, uint32_t n)
{
	const uint8_t *pa = a, *pb = b;
	uint32_t i;

	for (i = 0; i < n; i++)
		if (pa[i] != pb[i])
			return 0;
	return 1;
}

/* The message written and read back through the NT syscall path. */
static const char nt_message[] = "Finux NT syscall path works!";
#define NT_MESSAGE_LEN	(sizeof(nt_message) - 1)

/*
 * The NT-mode child.  Everything it needs is built before it enters NT syscall
 * mode; afterwards it makes only NT system calls and exits through
 * NtTerminateProcess.  Never returns.
 */
static _Noreturn void run_nt_child(void)
{
	static const char path_ascii[] = "C:\\ntsyscall.dat";
	uint16_t wpath[sizeof(path_ascii)];
	struct nt_object_attributes oa = {};
	struct nt_unicode_string us = {};
	struct nt_io_status_block iosb = {};
	struct nt_file_standard_information si = {};
	uint64_t handle = 0, handle2 = 0;
	char rbuf[64] = {};
	int64_t off0 = 0;
	unsigned int i;
	uint32_t st;

	/* Build the UNICODE_STRING path (UTF-16LE) and OBJECT_ATTRIBUTES. */
	for (i = 0; path_ascii[i]; i++)
		wpath[i] = (uint8_t)path_ascii[i];
	us.length = (uint16_t)(i * 2);
	us.maximum_length = us.length;
	us.buffer = (uint64_t)(uintptr_t)wpath;

	oa.length = sizeof(oa);
	oa.object_name = (uint64_t)(uintptr_t)&us;
	oa.attributes = NT_OBJ_CASE_INSENSITIVE;

	/*
	 * Confirm the personality is present while still an ordinary Linux
	 * task - this prctl does not enter NT mode, so its failure (feature
	 * absent) can still be reported by a normal exit the parent maps to a
	 * SKIP.
	 */
	if (prctl(PR_SET_NT_PERSONALITY,
		  NT_PERSONALITY_ENABLED | NT_PERSONALITY_CASE_INSENSITIVE,
		  0, 0, 0) != 0)
		_exit(4);

	/*
	 * Enter NT syscall mode.  This is the last Linux system call the child
	 * makes: from here every `syscall` dispatches through the NT table.
	 */
	prctl(PR_SET_NT_SYSCALL_MODE, 1, 0, 0, 0);

	st = nt_create(&handle, NT_ACCESS_GENERIC_WRITE, &oa, &iosb,
		       NT_FILE_OVERWRITE_IF, 0, NT_FILE_ATTRIBUTE_NORMAL,
		       NT_SHARE_READ | NT_SHARE_WRITE);
	if (st != STATUS_SUCCESS)
		nt_terminate(11);

	st = nt_write(handle, &iosb, nt_message, NT_MESSAGE_LEN, &off0);
	if (st != STATUS_SUCCESS)
		nt_terminate(12);
	if (iosb.information != NT_MESSAGE_LEN)
		nt_terminate(13);

	if (nt_close(handle) != STATUS_SUCCESS)
		nt_terminate(14);

	st = nt_open(&handle2, NT_ACCESS_GENERIC_READ, &oa, &iosb,
		     NT_SHARE_READ | NT_SHARE_WRITE, 0);
	if (st != STATUS_SUCCESS)
		nt_terminate(15);

	st = nt_query(handle2, &iosb, &si, sizeof(si), NT_FILEINFO_STANDARD);
	if (st != STATUS_SUCCESS)
		nt_terminate(16);
	if (si.end_of_file != (int64_t)NT_MESSAGE_LEN)
		nt_terminate(17);

	off0 = 0;
	st = nt_read(handle2, &iosb, rbuf, NT_MESSAGE_LEN, &off0);
	if (st != STATUS_SUCCESS)
		nt_terminate(18);
	if (iosb.information != NT_MESSAGE_LEN)
		nt_terminate(19);
	if (!bytes_equal(rbuf, nt_message, NT_MESSAGE_LEN))
		nt_terminate(20);

	if (nt_close(handle2) != STATUS_SUCCESS)
		nt_terminate(21);

	/* Everything round-tripped through the real `syscall` path. */
	nt_terminate(42);
}

/*
 * Register a C: volume over the root filesystem through the personality's
 * debugfs control file.  Returns 0 on success, -1 if the interface is not
 * present (kernel without the NT personality), in which case the caller skips.
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

TEST(nt_syscall_roundtrip)
{
	int status;
	pid_t pid;

	if (ensure_c_drive() != 0)
		SKIP(return, "NT personality debugfs unavailable; no C: volume");

	pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0)
		run_nt_child();		/* never returns */

	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	ASSERT_TRUE(WIFEXITED(status));
	if (WEXITSTATUS(status) == 4)
		SKIP(return, "kernel built without CONFIG_NT_FS_PERSONALITY");
	if (WEXITSTATUS(status) != 42)
		TH_LOG("NT child exited %d (step code; 42 == round-trip ok)",
		       WEXITSTATUS(status));
	EXPECT_EQ(42, WEXITSTATUS(status));
}

/*
 * A custom main so the same binary is a UML init: as PID 1 it powers the
 * machine off once the harness has printed its TAP result, rather than
 * returning into a "killed init" panic.  As an ordinary selftest it is just
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
