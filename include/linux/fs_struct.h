/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FS_STRUCT_H
#define _LINUX_FS_STRUCT_H

#include <linux/sched.h>
#include <linux/path.h>
#include <linux/spinlock.h>
#include <linux/seqlock.h>

struct nt_task_ctx;

struct fs_struct {
	int users;
	seqlock_t seq;
	int umask;
	int in_exec;
	struct path root, pwd;
#ifdef CONFIG_NT_FS_PERSONALITY
	/*
	 * NT filesystem personality state: current drive, per-drive
	 * current directories, and which NT namespace this process
	 * resolves against.  It lives here so that it is shared and
	 * copied on exactly the same terms as the root and current
	 * directory above.  NULL for every process that has not opted in,
	 * which is all of them until something calls prctl().
	 */
	struct nt_task_ctx *nt_ctx;
#endif
} __randomize_layout;

extern struct kmem_cache *fs_cachep;

extern void exit_fs(struct task_struct *);
extern void set_fs_root(struct fs_struct *, const struct path *);
extern void set_fs_pwd(struct fs_struct *, const struct path *);
extern struct fs_struct *copy_fs_struct(struct fs_struct *);
extern void free_fs_struct(struct fs_struct *);
extern int unshare_fs_struct(void);

static inline void get_fs_root(struct fs_struct *fs, struct path *root)
{
	read_seqlock_excl(&fs->seq);
	*root = fs->root;
	path_get(root);
	read_sequnlock_excl(&fs->seq);
}

static inline void get_fs_pwd(struct fs_struct *fs, struct path *pwd)
{
	read_seqlock_excl(&fs->seq);
	*pwd = fs->pwd;
	path_get(pwd);
	read_sequnlock_excl(&fs->seq);
}

extern bool current_chrooted(void);

static inline int current_umask(void)
{
	return current->fs->umask;
}

#endif /* _LINUX_FS_STRUCT_H */
