// SPDX-License-Identifier: GPL-2.0
/*
 * NT namespaces and per-process personality state.
 *
 * struct nt_namespace owns the drive-letter table.  There is one per
 * machine by default (init_nt_ns); it is refcounted so that a
 * container-visible NT namespace can be added later without touching any
 * of its users.
 *
 * struct nt_task_ctx is per-process state: which namespace the process
 * resolves against, whether it has opted into NT pathname rules, its
 * current drive, and its remembered current directory on other drives.
 * It hangs off struct fs_struct, which means it follows exactly the same
 * sharing rules as the root and current directory: threads share it, and
 * CLONE_FS shares it across processes.  That matches Windows, where the
 * current directory is a property of the process, not the thread.
 *
 * A process with no context behaves exactly as it does today.  Nothing in
 * this file changes POSIX pathname resolution.
 */

#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/utsname.h>
#include <linux/nt_personality.h>

#include "internal.h"

/*
 * The machine-wide NT namespace.
 *
 * Statically allocated so that it exists before any filesystem is
 * mounted and can never fail to be available.
 */
struct nt_namespace init_nt_ns = {
	.count		= REFCOUNT_INIT(1),
	.lock		= __SPIN_LOCK_UNLOCKED(init_nt_ns.lock),
	.volumes	= LIST_HEAD_INIT(init_nt_ns.volumes),
	.computer_name	= "FINUX",
};
EXPORT_SYMBOL_GPL(init_nt_ns);

/**
 * nt_ns_create - allocate a new, empty NT namespace
 *
 * Returns the namespace with one reference, or NULL.
 */
struct nt_namespace *nt_ns_create(void)
{
	struct nt_namespace *ns;

	ns = kzalloc_obj(struct nt_namespace);
	if (!ns)
		return NULL;

	refcount_set(&ns->count, 1);
	spin_lock_init(&ns->lock);
	INIT_LIST_HEAD(&ns->volumes);
	strscpy(ns->computer_name, init_nt_ns.computer_name,
		sizeof(ns->computer_name));

	return ns;
}
EXPORT_SYMBOL_GPL(nt_ns_create);

/**
 * nt_ns_get - take a reference on a namespace
 * @ns: the namespace, may be NULL
 */
struct nt_namespace *nt_ns_get(struct nt_namespace *ns)
{
	if (ns)
		refcount_inc(&ns->count);
	return ns;
}
EXPORT_SYMBOL_GPL(nt_ns_get);

static void nt_ns_free_rcu(struct rcu_head *head)
{
	struct nt_namespace *ns = container_of(head, struct nt_namespace, rcu);

	kfree(ns);
}

void nt_ns_free(struct nt_namespace *ns)
{
	struct nt_volume *vol, *tmp;

	/*
	 * Nothing can reach the namespace any more, so the volume list
	 * cannot change under us.  Drop the namespace's reference on each
	 * volume; anyone still holding one keeps a working volume until
	 * they let go.
	 */
	list_for_each_entry_safe(vol, tmp, &ns->volumes, list) {
		list_del_init(&vol->list);
		nt_volume_put(vol);
	}

	call_rcu(&ns->rcu, nt_ns_free_rcu);
}

/**
 * nt_ns_put - drop a reference on a namespace
 * @ns: the namespace, may be NULL
 */
void nt_ns_put(struct nt_namespace *ns)
{
	if (!ns || ns == &init_nt_ns)
		return;
	if (refcount_dec_and_test(&ns->count))
		nt_ns_free(ns);
}
EXPORT_SYMBOL_GPL(nt_ns_put);

/**
 * nt_ctx_alloc - allocate a per-process NT context
 * @ns: namespace it should resolve against; a reference is taken
 *
 * The context starts with the personality disabled, so allocating one
 * changes no behaviour until prctl() turns something on.
 *
 * Returns the context with one reference, or NULL.
 */
struct nt_task_ctx *nt_ctx_alloc(struct nt_namespace *ns)
{
	struct nt_task_ctx *ctx;

	ctx = kmem_cache_zalloc(nt_ctx_cache, GFP_KERNEL);
	if (!ctx)
		return NULL;

	refcount_set(&ctx->count, 1);
	spin_lock_init(&ctx->lock);
	nt_handle_table_init(&ctx->handles);
	ctx->ns = nt_ns_get(ns ? ns : &init_nt_ns);
	ctx->cur_drive = 'C';

