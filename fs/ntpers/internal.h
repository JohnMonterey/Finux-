/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NT filesystem personality - internal declarations.
 *
 * Nothing here is part of the interface offered to the rest of the
 * kernel; see include/linux/nt_personality.h for that.
 */
#ifndef _FS_NTPERS_INTERNAL_H
#define _FS_NTPERS_INTERNAL_H

#include <linux/nt_personality.h>

struct dentry;
struct inode;
struct seq_file;

#define NTPERS_NAME "ntpers"

#undef pr_fmt
#define pr_fmt(fmt) NTPERS_NAME ": " fmt

/* --- volume.c -------------------------------------------------------- */

void nt_volume_free(struct nt_volume *vol);

/* --- namespace.c ----------------------------------------------------- */

void nt_ns_free(struct nt_namespace *ns);
int nt_namespace_init(void);
void nt_namespace_exit(void);

/* --- casefold.c ------------------------------------------------------ */

int nt_casefold_init(void);
void nt_casefold_exit(void);

/* --- debugfs.c ------------------------------------------------------- */

#ifdef CONFIG_NT_FS_PERSONALITY_DEBUGFS
int nt_debugfs_init(void);
void nt_debugfs_exit(void);
#else
static inline int nt_debugfs_init(void) { return 0; }
static inline void nt_debugfs_exit(void) { }
#endif

/* --- main.c ---------------------------------------------------------- */

extern struct kmem_cache *nt_volume_cache;
extern struct kmem_cache *nt_ctx_cache;

#endif /* _FS_NTPERS_INTERNAL_H */
