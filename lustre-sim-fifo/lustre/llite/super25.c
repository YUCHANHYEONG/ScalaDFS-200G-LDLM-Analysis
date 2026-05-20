/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2016, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 */

#define DEBUG_SUBSYSTEM S_LLITE

#define D_MOUNT (D_SUPER | D_CONFIG/*|D_WARNING */)

#include <linux/module.h>
#include <linux/types.h>
#include <linux/version.h>
#include <lustre_ha.h>
#include <lustre_dlm.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/random.h>
#include <lprocfs_status.h>
#include "llite_internal.h"
#include "vvp_internal.h"

#include "../include/calclock.h"
#include "../include/cl_object.h"
#include "../osc/osc_internal.h"
#include "../mdc/mdc_internal.h"
#include <linux/lflist.h>

static struct kmem_cache *ll_inode_cachep;

static struct inode *ll_alloc_inode(struct super_block *sb)
{
	struct ll_inode_info *lli;
#ifdef HAVE_ALLOC_INODE_SB
	lli = alloc_inode_sb(sb, ll_inode_cachep, GFP_NOFS);
	if (!lli)
		return NULL;
	OBD_ALLOC_POST(lli, sizeof(*lli), "slab-alloced");
	memset(lli, 0, sizeof(*lli));
#else
	OBD_SLAB_ALLOC_PTR_GFP(lli, ll_inode_cachep, GFP_NOFS);
	if (!lli)
		return NULL;
#endif
	inode_init_once(&lli->lli_vfs_inode);
	lli->lli_open_thrsh_count = UINT_MAX;

	return &lli->lli_vfs_inode;
}

static void ll_inode_destroy_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	struct ll_inode_info *ptr = ll_i2info(inode);
	llcrypt_free_inode(inode);
	OBD_SLAB_FREE_PTR(ptr, ll_inode_cachep);
}

static void ll_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, ll_inode_destroy_callback);
}

static int ll_drop_inode(struct inode *inode)
{
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	int drop;

	if (!sbi->ll_inode_cache_enabled)
		return 1;

	drop = generic_drop_inode(inode);
	if (!drop)
		drop = llcrypt_drop_inode(inode);

	return drop;
}

/* exported operations */
const struct super_operations lustre_super_operations =
{
	.alloc_inode   = ll_alloc_inode,
	.destroy_inode = ll_destroy_inode,
	.drop_inode    = ll_drop_inode,
	.evict_inode   = ll_delete_inode,
	.put_super     = ll_put_super,
	.statfs        = ll_statfs,
	.umount_begin  = ll_umount_begin,
	.remount_fs    = ll_remount_fs,
	.show_options  = ll_show_options,
};

/**
 * This is the entry point for the mount call into Lustre.
 * This is called when a client is mounted, and this is
 * where we start setting things up.
 *
 * @lmd2data data Mount options (e.g. -o flock,abort_recov)
 */
