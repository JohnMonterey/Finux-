// SPDX-License-Identifier: GPL-2.0
/*
 * NT filesystem personality - subsystem initialisation.
 *
 * Brings up the object caches, the namespace and the fold-hint cache
 * early enough that a volume can be registered as soon as the root
 * filesystem is mounted, and late enough that slab and Unicode are
 * available.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/nt_personality.h>

#include "internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/ntpers.h>

struct kmem_cache *nt_volume_cache __read_mostly;
struct kmem_cache *nt_ctx_cache __read_mostly;

static bool nt_ready __read_mostly;

/**
 * nt_personality_ready - has the subsystem finished initialising?
 *
 * Callers that can run before fs_initcall time must check this before
 * using anything else in the subsystem.
 */
bool nt_personality_ready(void)
{
	return READ_ONCE(nt_ready);
}
EXPORT_SYMBOL_GPL(nt_personality_ready);

static int __init nt_personality_init(void)
{
	int err;

	nt_volume_cache = KMEM_CACHE(nt_volume, SLAB_RECLAIM_ACCOUNT);
	if (!nt_volume_cache)
		return -ENOMEM;

	nt_ctx_cache = KMEM_CACHE(nt_task_ctx, SLAB_ACCOUNT);
	if (!nt_ctx_cache) {
		err = -ENOMEM;
		goto err_volume_cache;
	}

	err = nt_casefold_init();
	if (err)
		goto err_ctx_cache;

	err = nt_namespace_init();
	if (err)
		goto err_casefold;

	err = nt_share_subsystem_init();
	if (err)
		goto err_namespace;

	err = nt_debugfs_init();
	if (err)
		goto err_share;

	WRITE_ONCE(nt_ready, true);

	pr_info("NT filesystem personality registered (computer name %s)\n",
		init_nt_ns.computer_name);
	return 0;

err_share:
	nt_share_subsystem_exit();
err_namespace:
	nt_namespace_exit();
err_casefold:
	nt_casefold_exit();
err_ctx_cache:
	kmem_cache_destroy(nt_ctx_cache);
	nt_ctx_cache = NULL;
err_volume_cache:
	kmem_cache_destroy(nt_volume_cache);
	nt_volume_cache = NULL;
	return err;
}

/*
 * fs_initcall, not late_initcall: volumes have to be registerable before
 * userspace starts, so that a process which opts into the personality
 * during early boot finds a C: drive already there.
 */
fs_initcall(nt_personality_init);