	return ctx;
}
EXPORT_SYMBOL_GPL(nt_ctx_alloc);

/**
 * nt_ctx_get - take a reference on a context
 * @ctx: the context, may be NULL
 */
struct nt_task_ctx *nt_ctx_get(struct nt_task_ctx *ctx)
{
	if (ctx)
		refcount_inc(&ctx->count);
	return ctx;
}
EXPORT_SYMBOL_GPL(nt_ctx_get);

/**
 * nt_ctx_put - drop a reference on a context
 * @ctx: the context, may be NULL
 */
void nt_ctx_put(struct nt_task_ctx *ctx)
{
	u8 i;

	if (!ctx || !refcount_dec_and_test(&ctx->count))
		return;

	/*
	 * Last reference to this process's NT state.  Close every handle still
	 * open first, so process exit releases the nt_open objects - and the
	 * mount and dentry references behind them - rather than leaking them.
	 */
	nt_handle_table_destroy(&ctx->handles);

	for (i = 0; i < ctx->nr_cwd; i++) {
		if (ctx->cwd[i].path.dentry)
			path_put(&ctx->cwd[i].path);
	}

	kfree(ctx->cwd);
	nt_ns_put(ctx->ns);
	kmem_cache_free(nt_ctx_cache, ctx);
}
EXPORT_SYMBOL_GPL(nt_ctx_put);

/**
 * nt_ctx_current - the calling process's NT context
 *
 * Returns the context without taking a reference.  The caller must be in
 * process context; the context lives as long as current->fs, so a
 * borrowed pointer is safe for the duration of a syscall.
 */
struct nt_task_ctx *nt_ctx_current(void)
{
	struct fs_struct *fs = current->fs;

	if (!fs)
		return NULL;
	return READ_ONCE(fs->nt_ctx);
}
EXPORT_SYMBOL_GPL(nt_ctx_current);

/**
 * nt_ctx_current_or_create - the calling process's NT context, allocating
 *			      one if it does not have one yet
 *
 * Returns the context without taking a reference, or NULL on allocation
 * failure.
 */
struct nt_task_ctx *nt_ctx_current_or_create(void)
{
	struct fs_struct *fs = current->fs;
	struct nt_task_ctx *ctx, *old;

	if (!fs)
		return NULL;

	ctx = READ_ONCE(fs->nt_ctx);
	if (ctx)
		return ctx;

	ctx = nt_ctx_alloc(&init_nt_ns);
	if (!ctx)
		return NULL;

	/*
	 * Two threads sharing this fs_struct can race here.  Publish under
	 * the fs_struct seqlock, which is what everything else that
	 * mutates fs_struct uses, and discard the loser's allocation.
	 */
	write_seqlock(&fs->seq);
	old = fs->nt_ctx;
	if (!old)
		fs->nt_ctx = ctx;
	write_sequnlock(&fs->seq);

	if (old) {
		nt_ctx_put(ctx);
		return old;
	}

	return ctx;
}
EXPORT_SYMBOL_GPL(nt_ctx_current_or_create);

/**
 * nt_ns_current - the namespace the calling process resolves against
 *
 * Falls back to the machine-wide namespace for processes that have no
 * context, so callers never have to handle NULL.  No reference is taken;
 * init_nt_ns is permanent and a process's own namespace lives as long as
 * its context.
 */
struct nt_namespace *nt_ns_current(void)
{
	struct nt_task_ctx *ctx = nt_ctx_current();

	return ctx ? ctx->ns : &init_nt_ns;
}
EXPORT_SYMBOL_GPL(nt_ns_current);

/**
 * nt_personality_get - read the calling process's personality flags
 *
 * Returns a mask of NT_PERSONALITY_*, or 0 if the process has no context.
 */
u32 nt_personality_get(void)
{
	struct nt_task_ctx *ctx = nt_ctx_current();

	return ctx ? READ_ONCE(ctx->flags) : 0;
}
EXPORT_SYMBOL_GPL(nt_personality_get);

/**
 * nt_personality_set - set the calling process's personality flags
 * @flags: mask of NT_PERSONALITY_*
 *
 * Setting flags to zero leaves the context allocated but inert.  There is
 * deliberately no privilege check: the personality only changes how this
 * process's own pathnames are interpreted, and it cannot reach anything
 * the process could not already reach through the ordinary VFS, because
 * every resolution still goes through the same permission checks.
 *
 * Returns 0, -EINVAL for unknown flags, or -ENOMEM.
 */
