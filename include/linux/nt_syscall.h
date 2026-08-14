/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NT system-call dispatch - the per-task "NT syscall mode" flag and the
 * hook the architecture syscall-entry code branches to.
 *
 * A task in NT syscall mode drives the `syscall` instruction with the
 * Windows x64 system-service convention rather than the Linux one: RAX
 * holds an NT service number (a namespace disjoint from Linux syscall
 * numbers), arguments one through four are in R10, RDX, R8 and R9, and any
 * further arguments are on the user stack.  The architecture entry path
 * tests the flag and, when set, calls nt_do_syscall() instead of the Linux
 * dispatcher; see arch/x86/entry/syscall_64.c and arch/um/kernel/skas/
 * syscall.c.  The dispatcher itself lives in fs/ntpers/dispatch.c.
 *
 * The flag is sticky (set once, stays set) and cheap to test: it is a plain
 * word in task_struct, so the hot path for an ordinary Linux task is a
 * single predicted-not-taken branch, and nothing at all when
 * CONFIG_NT_FS_PERSONALITY is off.
 */
#ifndef _LINUX_NT_SYSCALL_H
#define _LINUX_NT_SYSCALL_H

#include <linux/sched.h>

struct pt_regs;

#ifdef CONFIG_NT_FS_PERSONALITY

/**
 * nt_syscall_mode - is @tsk dispatching syscalls through the NT table?
 * @tsk: the task to test (normally current)
 *
 * The single test the syscall hot path makes.  Inlined to a word load and a
 * branch so an ordinary Linux task pays only a predicted-not-taken branch.
 */
static inline bool nt_syscall_mode(struct task_struct *tsk)
{
	return tsk->nt_syscall_mode;
}

/**
 * nt_syscall_mode_set - put @tsk into NT syscall mode
 * @tsk: the task, which must be current or not yet running
 *
 * Sticky: once set the task's `syscall` instructions dispatch through the
 * NT service table for the rest of its life (or until nt_syscall_mode_clear()
 * on a fresh exec).  The PE loader calls this so a Windows image runs in NT
 * mode from its first instruction; prctl(PR_SET_NT_SYSCALL_MODE) is the
 * test entry point.
 */
static inline void nt_syscall_mode_set(struct task_struct *tsk)
{
	tsk->nt_syscall_mode = 1;
}

/**
 * nt_syscall_mode_clear - take @tsk out of NT syscall mode
 * @tsk: the task, which must be current or not yet running
 *
 * Provided for exec of a non-PE image, which must return the task to plain
 * Linux syscall dispatch.
 */
static inline void nt_syscall_mode_clear(struct task_struct *tsk)
{
	tsk->nt_syscall_mode = 0;
}

#ifdef CONFIG_NT_SYSCALL
/*
 * The dispatch core, defined by fs/ntpers/dispatch.c on the architectures
 * whose syscall entry is wired up (x86-64, native and UML).  Reads the NT
 * service number and arguments out of @regs, invokes the matching Nt*
 * service and writes the NTSTATUS result back into @regs.
 */
void nt_do_syscall(struct pt_regs *regs);
#endif

#else /* !CONFIG_NT_FS_PERSONALITY */

static inline bool nt_syscall_mode(struct task_struct *tsk) { return false; }
static inline void nt_syscall_mode_set(struct task_struct *tsk) { }
static inline void nt_syscall_mode_clear(struct task_struct *tsk) { }

#endif /* CONFIG_NT_FS_PERSONALITY */

#endif /* _LINUX_NT_SYSCALL_H */