static int lustre_fill_super(struct super_block *sb, void *lmd2_data,
			     int silent)
{
	struct lustre_mount_data *lmd;
	struct lustre_sb_info *lsi;
	int rc;

	ENTRY;

	CDEBUG(D_MOUNT|D_VFSTRACE, "VFS Op: sb %p\n", sb);

	lsi = lustre_init_lsi(sb);
	pr_info("lustre_fill_super\n");
	if (!lsi)
		RETURN(-ENOMEM);
	lmd = lsi->lsi_lmd;

	/*
	 * Disable lockdep during mount, because mount locking patterns are
	 * 'special'.
	 */
	lockdep_off();

	/*
	 * LU-639: the OBD cleanup of last mount may not finish yet, wait here.
	 */
	obd_zombie_barrier();

	/* Figure out the lmd from the mount options */
	if (lmd_parse(lmd2_data, lmd)) {
		lustre_put_lsi(sb);
		GOTO(out, rc = -EINVAL);
	}

	if (!lmd_is_client(lmd)) {
#ifdef HAVE_SERVER_SUPPORT
		static bool printed;

		if (!printed) {
			LCONSOLE_WARN("%s: mounting server target with '-t lustre' deprecated, use '-t lustre_tgt'\n",
				      lmd->lmd_profile);
			printed = true;
		}
		rc = server_fill_super(sb);
#else
		rc = -ENODEV;
		CERROR("%s: This is client-side-only module, cannot handle server mount: rc = %d\n",
		       lmd->lmd_profile, rc);
		lustre_put_lsi(sb);
#endif
		GOTO(out, rc);
	}

	CDEBUG(D_MOUNT, "Mounting client %s\n", lmd->lmd_profile);
	rc = lustre_start_mgc(sb);
	if (rc) {
		lustre_common_put_super(sb);
		GOTO(out, rc);
	}
	/* Connect and start */
	rc = ll_fill_super(sb);
	/* ll_file_super will call lustre_common_put_super on failure,
	 * which takes care of the module reference.
	 *
	 * If error happens in fill_super() call, @lsi will be killed there.
	 * This is why we do not put it here.
	 */
out:
	if (rc) {
		CERROR("llite: Unable to mount %s: rc = %d\n",
		       s2lsi(sb) ? lmd->lmd_dev : "<unknown>", rc);
	} else {
		CDEBUG(D_SUPER, "%s: Mount complete\n",
		       lmd->lmd_dev);
	}
	lockdep_on();
	return rc;
}

/***************** FS registration ******************/
static struct dentry *lustre_mount(struct file_system_type *fs_type, int flags,
				   const char *devname, void *data)
{
	return mount_nodev(fs_type, flags, data, lustre_fill_super);
}

static void lustre_kill_super(struct super_block *sb)
{
	struct lustre_sb_info *lsi = s2lsi(sb);

	if (lsi && !IS_SERVER(lsi))
		ll_kill_super(sb);

	kill_anon_super(sb);
}

/** Register the "lustre" fs type
 */
static struct file_system_type lustre_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "lustre",
	.mount		= lustre_mount,
	.kill_sb	= lustre_kill_super,
	.fs_flags	= FS_RENAME_DOES_D_MOVE,
};
MODULE_ALIAS_FS("lustre");

// extern int (*io_submit_one_in_lustre)(struct kioctx *ctx, struct iocb __user *user_iocb,
// 			 bool compat);
// extern int lustre_io_submit_one(struct kioctx *ctx, struct iocb __user *user_iocb,
// 			 bool compat);

extern struct lflist_head lf_b_more_io;

extern void lustre_iput(struct inode *inode);
extern void (*iput_in_lustre)(struct inode *inode);

// mempool_t *lustre_mempool;
#define POOL_SIZE 10000

