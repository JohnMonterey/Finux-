/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NT dispatch - the tiny architecture shim.
 *
 * The dispatch core (dispatch.c) is otherwise architecture-neutral; only the
 * spelling of "read this register out of struct pt_regs" differs between a
 * native x86-64 pt_regs (named fields) and a UML/x86-64 one (the captured
 * guest register array reached through the PT_REGS_* accessors).  The two
 * layouts are otherwise identical, which is exactly what lets the same
 * nt_do_syscall() drive both the native `syscall` path and the UML guest.
 *
 * The Windows x64 system-service convention this reads:
 *
 *   RAX          the NT service number the ntdll stub put there with
 *                `mov eax, <nr>` (so its high 32 bits are zero)
 *   R10          argument 1  (the stub's `mov r10, rcx`)
 *   RDX,R8,R9    arguments 2, 3, 4
 *   [RSP+0x28+]  arguments 5, 6, ... on the user stack, above the return
 *                address and the four-slot register shadow space
 *   RAX          the NTSTATUS result on return
 *
 * The service number is taken from ORIG_AX (native) / uml_pt_regs.syscall
 * (UML), never from the AX slot: both syscall entry paths have already moved
 * the live RAX out of that slot (native leaves -ENOSYS there, UML uses it for
 * the return value).  The result is written to the AX slot, which the syscall
 * exit path restores into the caller's RAX.
 */
#ifndef _FS_NTPERS_NT_DISPATCH_H
#define _FS_NTPERS_NT_DISPATCH_H

#include <linux/types.h>
#include <asm/ptrace.h>

#ifdef CONFIG_UML

static inline u64 nt_reg_service(struct pt_regs *regs)
{
	return UPT_SYSCALL_NR(&regs->regs);
}

static inline u64 nt_reg_arg1(struct pt_regs *regs) { return PT_REGS_R10(regs); }
static inline u64 nt_reg_arg2(struct pt_regs *regs) { return PT_REGS_DX(regs); }
static inline u64 nt_reg_arg3(struct pt_regs *regs) { return PT_REGS_R8(regs); }
static inline u64 nt_reg_arg4(struct pt_regs *regs) { return PT_REGS_R9(regs); }

static inline unsigned long nt_reg_user_sp(struct pt_regs *regs)
{
	return PT_REGS_SP(regs);
}

static inline void nt_reg_set_result(struct pt_regs *regs, u32 status)
{
	PT_REGS_SET_SYSCALL_RETURN(regs, (unsigned long)status);
}

#else /* native x86-64 */

static inline u64 nt_reg_service(struct pt_regs *regs) { return regs->orig_ax; }
static inline u64 nt_reg_arg1(struct pt_regs *regs) { return regs->r10; }
static inline u64 nt_reg_arg2(struct pt_regs *regs) { return regs->dx; }
static inline u64 nt_reg_arg3(struct pt_regs *regs) { return regs->r8; }
static inline u64 nt_reg_arg4(struct pt_regs *regs) { return regs->r9; }

static inline unsigned long nt_reg_user_sp(struct pt_regs *regs)
{
	return regs->sp;
}

static inline void nt_reg_set_result(struct pt_regs *regs, u32 status)
{
	regs->ax = status;
}

#endif /* CONFIG_UML */

#endif /* _FS_NTPERS_NT_DISPATCH_H */