int nt_personality_set(u32 flags)
{
	struct nt_task_ctx *ctx;

	if (flags & ~NT_PERSONALITY_ALL_FLAGS)
		return -EINVAL;

	if (!flags && !nt_ctx_current())
		return 0;

	ctx = nt_ctx_current_or_create();
	if (!ctx)
		return -ENOMEM;

	WRITE_ONCE(ctx->flags, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(nt_personality_set);

/**
 * nt_ctx_set_current_drive - set the process's current drive
 * @ctx:    the context
 * @letter: drive letter, either case
 *
 * Returns 0 or -EINVAL.  The drive need not currently have a volume; NT
 * lets you sit on a drive that has gone away and fails on use.
 */
int nt_ctx_set_current_drive(struct nt_task_ctx *ctx, u8 letter)
{
	if (!ctx || nt_drive_index(letter) < 0)
		return -EINVAL;

	WRITE_ONCE(ctx->cur_drive, nt_drive_upper(letter));
	return 0;
}
EXPORT_SYMBOL_GPL(nt_ctx_set_current_drive);

/**
 * nt_ctx_set_drive_cwd - remember a current directory for one drive
 * @ctx:    the context
 * @letter: drive letter, either case
 * @path:   the directory; a reference is taken
 *
 * This is what makes "C:foo" mean something other than "C:\foo".  In real
 * Windows the per-drive current directory lives in the Win32 process
 * environment block as a hidden "=C:" variable, not in the NT kernel; we
 * keep it in the kernel because our Win32 personality has no PEB yet.
 *
 * The table is allocated on first use and holds one slot per drive
 * letter, indexed by letter.  A process that never uses a drive-relative
 * path pays nothing; one that does pays a single small allocation and can
 * never silently lose a remembered directory.
 *
 * Returns 0, -EINVAL, -ENOTDIR, or -ENOMEM.
 */
int nt_ctx_set_drive_cwd(struct nt_task_ctx *ctx, u8 letter,
			 const struct path *path)
{
	struct nt_drive_cwd *table = NULL;
	struct path old = {};
	int idx;

	if (!ctx || !path || !path->dentry)
		return -EINVAL;

	idx = nt_drive_index(letter);
	if (idx < 0)
		return -EINVAL;
	if (!d_is_dir(path->dentry))
		return -ENOTDIR;

	if (!READ_ONCE(ctx->cwd)) {
		table = kcalloc(NT_NR_DRIVES, sizeof(*table), GFP_KERNEL);
		if (!table)
			return -ENOMEM;
	}

	/* Take the reference before the lock; path_get() can be done here. */
	path_get(path);

	spin_lock(&ctx->lock);

	/*
	 * ctx->cwd is only ever installed, never cleared while the context
	 * is alive, so it is non-NULL here unless we brought the table.
	 * A concurrent installer simply wins and we free ours below.
	 */
	if (!ctx->cwd) {
		ctx->cwd = table;
		ctx->max_cwd = NT_NR_DRIVES;
		ctx->nr_cwd = NT_NR_DRIVES;
		table = NULL;
	}

	old = ctx->cwd[idx].path;
	ctx->cwd[idx].path = *path;
	ctx->cwd[idx].letter = nt_drive_upper(letter);

	spin_unlock(&ctx->lock);

	kfree(table);
	if (old.dentry)
		path_put(&old);
	return 0;
}
EXPORT_SYMBOL_GPL(nt_ctx_set_drive_cwd);

/**
 * nt_ctx_get_drive_cwd - look up the remembered directory for a drive
 * @ctx:    the context
 * @letter: drive letter, either case
 * @out:    filled in with a referenced path on success
 *
 * Returns true if a directory was remembered.  A false return is not an
 * error: Windows resolves a drive-relative path against the root of the
 * drive when nothing has been remembered for it.
 */
bool nt_ctx_get_drive_cwd(struct nt_task_ctx *ctx, u8 letter,
			  struct path *out)
{
	bool found = false;
	int idx;

	if (!ctx || !out || !READ_ONCE(ctx->cwd))
		return false;

	idx = nt_drive_index(letter);
	if (idx < 0)
		return false;

	spin_lock(&ctx->lock);
	if (ctx->cwd && ctx->cwd[idx].path.dentry) {
		*out = ctx->cwd[idx].path;
		path_get(out);
		found = true;
	}
	spin_unlock(&ctx->lock);

	return found;
}
EXPORT_SYMBOL_GPL(nt_ctx_get_drive_cwd);

/**
 * nt_fs_struct_copy - propagate NT context across a copy of fs_struct
 * @new_fs: the new fs_struct, already initialised
 * @old_fs: the fs_struct being copied
 *
 * Called from copy_fs_struct().  A copied fs_struct is a new process's
 * view of the filesystem, so it gets a private context seeded from the
 * parent's, exactly as it gets a private root and current directory.
 *
 * Returns 0 or -ENOMEM.
 */
int nt_fs_struct_copy(struct fs_struct *new_fs, struct fs_struct *old_fs)
{
	struct nt_task_ctx *old, *new;
	u8 i;

	new_fs->nt_ctx = NULL;

	old = READ_ONCE(old_fs->nt_ctx);
	if (!old)
		return 0;

	new = nt_ctx_alloc(old->ns);
	if (!new)
		return -ENOMEM;

	/*
	 * Personality flags, current drive and the per-drive cwd cache carry
	 * over.  The handle table deliberately does not: a plain fork starts
	 * with an empty one, and only threads (which share this fs_struct)
	 * share handles, exactly as NT inherits handles only when asked to.
	 */
	new->flags = READ_ONCE(old->flags);
	new->cur_drive = READ_ONCE(old->cur_drive);

	/*
	 * Allocate the copy's table before taking the lock: copy_fs_struct()
	 * is allowed to sleep and we would rather not use GFP_ATOMIC here.
	 * If the parent has no table there is nothing to inherit.
	 */
	if (READ_ONCE(old->cwd)) {
		struct nt_drive_cwd *table;

		table = kcalloc(NT_NR_DRIVES, sizeof(*table), GFP_KERNEL);
		if (!table) {
			nt_ctx_put(new);
			return -ENOMEM;
		}

		spin_lock(&old->lock);
		for (i = 0; i < old->nr_cwd; i++) {
			table[i] = old->cwd[i];
			if (table[i].path.dentry)
				path_get(&table[i].path);
		}
		new->nr_cwd = old->nr_cwd;
		spin_unlock(&old->lock);

		new->cwd = table;
		new->max_cwd = NT_NR_DRIVES;
	}

	new_fs->nt_ctx = new;
	return 0;
}
EXPORT_SYMBOL_GPL(nt_fs_struct_copy);

/**
 * nt_fs_struct_free - release the NT context of a dying fs_struct
 * @fs: the fs_struct
 *
 * Called from free_fs_struct().
 */
void nt_fs_struct_free(struct fs_struct *fs)
{
	nt_ctx_put(fs->nt_ctx);
	fs->nt_ctx = NULL;
}
EXPORT_SYMBOL_GPL(nt_fs_struct_free);

int __init nt_namespace_init(void)
{
	/*
	 * Seed the computer name from the boot-time hostname so that a
	 * Win32 personality has something sensible to report before
	 * anything configures it.  NetBIOS names are uppercase and at most
	 * 15 characters.
	 */
	char *p;

	down_read(&uts_sem);
	strscpy(init_nt_ns.computer_name, init_utsname()->nodename,
		sizeof(init_nt_ns.computer_name));
	up_read(&uts_sem);

	for (p = init_nt_ns.computer_name; *p; p++) {
		if (*p >= 'a' && *p <= 'z')
			*p -= 'a' - 'A';
		else if (*p == '.')
			*p = '\0';
	}

	if (!init_nt_ns.computer_name[0])
		strscpy(init_nt_ns.computer_name, "FINUX",
			sizeof(init_nt_ns.computer_name));

	return 0;
}

void nt_namespace_exit(void)
{
	struct nt_volume *vol, *tmp;
	LIST_HEAD(doomed);
	int i;

	spin_lock(&init_nt_ns.lock);
	list_splice_init(&init_nt_ns.volumes, &doomed);
	init_nt_ns.nr_volumes = 0;
	for (i = 0; i < NT_NR_DRIVES; i++)
		RCU_INIT_POINTER(init_nt_ns.drives[i], NULL);
	RCU_INIT_POINTER(init_nt_ns.system, NULL);
	spin_unlock(&init_nt_ns.lock);

	list_for_each_entry_safe(vol, tmp, &doomed, list) {
		list_del_init(&vol->list);
		nt_volume_put(vol);
	}
}