static int __init lustre_init(void)
{
	int rc;
	unsigned long lustre_inode_cache_flags;

	pr_info("Insert module \n");

	init_lflist_head(&lf_b_more_io);

	// io_submit_one_in_lustre = &lustre_io_submit_one;
	iput_in_lustre = NULL;

	BUILD_BUG_ON(sizeof(LUSTRE_VOLATILE_HDR) !=
		     LUSTRE_VOLATILE_HDR_LEN + 1);

	/* print an address of _any_ initialized kernel symbol from this
	 * module, to allow debugging with gdb that doesn't support data
	 * symbols from modules.*/
	CDEBUG(D_INFO, "Lustre client module (%p).\n",
	       &lustre_super_operations);

	lustre_inode_cache_flags = SLAB_HWCACHE_ALIGN | SLAB_RECLAIM_ACCOUNT |
				   SLAB_MEM_SPREAD;
#ifdef SLAB_ACCOUNT
	lustre_inode_cache_flags |= SLAB_ACCOUNT;
#endif

	ll_inode_cachep = kmem_cache_create("lustre_inode_cache",
					    sizeof(struct ll_inode_info),
					    0, lustre_inode_cache_flags, NULL);
	if (ll_inode_cachep == NULL)
		GOTO(out_cache, rc = -ENOMEM);

	ll_file_data_slab = kmem_cache_create("ll_file_data",
						 sizeof(struct ll_file_data), 0,
						 SLAB_HWCACHE_ALIGN, NULL);
	if (ll_file_data_slab == NULL)
		GOTO(out_cache, rc = -ENOMEM);

	pcc_inode_slab = kmem_cache_create("ll_pcc_inode",
					   sizeof(struct pcc_inode), 0,
					   SLAB_HWCACHE_ALIGN, NULL);
	if (pcc_inode_slab == NULL)
		GOTO(out_cache, rc = -ENOMEM);

	rc = llite_tunables_register();
	if (rc)
		GOTO(out_cache, rc);

	rc = vvp_global_init();
	if (rc != 0)
		GOTO(out_tunables, rc);

	cl_inode_fini_env = cl_env_alloc(&cl_inode_fini_refcheck,
					 LCT_REMEMBER | LCT_NOREF);
	if (IS_ERR(cl_inode_fini_env))
		GOTO(out_vvp, rc = PTR_ERR(cl_inode_fini_env));

	cl_inode_fini_env->le_ctx.lc_cookie = 0x4;

	rc = ll_xattr_init();
	if (rc != 0)
		GOTO(out_inode_fini_env, rc);

	rc = register_filesystem(&lustre_fs_type);
	if (rc)
		GOTO(out_xattr, rc);

	// lustre_mempool = mempool_create(POOL_SIZE, mempool_alloc_pages, mempool_free_pages, NULL);
    // if (!lustre_mempool) {
    //     printk(KERN_ERR "Failed to create page mempool\n");
    //     GOTO(out_xattr, rc);
    // }

	RETURN(0);

out_xattr:
	ll_xattr_fini();
out_inode_fini_env:
	cl_env_put(cl_inode_fini_env, &cl_inode_fini_refcheck);
out_vvp:
	vvp_global_fini();
out_tunables:
	llite_tunables_unregister();
out_cache:
	kmem_cache_destroy(ll_inode_cachep);
	kmem_cache_destroy(ll_file_data_slab);
	kmem_cache_destroy(pcc_inode_slab);
	return rc;
}

KTDEC(ll_file_write_iter);
KTDEC(cl_io_loop);
KTDEC(ll_file_io_generic);
KTDEC(cl_io_loop_internal);
KTDEC(cl_io_lock);
KTDEC(cl_io_iter_init);
KTDEC(cl_io_start);
KTDEC(cl_io_end);
KTDEC(cl_io_unlock);
KTDEC(cl_io_iter_fini);
KTDEC(cl_io_rw_advance);

KTDEC(vvp_io_write_start);
KTDEC(vvp_io_lseek_start);
KTDEC(vvp_io_fault_start);
KTDEC(vvp_io_setattr_start);
KTDEC(vvp_io_read_start);
KTDEC(cis_op);
KTDEC(generic_file_read_iter);
KTDEC(__generic_file_write_iter);
KTDEC(generic_write_sync);
KTDEC(vvp_io_write_commit);

KTDEC(vvp_io_write_lock);
KTDEC(vvp_io_read_lock);
KTDEC(cl_lockset_lock);

KTDEC(cl_lock_enqueue);
KTDEC(osc_lock_enqueue);
KTDEC(osc_enqueue_base);

KTDEC(mdc_lock_enqueue);
KTDEC(cl_lock_init);

KTDEC(ldlm_lock_match_with_skip);
KTDEC(ldlm_cli_enqueue);
KTDEC(ll_direct_IO_impl);
KTDEC(cl_sync_io_wait_recycle);
KTDEC(cl_sync_io_wait);
KTDEC(cl_sync_io_wait_event_idle);
KTDEC(osc_io_end);
KTDEC(osc_io_fsync_end);
KTDEC(osc_io_fsync_end_wait_for_completion);
KTDEC(osc_cache_wait_range);

KTDEC(osc_lock_upcall);
KTDEC(osc_set_lock_data_wrap);
KTDEC(directIO);

KTDEC(ll_direct_rw_pages);
// KTDEC(lustre_io_submit_one);
KTDEC(osc_io_submit);
KTDEC(search_itree);
KTDEC(ll_atomic_open);

KTDEC(mdc_enqueue_send);
KTDEC(ptlrpc_queue_wait);
KTDEC(ptlrpc_set_wait);

