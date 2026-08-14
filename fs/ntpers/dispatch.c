// SPDX-License-Identifier: GPL-2.0
/*
 * NT system-call dispatch: turn a `syscall` from an NT-mode task into a call
 * to the matching Nt* service, marshalling arguments the way 64-bit Windows
 * ntdll drives the instruction.
 *
 * A native PE (or Wine's ntdll) reaches a system service through a stub that
 * is, byte for byte, `mov r10, rcx; mov eax, <nr>; syscall`.  So when the
 * kernel's syscall entry runs for an NT-mode task the register file holds the
 * Windows x64 convention, not the Linux one:
 *
 *   - the NT service number is in RAX (read here via the arch shim, which
 *     takes it from ORIG_AX / the captured guest number, not the AX slot -
 *     the entry path has already overwritten AX);
 *   - arguments one through four are in R10, RDX, R8, R9 (note R10, the
 *     stub's copy of RCX, not RDI/RSI);
 *   - arguments five and up are on the user stack.  At the `syscall`, [RSP]
 *     is the caller's return address and [RSP+8 .. +0x20] is the four-slot
 *     register shadow space Win64 always reserves, so argument five is at
 *     [RSP+0x28], six at [RSP+0x30], and so on;
 *   - the NTSTATUS result goes back in RAX.
 *
 * The dispatcher is architecture-neutral apart from reading and writing those
 * registers, which it does through nt_dispatch.h; the same code drives the
 * native x86-64 entry path and the UML/x86-64 guest, whose register and stack
 * layout is identical.
 *
 * SECURITY CAVEAT.  NT service numbers are a namespace disjoint from Linux
 * syscall numbers, so the architecture entry hooks branch here *before* the
 * Linux syscall entry work runs: NT-mode syscalls are therefore neither
 * filtered by Linux seccomp nor recorded by Linux audit.  A sandbox that
 * relies on a seccomp policy written in terms of Linux syscall numbers does
 * not constrain what an NT-mode task's services do; a seccomp-based NT policy
 * is future work.  See arch/x86/entry/syscall_64.c and
 * arch/um/kernel/skas/syscall.c for the branch points.
 */

#include <linux/kernel.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/nt_personality.h>
#include <linux/nt_syscall.h>

#include "internal.h"
#include "nt_dispatch.h"

/* Arguments one through four arrive in registers; the rest on the stack. */
#define NT_SYSCALL_REG_ARGS	4
/* NtCreateFile is the widest service at eleven arguments. */
#define NT_SYSCALL_ARG_MAX	11
/* Byte offset of the fifth argument above the user RSP (return addr + shadow). */
#define NT_SYSCALL_STACK_ARG0	0x28

/*
 * A service in the table.  @call is a thunk that reads the gathered argument
 * array and invokes the real Nt* wrapper with its true C prototype - one
 * uniform table type, no function-pointer casts and no reliance on unused
 * trailing arguments.  @argc is how many arguments the service takes, which
 * is exactly how many stack slots the dispatcher may safely read.
 */
struct nt_syscall_desc {
	u32	(*call)(const u64 *args);
	u8	argc;
};

static u32 nt_call_close(const u64 *a)
{
	return NtClose(a[0]);
}

static u32 nt_call_create(const u64 *a)
{
	return NtCreateFile((u64 __user *)u64_to_user_ptr(a[0]),
			    (u32)a[1],
			    (struct nt_object_attributes __user *)u64_to_user_ptr(a[2]),
			    (struct nt_io_status_block __user *)u64_to_user_ptr(a[3]),
			    (s64 __user *)u64_to_user_ptr(a[4]),
			    (u32)a[5], (u32)a[6], (u32)a[7], (u32)a[8],
			    (void __user *)u64_to_user_ptr(a[9]),
			    (u32)a[10]);
}

static u32 nt_call_open(const u64 *a)
{
	return NtOpenFile((u64 __user *)u64_to_user_ptr(a[0]),
			  (u32)a[1],
			  (struct nt_object_attributes __user *)u64_to_user_ptr(a[2]),
			  (struct nt_io_status_block __user *)u64_to_user_ptr(a[3]),
			  (u32)a[4], (u32)a[5]);
}