KTDEC(ptlrpc_wait_event_abortable);
KTDEC(ptlrpc_wait_event_idle);
KTDEC(ptlrpc_send_new_req);
KTDEC(mdc_enqueue_base);
KTDEC(ll_intent_file_open);
KTDEC(ptl_send_buf);

KTDEC(ll_lookup_it);
KTDEC(md_intent_lock);
KTDEC(lmv_intent_lock);
KTDEC(lmv_intent_open);
KTDEC(md_intent_lock_lmv);
KTDEC(mdc_enqueue_base_mdt);
KTDEC(ldlm_cli_enqueue_mdt);
KTDEC(ptlrpc_queue_wait_mdt);
KTDEC(ptlrpc_get_mod_rpc_slot);
KTDEC(obd_get_mod_rpc_slot);
KTDEC(obd_get_mod_rpc_slot_wait);
KTDEC(ptlrpc_main);
KTDEC(claim_mod_rpc_function);
KTDEC(obd_get_request_slot);
KTDEC(obd_get_request_slot_wait);
KTDEC(ptlrpc_wait_event_abortable_mdt);
KTDEC(ptlrpc_wait_event_idle_mdt);
KTDEC(rq_lock);
KTDEC(cl_sync_io_wait_submit_sync);
KTDEC(cl_io_commit_async);
KTDEC(osc_page_cache_add);
KTDEC(osc_queue_async_io);
KTDEC(ll_merge_attr);
KTDEC(ll_inode_size_lock);
KTDEC(lli_size_mutex);
KTDEC(cl_loi_list_lock);
KTDEC(osc_enter_cache);
KTDEC(osc_enter_cache_wait);

KTDEC(md_intent_lock_ll_open);
KTDEC(osc_extent_release);
KTDEC(ptlrpc_check_set);

KTDEC(vvp_io_commit_sync);

KTDEC(ll_file_release);

KTDEC(ll_getattr_dentry);
KTDEC(osc_io_unplug0_sync);
KTDEC(osc_enter_cache_try);
KTDEC(osc_enter_cache_atomic);
KTDEC(application_check_req);

KTDEC(balance_dirty_pages_ratelimited);
KTDEC(iov_iter_copy_from_user_atomic);
KTDEC(write_begin);
KTDEC(write_end);
KTDEC(grab_cache_page_nowait);
KTDEC(find_get_entry);
KTDEC(page_cache_alloc);
KTDEC(cl_page_find);
KTDEC(sptlrpc_import_check_ctx);

KTDEC(ll_writepages);
KTDEC(wb_check_background_flush);
KTDEC(__writeback_inodes_wb);
KTDEC(__writeback_single_inode);
KTDEC(inode_to_wb_and_lock_list);
KTDEC(wb_do_writeback);
KTDEC(io_throttling);

KTDEC(add_to_page_cache_lru);
KTDEC(__add_to_page_cache_locked);
KTDEC(lru_cache_add);
KTDEC(__page_cache_alloc);
KTDEC(__alloc_pages_slowpath);
KTDEC(__alloc_pages_nodemask);
KTDEC(get_page_from_freelist);
KTDEC(prepare_alloc_pages);
KTDEC(cl_sync_file_range);
KTDEC(cl_sync_loop);
KTDEC(set_bit_wb);
KTDEC(lustre_wb_writeback_spinlock);
KTDEC(wb_over_bg_thresh);
KTDEC(lustre_requeue_inode);
KTDEC(InsertInode);
KTDEC(application_io_unplug);
KTDEC(application_check_set);
KTDEC(kiet_spin_lock);

KTDEC(sync_cl_io_lock);
KTDEC(sync_cl_io_iter_init);
KTDEC(sync_cl_io_start);
KTDEC(sync_cl_io_end);
KTDEC(sync_cl_io_unlock);
KTDEC(sync_cl_io_iter_fini);
KTDEC(sync_cl_io_rw_advance);

KTDEC(wait_woken);
// KTDEC(ksocknal_process_receive_wait);
// KTDEC(ksocknal_process_receive);
// KTDEC(ksocknal_process_transmit);

KTDEC(osc_extent_find);
KTDEC(osc_extent_search);

KTDEC(test);
KTDEC(lock_res);
KTDEC(lock_res_and_lock);

KTDEC(ldlm_lock_decref);
KTDEC(ldlm_lock_add_to_lru);

KTDEC(ptlrpcd_check);
KTDEC(cl_io_lru_reserve);
KTDEC(osc_lru_reserve_wait);
KTDEC(osc_lru_reserve);
KTDEC(sync_cio_start);
KTDEC(lov_io_start);
KTDEC(osc_io_fsync_start);
KTDEC(osc_cache_writeback_range);
KTDEC(osc_io_unplug);
KTDEC(osc_cache_while);
KTDEC(osc_extent_make_ready_top);
KTDEC(osc_extent_make_ready_bottom);

// osc_extent_finish
KTDEC(osc_cache_bottom);
KTDEC(osc_extent_finish_top);
KTDEC(osc_extent_finish_bottom);

KTDEC(application_check_set_INTERPRET);
KTDEC(application_check_set_RPC);
KTDEC(application_check_set_bot_1);
KTDEC(application_check_set_bot);
KTDEC(application_check_set_BULK);
KTDEC(application_check_set_middle);
KTDEC(application_check_set_middle_1);
KTDEC(application_check_set_atomic);
KTDEC(application_check_set_for);
KTDEC(osc_lru_reclaim);

KTDEC(ptlrpc_check_set_rq_lock);
KTDEC(ptlrpc_req_interpret);
KTDEC(brw_interpret);
KTDEC(osc_brw_fini_request);

KTDEC(application_cond_resched);

KTDEC(lustre_balance_dirty_pages);
KTDEC(lustre_wb_workfn);

KTDEC(wb_workfn);