static u32 nt_call_read(const u64 *a)
{
	return NtReadFile(a[0], a[1], a[2], a[3],
			  (struct nt_io_status_block __user *)u64_to_user_ptr(a[4]),
			  (void __user *)u64_to_user_ptr(a[5]),
			  (u32)a[6],
			  (s64 __user *)u64_to_user_ptr(a[7]),
			  (u32 __user *)u64_to_user_ptr(a[8]));
}

static u32 nt_call_write(const u64 *a)
{
	return NtWriteFile(a[0], a[1], a[2], a[3],
			   (struct nt_io_status_block __user *)u64_to_user_ptr(a[4]),
			   (const void __user *)u64_to_user_ptr(a[5]),
			   (u32)a[6],
			   (s64 __user *)u64_to_user_ptr(a[7]),
			   (u32 __user *)u64_to_user_ptr(a[8]));
}

static u32 nt_call_query(const u64 *a)
{
	return NtQueryInformationFile(a[0],
			(struct nt_io_status_block __user *)u64_to_user_ptr(a[1]),
			(void __user *)u64_to_user_ptr(a[2]),
			(u32)a[3], (u32)a[4]);
}

static u32 nt_call_terminate(const u64 *a)
{
	return NtTerminateProcess(a[0], (u32)a[1]);
}

/*
 * The NT service table, indexed by the numbers in <uapi/linux/nt_personality.h>.
 * A hole (a number with no service) reads back as a NULL @call and is answered
 * with STATUS_NOT_IMPLEMENTED, the same as an out-of-range number.
 */
static const struct nt_syscall_desc nt_syscall_table[NT_SYS_MAX] = {
	[NT_SYS_NtClose]		= { nt_call_close,	1 },
	[NT_SYS_NtCreateFile]		= { nt_call_create,	11 },
	[NT_SYS_NtOpenFile]		= { nt_call_open,	6 },
	[NT_SYS_NtReadFile]		= { nt_call_read,	9 },
	[NT_SYS_NtWriteFile]		= { nt_call_write,	9 },
	[NT_SYS_NtQueryInformationFile]	= { nt_call_query,	5 },
	[NT_SYS_NtTerminateProcess]	= { nt_call_terminate,	2 },
};

/**
 * nt_do_syscall - dispatch one NT system service from a `syscall` frame
 * @regs: the register state saved on syscall entry
 *
 * Reads the NT service number and arguments out of @regs following the
 * Windows x64 convention, invokes the service, and writes its NTSTATUS back
 * into @regs (whence the syscall exit path restores it into the caller's RAX).
 *
 * An unknown service number is answered with STATUS_NOT_IMPLEMENTED; a stack
 * argument that faults on read is answered with STATUS_ACCESS_VIOLATION.  The
 * dispatcher reads exactly as many stack slots as the selected service takes,
 * so a service with no stack arguments never touches the user stack.
 */
void nt_do_syscall(struct pt_regs *regs)
{
	u64 args[NT_SYSCALL_ARG_MAX] = {};
	const struct nt_syscall_desc *desc;
	unsigned long user_sp;
	unsigned int i;
	u64 number;

	number = nt_reg_service(regs);
	if (number >= NT_SYS_MAX || !nt_syscall_table[number].call) {
		nt_reg_set_result(regs, STATUS_NOT_IMPLEMENTED);
		return;
	}
	desc = &nt_syscall_table[number];

	args[0] = nt_reg_arg1(regs);
	args[1] = nt_reg_arg2(regs);
	args[2] = nt_reg_arg3(regs);
	args[3] = nt_reg_arg4(regs);

	user_sp = nt_reg_user_sp(regs);
	for (i = NT_SYSCALL_REG_ARGS; i < desc->argc; i++) {
		u64 __user *slot = (u64 __user *)(user_sp + NT_SYSCALL_STACK_ARG0 +
						  8 * (i - NT_SYSCALL_REG_ARGS));

		if (get_user(args[i], slot)) {
			nt_reg_set_result(regs, STATUS_ACCESS_VIOLATION);
			return;
		}
	}

	nt_reg_set_result(regs, desc->call(args));
}