static void __exit lustre_exit(void)
{
	unregister_filesystem(&lustre_fs_type);

	llite_tunables_unregister();
	pr_info("llite_tunables_unregister \n");

	ll_xattr_fini();
	cl_env_put(cl_inode_fini_env, &cl_inode_fini_refcheck);
	pr_info("cl_env_put \n");
	vvp_global_fini();

	/*
	 * Make sure all delayed rcu free inodes are flushed before we
	 * destroy cache.
	 */
	rcu_barrier();

	iput_in_lustre = NULL;

	kmem_cache_destroy(ll_inode_cachep);
	kmem_cache_destroy(ll_file_data_slab);
	kmem_cache_destroy(pcc_inode_slab);
	pr_info("Destroy cache done \n");
	// mempool_destroy(lustre_mempool);

	// ktprint(0, ll_atomic_open);
	// ktprint(1, mdc_enqueue_base);
	// ktprint(1, ldlm_cli_enqueue);
	// ktprint(1, mdc_enqueue_send);
	// ktprint(2, ptlrpc_queue_wait);
	// ktprint(3, ptlrpc_set_wait);
	// ktprint(3, ptl_send_buf);
	// ktprint(4, ptlrpc_send_new_req);
	// ktprint(4, ptlrpc_wait_event_abortable);
	// ktprint(4, ptlrpc_wait_event_idle);

	ktprint(0, ll_file_io_generic);
	ktprint(1, ll_file_write_iter);
	ktprint(2, lustre_balance_dirty_pages);
	ktprint(2, lustre_wb_workfn);
	ktprint(3, cl_sync_io_wait_recycle);
	ktprint(4, cl_sync_io_wait);
	ktprint(5, cl_sync_io_wait_event_idle);
	ktprint(5, ll_direct_IO_impl);
	ktprint(2, cl_io_loop);

	ktprint(3, cl_io_loop_internal);
	ktprint(4, cl_io_lock);
	ktprint(5, cl_lockset_lock);
	ktprint(6, cl_lock_init);
	ktprint(6, cl_lock_enqueue);
	ktprint(7, osc_lock_enqueue);
	ktprint(8, osc_enqueue_base);
	ktprint(9, ldlm_lock_match_with_skip);
	ktprint(10, sptlrpc_import_check_ctx);
	ktprint(10, lock_res);
	ktprint(10, search_itree);
	ktprint(9, ldlm_cli_enqueue);
	ktprint(9, osc_lock_upcall);
	ktprint(9, ldlm_lock_decref);
	ktprint(10, ldlm_lock_add_to_lru);
	ktprint(10, lock_res_and_lock);
	ktprint(9, osc_set_lock_data_wrap);
	ktprint(7, mdc_lock_enqueue);
	// ktprint(5, vvp_io_write_lock);
	// ktprint(5, vvp_io_read_lock);

	ktprint(4, cl_io_iter_init);
	ktprint(4, cl_io_start);
	// ktprint(5, cis_op);
	ktprint(6, vvp_io_write_start);
	ktprint(7, cl_io_lru_reserve);
	ktprint(8, osc_lru_reserve);
	ktprint(9, osc_lru_reserve_wait);
	ktprint(9, osc_lru_reclaim);
	ktprint(7, __generic_file_write_iter);
	ktprint(8, write_begin);
	ktprint(9, grab_cache_page_nowait);
	ktprint(10, find_get_entry);
	ktprint(10, page_cache_alloc);
	ktprint(11, __alloc_pages_nodemask);
	ktprint(12, prepare_alloc_pages);
	ktprint(12, __alloc_pages_slowpath);
	ktprint(12, get_page_from_freelist);
	ktprint(11, __page_cache_alloc);
	ktprint(10, add_to_page_cache_lru);
	ktprint(11, __add_to_page_cache_locked);
	ktprint(11, lru_cache_add);
	ktprint(9, cl_page_find);
	ktprint(8, iov_iter_copy_from_user_atomic);
	ktprint(8, balance_dirty_pages_ratelimited);
	ktprint(9, wb_do_writeback);
	ktprint(9, io_throttling);
	ktprint(10, set_bit_wb);
	ktprint(10, wb_check_background_flush);
	ktprint(10, wb_over_bg_thresh);
	ktprint(11, lustre_wb_writeback_spinlock);
	ktprint(11, __writeback_inodes_wb);
	ktprint(11, lustre_requeue_inode);
	ktprint(12, InsertInode);
	ktprint(12, __writeback_single_inode);
	ktprint(12, inode_to_wb_and_lock_list);
	ktprint(13, ll_writepages);
	ktprint(14, cl_sync_file_range);
	ktprint(15, cl_sync_loop);
	ktprint(16, sync_cl_io_lock);
	ktprint(16,sync_cl_io_iter_init);
	ktprint(16,sync_cl_io_start);
	ktprint(17, sync_cio_start);
	// ktprint(18, lov_io_start);
	ktprint(18, osc_io_fsync_start);
	ktprint(19, osc_cache_writeback_range);
	ktprint(20, osc_io_unplug);
	ktprint(20, osc_cache_while);
	ktprint(21, osc_extent_make_ready_top);
	ktprint(21, osc_extent_make_ready_bottom);

	ktprint(20, osc_cache_bottom);
	ktprint(21, osc_extent_finish_top);
	ktprint(21, osc_extent_finish_bottom);
	
	ktprint(16,sync_cl_io_end);
	ktprint(16,sync_cl_io_unlock);
	ktprint(16,sync_cl_io_iter_fini);
	ktprint(16,sync_cl_io_rw_advance);
	ktprint(8, directIO);
	ktprint(9, ll_direct_IO_impl);
	ktprint(10, ll_direct_rw_pages);
	ktprint(11, test);
	ktprint(7, generic_write_sync);
	ktprint(7, vvp_io_write_commit);
	ktprint(8, ll_merge_attr);
	ktprint(9, ll_inode_size_lock);
	ktprint(10, lli_size_mutex);
	ktprint(8, cl_io_commit_async);
	ktprint(9, osc_page_cache_add);
	ktprint(10, osc_queue_async_io);
	ktprint(11, osc_extent_find);
	ktprint(12, osc_extent_search);
	ktprint(11, osc_enter_cache_try);
	ktprint(12, osc_enter_cache_atomic);
	ktprint(11, cl_loi_list_lock);
	ktprint(11, osc_extent_release);
	ktprint(11, osc_enter_cache);
	ktprint(12, osc_enter_cache_wait);
	ktprint(12, application_io_unplug);
	ktprint(12, application_check_req);
	ktprint(13, application_check_set);
	ktprint(14, ptlrpc_send_new_req);
	ktprint(14, application_cond_resched);
	ktprint(14, application_check_set_bot_1);
	ktprint(14, application_check_set_bot);
	ktprint(14, application_check_set_RPC);
	ktprint(14, application_check_set_middle);
	ktprint(14, application_check_set_middle_1);
	ktprint(14, application_check_set_INTERPRET);
	ktprint(14, application_check_set_atomic);
	ktprint(14, application_check_set_for);
	// ktprint(13, osc_io_unplug0_sync);
	// ktprint(13, ptlrpc_check_set);
	ktprint(8, vvp_io_commit_sync);
	ktprint(9, cl_sync_io_wait_submit_sync);
	ktprint(10, cl_sync_io_wait_event_idle);
	// ktprint(6, vvp_io_lseek_start);
	// ktprint(6, vvp_io_fault_start);
	// ktprint(6, vvp_io_setattr_start);
	ktprint(6, vvp_io_read_start);
	ktprint(7, generic_file_read_iter);
	ktprint(4, cl_io_end);
	ktprint(6, osc_io_end);
	ktprint(6, osc_io_fsync_end);
	ktprint(7, osc_io_fsync_end_wait_for_completion);
	ktprint(7, osc_cache_wait_range);
	ktprint(4, cl_io_unlock);
	ktprint(4, cl_io_iter_fini);
	ktprint(4, cl_io_rw_advance);


	ktprint(2, rq_lock);
	ktprint(2, kiet_spin_lock);
	pr_info("\n");

	// ktprint(0, ksocknal_process_receive);
	// ktprint(0, ksocknal_process_receive_wait);
	// ktprint(0, ksocknal_process_transmit);

	ktprint(0, ptlrpcd_check);
	ktprint(1, ptlrpc_check_set);
	ktprint(2, ptlrpc_check_set_rq_lock);
	ktprint(2, ptlrpc_req_interpret);
	ktprint(3, brw_interpret);
	ktprint(4, osc_brw_fini_request);
	ktprint(1, wait_woken);

	// ktreset(ksocknal_process_receive);
	// ktreset(ksocknal_process_receive_wait);
	// ktreset(ksocknal_process_transmit);

	// ktprint(0, osc_io_submit);

	// ktprint(0, ll_intent_file_open);
	// ktprint(1, md_intent_lock_ll_open);
	// ktprint(3, mdc_enqueue_base_mdt);
	// ktprint(4, ldlm_cli_enqueue_mdt);
	// ktprint(5, ptlrpc_queue_wait_mdt);
	// // ktprint(9, ptlrpc_wait_event_abortable_mdt);
	// ktprint(6, ptlrpc_wait_event_idle_mdt);
	// ktprint(7, rq_lock);
	// ktprint(5, ptlrpc_get_mod_rpc_slot);
	// ktprint(6, obd_get_mod_rpc_slot);
	// ktprint(7, obd_get_mod_rpc_slot_wait);
	// ktprint(5, obd_get_request_slot);
	// ktprint(7, obd_get_request_slot_wait);
	// ktprint(0, ll_lookup_it);
	// ktprint(1, md_intent_lock);
	// ktprint(3, lmv_intent_lock);
	// ktprint(4, lmv_intent_open);
	// ktprint(5, md_intent_lock_lmv);
	// // ktprint(6, mdc_enqueue_base_mdt);
	// // ktprint(0, claim_mod_rpc_function);
	// // ktprint(0, ptlrpc_main);
	// ktprint(0, ll_file_release);
	// ktprint(0, ll_getattr_dentry);

	// io_submit_one_in_lustre = NULL;
}

MODULE_AUTHOR("OpenSFS, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Client File System");
MODULE_VERSION(LUSTRE_VERSION_STRING);
MODULE_LICENSE("GPL");

module_init(lustre_init);
module_exit(lustre_exit);
