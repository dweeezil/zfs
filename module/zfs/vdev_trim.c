/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2016 by Delphix. All rights reserved.
 */

#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/txg.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_trim.h>
#include <sys/refcount.h>
#include <sys/metaslab_impl.h>
#include <sys/dsl_synctask.h>
#include <sys/zap.h>
#include <sys/dmu_tx.h>

/*
 * Maximum number of metaslabs per group that can be trimmed simultaneously.
 */
int max_trim_ms = 3;

/* Maximum number of TRIMs outstanding per leaf vdev */
int zfs_trim_limit = 1;

/*
 * Tunable to allow for debugging SCSI UNMAP/SATA TRIM calls. Disabling
 * it will prevent ZFS from attempting to issue DKIOCFREE ioctls to the
 * underlying storage.
 */
int zfs_trim_enabled = B_TRUE;

/*
 * Minimum size of TRIM commands, extents smaller than 128Kib will be skipped.
 */
uint64_t zfs_trim_min_extent_bytes = 128 * 1024;

/*
 * Maximum size of TRIM command, ranges will be chunked in to 128MiB extents.
 */
uint64_t zfs_trim_max_extent_bytes = 128 * 1024 * 1024;

/*
 * All TRIM commands will be handled synchronously.
 */
int zfs_trim_sync = B_TRUE;
/*
 * If we accumulate a lot of trim extents due to trim running slow, this
 * is the memory pressure valve. We limit the amount of memory consumed
 * by the extents in memory to physmem/zfs_trim_mem_lim_fact (by default
 * 2%). If we exceed this limit, we start throwing out new extents
 * without queueing them.
 */
int zfs_trim_mem_lim_fact = 50;

/*
 * How many TXG's worth of updates should be aggregated per TRIM/UNMAP
 * issued to the underlying vdev. We keep two range trees of extents
 * (called "trim sets") to be trimmed per metaslab, the `current' and
 * the `previous' TS. New free's are added to the current TS. Then,
 * once `zfs_txgs_per_trim' transactions have elapsed, the `current'
 * TS becomes the `previous' TS and a new, blank TS is created to be
 * the new `current', which will then start accumulating any new frees.
 * Once another zfs_txgs_per_trim TXGs have passed, the previous TS's
 * extents are trimmed, the TS is destroyed and the current TS again
 * becomes the previous TS.
 * This serves to fulfill two functions: aggregate many small frees
 * into fewer larger trim operations (which should help with devices
 * which do not take so kindly to them) and to allow for disaster
 * recovery (extents won't get trimmed immediately, but instead only
 * after passing this rather long timeout, thus preserving
 * 'zfs import -F' functionality).
 * The exact default value of this tunable is a tradeoff between:
 * 1) Keeping the trim commands reasonably small.
 * 2) Keeping the ability to rollback back for as many txgs as possible.
 * 3) Waiting around too long that the user starts to get uneasy about not
 *	seeing any space being freed after they remove some files.
 * The default value of 32 is the maximum number of uberblocks in a vdev
 * label, assuming a 4k physical sector size (which seems to be the almost
 * universal smallest sector size used in SSDs).
 */
unsigned int zfs_txgs_per_trim = 32;


static boolean_t
vdev_trim_should_stop(vdev_t *vd)
{
	return (vd->vdev_trim_exit_wanted || !vdev_writeable(vd) ||
	    vd->vdev_detached || vd->vdev_top->vdev_removing);
}

static void
vdev_trim_zap_update_sync(void *arg, dmu_tx_t *tx)
{
	/*
	 * We pass in the guid instead of the vdev_t since the vdev may
	 * have been freed prior to the sync task being processed. This
	 * happens when a vdev is detached as we call spa_config_vdev_exit(),
	 * stop the trimming thread, schedule the sync task, and free
	 * the vdev. Later when the scheduled sync task is invoked, it would
	 * find that the vdev has been freed.
	 */
	uint64_t guid = *(uint64_t *)arg;
	uint64_t txg = dmu_tx_get_txg(tx);
	kmem_free(arg, sizeof (uint64_t));

	vdev_t *vd = spa_lookup_by_guid(tx->tx_pool->dp_spa, guid, B_FALSE);
	if (vd == NULL || vd->vdev_top->vdev_removing || !vdev_is_concrete(vd))
		return;

	uint64_t last_offset = vd->vdev_trim_offset[txg & TXG_MASK];
	vd->vdev_trim_offset[txg & TXG_MASK] = 0;

	VERIFY(vd->vdev_leaf_zap != 0);

	objset_t *mos = vd->vdev_spa->spa_meta_objset;

	if (last_offset > 0) {
		vd->vdev_trim_last_offset = last_offset;
		VERIFY0(zap_update(mos, vd->vdev_leaf_zap,
		    VDEV_LEAF_ZAP_TRIM_LAST_OFFSET,
		    sizeof (last_offset), 1, &last_offset, tx));
	}
	if (vd->vdev_trim_action_time > 0) {
		uint64_t val = (uint64_t)vd->vdev_trim_action_time;
		VERIFY0(zap_update(mos, vd->vdev_leaf_zap,
		    VDEV_LEAF_ZAP_TRIM_ACTION_TIME, sizeof (val),
		    1, &val, tx));
	}
	if (vd->vdev_trim_rate > 0) {
		uint64_t val = (uint64_t)vd->vdev_trim_rate;
		VERIFY0(zap_update(mos, vd->vdev_leaf_zap,
		    VDEV_LEAF_ZAP_TRIM_RATE, sizeof (val),
		    1, &val, tx));
	}

	uint64_t fulltrim = vd->vdev_trim_full;
	VERIFY0(zap_update(mos, vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_FULL,
	    sizeof (fulltrim), 1, &fulltrim, tx));

	uint64_t trim_state = vd->vdev_trim_state;
	VERIFY0(zap_update(mos, vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_STATE,
	    sizeof (trim_state), 1, &trim_state, tx));
}

static void
vdev_trim_change_state(vdev_t *vd, vdev_trim_state_t new_state,
    uint64_t rate, boolean_t fulltrim)
{
	ASSERT(MUTEX_HELD(&vd->vdev_trim_lock));
	spa_t *spa = vd->vdev_spa;

	if (new_state == vd->vdev_trim_state)
		return;

	/*
	 * Copy the vd's guid, this will be freed by the sync task.
	 */
	uint64_t *guid = kmem_zalloc(sizeof (uint64_t), KM_SLEEP);
	*guid = vd->vdev_guid;

	/*
	 * If we're suspending, then preserve the original start time.
	 */
	if (vd->vdev_trim_state != VDEV_TRIM_SUSPENDED) {
		vd->vdev_trim_action_time = gethrestime_sec();
	}

	/*
	 * If we're activating, then preserve the requested rate and
	 * TRIM method.
	 */
	if (new_state == VDEV_TRIM_ACTIVE) {
		vd->vdev_trim_rate = rate;
		vd->vdev_trim_full = fulltrim;
	}

	boolean_t resumed = !!(vd->vdev_trim_state == VDEV_TRIM_SUSPENDED);
	vd->vdev_trim_state = new_state;

	dmu_tx_t *tx = dmu_tx_create_dd(spa_get_dsl(spa)->dp_mos_dir);
	VERIFY0(dmu_tx_assign(tx, TXG_WAIT));
	dsl_sync_task_nowait(spa_get_dsl(spa), vdev_trim_zap_update_sync,
	    guid, 2, ZFS_SPACE_CHECK_RESERVED, tx);

	switch (new_state) {
	case VDEV_TRIM_ACTIVE:
		spa_event_notify(spa, vd, NULL,
		    resumed ? ESC_ZFS_TRIM_RESUME : ESC_ZFS_TRIM_START);
		spa_history_log_internal(spa, "trim", tx,
		    "vdev=%s activated", vd->vdev_path);
		break;
	case VDEV_TRIM_SUSPENDED:
		spa_event_notify(spa, vd, NULL, ESC_ZFS_TRIM_SUSPEND);
		spa_history_log_internal(spa, "trim", tx,
		    "vdev=%s suspended", vd->vdev_path);
		break;
	case VDEV_TRIM_CANCELED:
		spa_event_notify(spa, vd, NULL, ESC_ZFS_TRIM_CANCEL);
		spa_history_log_internal(spa, "trim", tx,
		    "vdev=%s canceled", vd->vdev_path);
		break;
	case VDEV_TRIM_COMPLETE:
		spa_event_notify(spa, vd, NULL, ESC_ZFS_TRIM_FINISH);
		spa_history_log_internal(spa, "trim", tx,
		    "vdev=%s complete", vd->vdev_path);
		break;
	default:
		panic("invalid state %llu", (unsigned long long)new_state);
	}

	dmu_tx_commit(tx);
}

static void
vdev_trim_cb(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	mutex_enter(&vd->vdev_trim_io_lock);
	if (zio->io_error == ENXIO && !vdev_writeable(vd)) {
		/*
		 * The I/O failed because the vdev was unavailable; roll the
		 * last offset back. (This works because spa_sync waits on
		 * spa_txg_zio before it runs sync tasks.)
		 */
		uint64_t *off =
		    &vd->vdev_trim_offset[zio->io_txg & TXG_MASK];
		*off = MIN(*off, zio->io_offset);
	} else {
		/*
		 * Since trimming is best-effort, we ignore I/O errors and
		 * rely on vdev_probe to determine if the errors are more
		 * critical.
		 */
		if (zio->io_error != 0) {
			vd->vdev_stat.vs_trim_errors++;
		} else if (zio->io_dfl != NULL) {
			uint64_t length = 0;
			for (int i = 0; i < zio->io_dfl->dfl_num_exts; i++)
				length += zio->io_dfl->dfl_exts[i].dfle_length;

			vd->vdev_trim_bytes_done += length;
			spa_trimstats_update(vd->vdev_spa, 1, length, 0, 0);
		}
	}
	ASSERT3U(vd->vdev_trim_inflight, >, 0);
	vd->vdev_trim_inflight--;
	cv_broadcast(&vd->vdev_trim_io_cv);
	mutex_exit(&vd->vdev_trim_io_lock);

	spa_config_exit(vd->vdev_spa, SCL_STATE_ALL, vd);
}

/* Takes care of physical discards and limiting # of concurrent ZIOs. */
static int
vdev_trim_range(vdev_t *vd, uint64_t start, uint64_t size)
{
	spa_t *spa = vd->vdev_spa;

	/* Limit inflight trimming I/Os */
	mutex_enter(&vd->vdev_trim_io_lock);
	while (vd->vdev_trim_inflight >= zfs_trim_limit) {
		cv_wait(&vd->vdev_trim_io_cv,
		    &vd->vdev_trim_io_lock);
	}
	vd->vdev_trim_inflight++;
	mutex_exit(&vd->vdev_trim_io_lock);

	dmu_tx_t *tx = dmu_tx_create_dd(spa_get_dsl(spa)->dp_mos_dir);
	VERIFY0(dmu_tx_assign(tx, TXG_WAIT));
	uint64_t txg = dmu_tx_get_txg(tx);

	spa_config_enter(spa, SCL_STATE_ALL, vd, RW_READER);
	mutex_enter(&vd->vdev_trim_lock);

	if (vd->vdev_trim_offset[txg & TXG_MASK] == 0) {
		uint64_t *guid = kmem_zalloc(sizeof (uint64_t), KM_SLEEP);
		*guid = vd->vdev_guid;

		/* This is the first write of this txg. */
		dsl_sync_task_nowait(spa_get_dsl(spa),
		    vdev_trim_zap_update_sync, guid, 2,
		    ZFS_SPACE_CHECK_RESERVED, tx);
	}

	/*
	 * We know the vdev struct will still be around since all
	 * consumers of vdev_free must stop the trimming first.
	 */
	if (vdev_trim_should_stop(vd)) {
		mutex_enter(&vd->vdev_trim_io_lock);
		ASSERT3U(vd->vdev_trim_inflight, >, 0);
		vd->vdev_trim_inflight--;
		mutex_exit(&vd->vdev_trim_io_lock);
		spa_config_exit(vd->vdev_spa, SCL_STATE_ALL, vd);
		mutex_exit(&vd->vdev_trim_lock);
		dmu_tx_commit(tx);
		return (SET_ERROR(EINTR));
	}
	mutex_exit(&vd->vdev_trim_lock);

	vd->vdev_trim_offset[txg & TXG_MASK] = start + size;
	zio_nowait(zio_trim(spa->spa_txg_zio[txg & TXG_MASK], vd, start,
	    size, vdev_trim_cb, NULL, ZIO_PRIORITY_MAN_TRIM, 0));

	dmu_tx_commit(tx);

	return (0);
}

static int
vdev_trim_ranges(vdev_t *vd)
{
	avl_tree_t *rt = &vd->vdev_trim_tree->rt_root;
	uint64_t max_bytes = zfs_trim_max_extent_bytes;
	spa_t *spa = vd->vdev_spa;

	for (range_seg_t *rs = avl_first(rt); rs != NULL;
	    rs = AVL_NEXT(rt, rs)) {
		uint64_t size = rs->rs_end - rs->rs_start;

		if (size < zfs_trim_min_extent_bytes) {
			spa_trimstats_update(spa, 0, 0, 1, size);
			continue;
		}

		/* Split range into legally-sized physical chunks */
		uint64_t writes_required = ((size - 1) / max_bytes) + 1;

		for (uint64_t w = 0; w < writes_required; w++) {
			int error;

			error = vdev_trim_range(vd,
			    rs->rs_start + (w * max_bytes),
			    MIN(size - (w * max_bytes), max_bytes));
			if (error != 0)
				return (error);
		}
	}
	return (0);
}

static void
vdev_trim_mg_wait(metaslab_group_t *mg)
{
	ASSERT(MUTEX_HELD(&mg->mg_ms_trim_lock));
	while (mg->mg_trim_updating) {
		cv_wait(&mg->mg_ms_trim_cv, &mg->mg_ms_trim_lock);
	}
}

static void
vdev_trim_mg_mark(metaslab_group_t *mg)
{
	ASSERT(MUTEX_HELD(&mg->mg_ms_trim_lock));
	ASSERT(mg->mg_trim_updating);

	while (mg->mg_ms_trimming >= max_trim_ms) {
		cv_wait(&mg->mg_ms_trim_cv, &mg->mg_ms_trim_lock);
	}
	mg->mg_ms_trimming++;
	ASSERT3U(mg->mg_ms_trimming, <=, max_trim_ms);
}

/*
 * Mark the metaslab as being trimmed to prevent any allocations on
 * this metaslab. We must also track how many metaslabs are currently
 * being trimmed within a metaslab group and limit them to prevent
 * allocation failures from occurring because all metaslabs are being
 * trimmed.
 */
static void
vdev_trim_ms_mark(metaslab_t *msp)
{
	ASSERT(!MUTEX_HELD(&msp->ms_lock));
	metaslab_group_t *mg = msp->ms_group;

	mutex_enter(&mg->mg_ms_trim_lock);

	/*
	 * To keep an accurate count of how many threads are trimming
	 * a specific metaslab group, we only allow one thread to mark
	 * the metaslab group at a time. This ensures that the value of
	 * ms_trimming will be accurate when we decide to mark a metaslab
	 * group as being trimmed. To do this we force all other threads
	 * to wait till the metaslab's mg_trim_updating flag is no
	 * longer set.
	 */
	vdev_trim_mg_wait(mg);
	mg->mg_trim_updating = B_TRUE;
	if (msp->ms_trimming == 0) {
		vdev_trim_mg_mark(mg);
	}
	mutex_enter(&msp->ms_lock);
	msp->ms_trimming++;
	mutex_exit(&msp->ms_lock);

	mg->mg_trim_updating = B_FALSE;
	cv_broadcast(&mg->mg_ms_trim_cv);
	mutex_exit(&mg->mg_ms_trim_lock);
}

static void
vdev_trim_ms_unmark(metaslab_t *msp)
{
	ASSERT(!MUTEX_HELD(&msp->ms_lock));
	metaslab_group_t *mg = msp->ms_group;
	mutex_enter(&mg->mg_ms_trim_lock);
	mutex_enter(&msp->ms_lock);
	if (--msp->ms_trimming == 0) {
		mg->mg_ms_trimming--;
		cv_broadcast(&mg->mg_ms_trim_cv);
	}
	mutex_exit(&msp->ms_lock);
	mutex_exit(&mg->mg_ms_trim_lock);
}

static void
vdev_trim_calculate_progress(vdev_t *vd)
{
	ASSERT(spa_config_held(vd->vdev_spa, SCL_CONFIG, RW_READER) ||
	    spa_config_held(vd->vdev_spa, SCL_CONFIG, RW_WRITER));
	ASSERT(vd->vdev_leaf_zap != 0);

	vd->vdev_trim_bytes_est = 0;
	vd->vdev_trim_bytes_done = 0;

	for (uint64_t i = 0; i < vd->vdev_top->vdev_ms_count; i++) {
		metaslab_t *msp = vd->vdev_top->vdev_ms[i];
		mutex_enter(&msp->ms_lock);

		uint64_t ms_free = msp->ms_size -
		    space_map_allocated(msp->ms_sm);

		if (vd->vdev_top->vdev_ops == &vdev_raidz_ops)
			ms_free /= vd->vdev_top->vdev_children;

		/*
		 * Convert the metaslab range to a physical range
		 * on our vdev. We use this to determine if we are
		 * in the middle of this metaslab range.
		 */
		range_seg_t logical_rs, physical_rs;
		logical_rs.rs_start = msp->ms_start;
		logical_rs.rs_end = msp->ms_start + msp->ms_size;
		vdev_xlate(vd, &logical_rs, &physical_rs);

		if (vd->vdev_trim_last_offset <= physical_rs.rs_start) {
			vd->vdev_trim_bytes_est += ms_free;
			mutex_exit(&msp->ms_lock);
			continue;
		} else if (vd->vdev_trim_last_offset > physical_rs.rs_end) {
			vd->vdev_trim_bytes_done += ms_free;
			vd->vdev_trim_bytes_est += ms_free;
			mutex_exit(&msp->ms_lock);
			continue;
		}

		/*
		 * If we get here, we're in the middle of trimming this
		 * metaslab. Load it and walk the free tree for more accurate
		 * progress estimation.
		 */
		VERIFY0(metaslab_load(msp));

		for (range_seg_t *rs = avl_first(&msp->ms_allocatable->rt_root);
		    rs; rs = AVL_NEXT(&msp->ms_allocatable->rt_root, rs)) {
			logical_rs.rs_start = rs->rs_start;
			logical_rs.rs_end = rs->rs_end;
			vdev_xlate(vd, &logical_rs, &physical_rs);

			uint64_t size = physical_rs.rs_end -
			    physical_rs.rs_start;
			vd->vdev_trim_bytes_est += size;
			if (vd->vdev_trim_last_offset >
			    physical_rs.rs_end) {
				vd->vdev_trim_bytes_done += size;
			} else if (vd->vdev_trim_last_offset >
			    physical_rs.rs_start &&
			    vd->vdev_trim_last_offset <
			    physical_rs.rs_end) {
				vd->vdev_trim_bytes_done +=
				    vd->vdev_trim_last_offset -
				    physical_rs.rs_start;
			}
		}
		mutex_exit(&msp->ms_lock);
	}
}

static int
vdev_trim_load(vdev_t *vd)
{
	int err = 0;
	ASSERT(spa_config_held(vd->vdev_spa, SCL_CONFIG, RW_READER) ||
	    spa_config_held(vd->vdev_spa, SCL_CONFIG, RW_WRITER));
	ASSERT(vd->vdev_leaf_zap != 0);

	if (vd->vdev_trim_state == VDEV_TRIM_ACTIVE ||
	    vd->vdev_trim_state == VDEV_TRIM_SUSPENDED) {
		err = zap_lookup(vd->vdev_spa->spa_meta_objset,
		    vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_LAST_OFFSET,
		    sizeof (vd->vdev_trim_last_offset), 1,
		    &vd->vdev_trim_last_offset);
		if (err == ENOENT) {
			vd->vdev_trim_last_offset = 0;
			err = 0;
		}

		if (err == 0) {
			err = zap_lookup(vd->vdev_spa->spa_meta_objset,
			    vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_RATE,
			    sizeof (vd->vdev_trim_rate), 1,
			    &vd->vdev_trim_rate);
			if (err == ENOENT) {
				vd->vdev_trim_rate = 0;
				err = 0;
			}
		}

		if (err == 0) {
			err = zap_lookup(vd->vdev_spa->spa_meta_objset,
			    vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_FULL,
			    sizeof (vd->vdev_trim_full), 1,
			    &vd->vdev_trim_full);
			if (err == ENOENT) {
				vd->vdev_trim_full = 1;
				err = 0;
			}
		}
	}

	vdev_trim_calculate_progress(vd);
	return (err);
}

/*
 * Convert the logical range into a physical range and add it to our
 * avl tree.
 */
void
vdev_trim_range_add(void *arg, uint64_t start, uint64_t size)
{
	vdev_t *vd = arg;
	range_seg_t logical_rs, physical_rs;
	logical_rs.rs_start = start;
	logical_rs.rs_end = start + size;

	ASSERT(vd->vdev_ops->vdev_op_leaf);
	vdev_xlate(vd, &logical_rs, &physical_rs);

	IMPLY(vd->vdev_top == vd,
	    logical_rs.rs_start == physical_rs.rs_start);
	IMPLY(vd->vdev_top == vd,
	    logical_rs.rs_end == physical_rs.rs_end);

	/* Only add segments that we have not visited yet */
	if (physical_rs.rs_end <= vd->vdev_trim_last_offset)
		return;

	/* Pick up where we left off mid-range. */
	if (vd->vdev_trim_last_offset > physical_rs.rs_start) {
		zfs_dbgmsg("range write: vd %s changed (%llu, %llu) to "
		    "(%llu, %llu)", vd->vdev_path,
		    (u_longlong_t)physical_rs.rs_start,
		    (u_longlong_t)physical_rs.rs_end,
		    (u_longlong_t)vd->vdev_trim_last_offset,
		    (u_longlong_t)physical_rs.rs_end);
		ASSERT3U(physical_rs.rs_end, >,
		    vd->vdev_trim_last_offset);
		physical_rs.rs_start = vd->vdev_trim_last_offset;
	}
	ASSERT3U(physical_rs.rs_end, >=, physical_rs.rs_start);

	/*
	 * With raidz, it's possible that the logical range does not live on
	 * this leaf vdev. We only add the physical range to this vdev's if it
	 * has a length greater than 0.
	 */
	if (physical_rs.rs_end > physical_rs.rs_start) {
		range_tree_add(vd->vdev_trim_tree, physical_rs.rs_start,
		    physical_rs.rs_end - physical_rs.rs_start);
	} else {
		ASSERT3U(physical_rs.rs_end, ==, physical_rs.rs_start);
	}
}

static void
vdev_trim_thread(void *arg)
{
	vdev_t *vd = arg;
	spa_t *spa = vd->vdev_spa;
	int error = 0;
	uint64_t ms_count = 0;

	ASSERT(vdev_is_concrete(vd));
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);

	vd->vdev_trim_last_offset = 0;
	VERIFY0(vdev_trim_load(vd));

	vd->vdev_trim_tree = range_tree_create(NULL, NULL);

	for (uint64_t i = 0; !vd->vdev_detached &&
	    i < vd->vdev_top->vdev_ms_count; i++) {
		metaslab_t *msp = vd->vdev_top->vdev_ms[i];

		/*
		 * If we've expanded the top-level vdev or it's our
		 * first pass, calculate our progress.
		 */
		if (vd->vdev_top->vdev_ms_count != ms_count) {
			vdev_trim_calculate_progress(vd);
			ms_count = vd->vdev_top->vdev_ms_count;
		}

		vdev_trim_ms_mark(msp);
		mutex_enter(&msp->ms_lock);
		VERIFY0(metaslab_load(msp));

		/*
		 * If a partial TRIM was requested skip metaslabs which have
		 * never been initialized and thus have never been written.
		 */
		if (msp->ms_sm == NULL && !vd->vdev_trim_full) {
			mutex_exit(&msp->ms_lock);
			vdev_trim_ms_unmark(msp);
			continue;
		}

		range_tree_walk(msp->ms_allocatable, vdev_trim_range_add, vd);
		mutex_exit(&msp->ms_lock);

		spa_config_exit(spa, SCL_CONFIG, FTAG);
		error = vdev_trim_ranges(vd);
		vdev_trim_ms_unmark(msp);
		spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);

		range_tree_vacate(vd->vdev_trim_tree, NULL, NULL);
		if (error != 0)
			break;

		/*
		 * XXX - Rate limiting delay loop.
		 */
	}

	spa_config_exit(spa, SCL_CONFIG, FTAG);
	mutex_enter(&vd->vdev_trim_io_lock);
	while (vd->vdev_trim_inflight > 0) {
		cv_wait(&vd->vdev_trim_io_cv,
		    &vd->vdev_trim_io_lock);
	}
	mutex_exit(&vd->vdev_trim_io_lock);

	range_tree_destroy(vd->vdev_trim_tree);
	vd->vdev_trim_tree = NULL;

	mutex_enter(&vd->vdev_trim_lock);
	if (!vd->vdev_trim_exit_wanted && vdev_writeable(vd)) {
		vdev_trim_change_state(vd, VDEV_TRIM_COMPLETE,
		    vd->vdev_trim_rate, vd->vdev_trim_full);
	}
	ASSERT(vd->vdev_trim_thread != NULL || vd->vdev_trim_inflight == 0);

	/*
	 * Drop the vdev_trim_lock while we sync out the txg since it's
	 * possible that a device might be trying to come online and must
	 * check to see if it needs to restart an initialization. That
	 * thread will be holding the spa_config_lock which would prevent
	 * the txg_wait_synced from completing.
	 */
	mutex_exit(&vd->vdev_trim_lock);
	txg_wait_synced(spa_get_dsl(spa), 0);
	mutex_enter(&vd->vdev_trim_lock);

	vd->vdev_trim_thread = NULL;
	cv_broadcast(&vd->vdev_trim_cv);
	mutex_exit(&vd->vdev_trim_lock);
}

/*
 * Initiates a device. Caller must hold vdev_trim_lock.
 * Device must be a leaf and not already be trimming.
 */
void
vdev_trim(vdev_t *vd, uint64_t rate, boolean_t fulltrim)
{
	ASSERT(MUTEX_HELD(&vd->vdev_trim_lock));
	ASSERT(vd->vdev_ops->vdev_op_leaf);
	ASSERT(vdev_is_concrete(vd));
	ASSERT3P(vd->vdev_trim_thread, ==, NULL);
	ASSERT(!vd->vdev_detached);
	ASSERT(!vd->vdev_trim_exit_wanted);
	ASSERT(!vd->vdev_top->vdev_removing);

	vdev_trim_change_state(vd, VDEV_TRIM_ACTIVE, rate, fulltrim);
	vd->vdev_trim_thread = thread_create(NULL, 0,
	    vdev_trim_thread, vd, 0, &p0, TS_RUN, maxclsyspri);
}

/*
 * Wait for the trimming thread to be terminated (cancelled or stopped).
 */
static void
vdev_trim_stop_wait_impl(vdev_t *vd)
{
	ASSERT(MUTEX_HELD(&vd->vdev_trim_lock));

	while (vd->vdev_trim_thread != NULL)
		cv_wait(&vd->vdev_trim_cv, &vd->vdev_trim_lock);

	ASSERT3P(vd->vdev_trim_thread, ==, NULL);
	vd->vdev_trim_exit_wanted = B_FALSE;
}

/*
 * Wait for vdev trim threads which were either to cleanly exit.
 */
void
vdev_trim_stop_wait(spa_t *spa, list_t *vd_list)
{
	vdev_t *vd;

	ASSERT(MUTEX_HELD(&spa_namespace_lock));

	while ((vd = list_remove_head(vd_list)) != NULL) {
		mutex_enter(&vd->vdev_trim_lock);
		vdev_trim_stop_wait_impl(vd);
		mutex_exit(&vd->vdev_trim_lock);
	}
}

/*
 * Stop trimming a device, with the resultant trimming state being tgt_state.
 * For blocking behavior pass NULL for vd_list.  Otherwise, when a list_t is
 * provided the stopping vdev is inserted in to the list.  Callers are then
 * required to call vdev_trim_stop_wait() to block for all the trim threads
 * to exit.  The caller must hold vdev_trim_lock and must not be writing to
 * the spa config, as the trimming thread may try to enter the config as a
 * reader before exiting.
 */
void
vdev_trim_stop(vdev_t *vd, vdev_trim_state_t tgt_state, list_t *vd_list)
{
	ASSERT(!spa_config_held(vd->vdev_spa, SCL_CONFIG|SCL_STATE, RW_WRITER));
	ASSERT(MUTEX_HELD(&vd->vdev_trim_lock));
	ASSERT(vd->vdev_ops->vdev_op_leaf);
	ASSERT(vdev_is_concrete(vd));

	/*
	 * Allow cancel requests to proceed even if the trim thread has
	 * stopped.
	 */
	if (vd->vdev_trim_thread == NULL && tgt_state != VDEV_TRIM_CANCELED)
		return;

	vdev_trim_change_state(vd, tgt_state, 0, 0);
	vd->vdev_trim_exit_wanted = B_TRUE;

	if (vd_list == NULL) {
		vdev_trim_stop_wait_impl(vd);
	} else {
		ASSERT(MUTEX_HELD(&spa_namespace_lock));
		list_insert_tail(vd_list, vd);
	}
}

static void
vdev_trim_stop_all_impl(vdev_t *vd, vdev_trim_state_t tgt_state,
    list_t *vd_list)
{
	if (vd->vdev_ops->vdev_op_leaf && vdev_is_concrete(vd)) {
		mutex_enter(&vd->vdev_trim_lock);
		vdev_trim_stop(vd, tgt_state, vd_list);
		mutex_exit(&vd->vdev_trim_lock);
		return;
	}

	for (uint64_t i = 0; i < vd->vdev_children; i++) {
		vdev_trim_stop_all_impl(vd->vdev_child[i], tgt_state,
		    vd_list);
	}
}

/*
 * Convenience function to stop trimming of a vdev tree and set all trim
 * thread pointers to NULL.
 */
void
vdev_trim_stop_all(vdev_t *vd, vdev_trim_state_t tgt_state)
{
	spa_t *spa = vd->vdev_spa;
	list_t vd_list;

	ASSERT(MUTEX_HELD(&spa_namespace_lock));

	list_create(&vd_list, sizeof (vdev_t),
	    offsetof(vdev_t, vdev_trim_node));

	vdev_trim_stop_all_impl(vd, tgt_state, &vd_list);
	vdev_trim_stop_wait(spa, &vd_list);

	if (vd->vdev_spa->spa_sync_on) {
		/* Make sure that our state has been synced to disk */
		txg_wait_synced(spa_get_dsl(vd->vdev_spa), 0);
	}

	list_destroy(&vd_list);
}

void
vdev_trim_restart(vdev_t *vd)
{
	ASSERT(MUTEX_HELD(&spa_namespace_lock));
	ASSERT(!spa_config_held(vd->vdev_spa, SCL_ALL, RW_WRITER));

	if (vd->vdev_leaf_zap != 0) {
		mutex_enter(&vd->vdev_trim_lock);
		uint64_t trim_state = VDEV_TRIM_NONE;
		int err = zap_lookup(vd->vdev_spa->spa_meta_objset,
		    vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_STATE,
		    sizeof (trim_state), 1, &trim_state);
		ASSERT(err == 0 || err == ENOENT);
		vd->vdev_trim_state = trim_state;

		uint64_t timestamp = 0;
		err = zap_lookup(vd->vdev_spa->spa_meta_objset,
		    vd->vdev_leaf_zap, VDEV_LEAF_ZAP_TRIM_ACTION_TIME,
		    sizeof (timestamp), 1, &timestamp);
		ASSERT(err == 0 || err == ENOENT);
		vd->vdev_trim_action_time = (time_t)timestamp;

		if (vd->vdev_trim_state == VDEV_TRIM_SUSPENDED ||
		    vd->vdev_offline) {
			/* load progress for reporting, but don't resume */
			VERIFY0(vdev_trim_load(vd));
		} else if (vd->vdev_trim_state == VDEV_TRIM_ACTIVE &&
		    vdev_writeable(vd)) {
			VERIFY0(vdev_trim_load(vd));
			vdev_trim(vd, vd->vdev_trim_rate, vd->vdev_trim_full);
		}

		mutex_exit(&vd->vdev_trim_lock);
	}

	for (uint64_t i = 0; i < vd->vdev_children; i++) {
		vdev_trim_restart(vd->vdev_child[i]);
	}
}

/*
 * Runs through all metaslabs on the vdev and does their autotrim processing.
 */
void
vdev_auto_trim(vdev_trim_info_t *vti)
{
	vdev_t *vd = vti->vti_vdev;
	spa_t *spa = vd->vdev_spa;
	uint64_t txg = vti->vti_txg;
	uint64_t txgs_per_trim = MAX(zfs_txgs_per_trim, 1);
	uint64_t mlim = 0, mused = 0;
	uint64_t ms_count = vd->vdev_ms_count;
	boolean_t preserve_spilled;

	ASSERT3P(vd->vdev_top, ==, vd);
	ASSERT3P(vti->vti_done_cb, !=, NULL);

	mutex_enter(&vd->vdev_trim_lock);
	if (vd->vdev_trim_thread != NULL) {
		mutex_exit(&vd->vdev_trim_lock);
		vti->vti_done_cb(vti->vti_done_arg);
		return;
	}
	mutex_exit(&vd->vdev_trim_lock);

	/*
	 * In case trimming is slow and the previous trim run has no yet
	 * finished, we order metaslab_auto_trim to keep the extents that
	 * were about to be trimmed so that they can be trimmed in a future
	 * autotrim run. But we only do so if the amount of memory consumed
	 * by the extents doesn't exceed a threshold, otherwise we drop them.
	 */
	for (uint64_t i = 0; i < ms_count; i++)
		mused += metaslab_trim_mem_used(vd->vdev_ms[i]);

	mlim = (physmem * PAGESIZE) / (zfs_trim_mem_lim_fact *
	    spa->spa_root_vdev->vdev_children);
	preserve_spilled = mused < mlim;
	DTRACE_PROBE3(autotrim__mem__lim, vdev_t *, vd, uint64_t, mused,
	    uint64_t, mlim);

	/*
	 * Since we typically have hundreds of metaslabs per vdev, but we only
	 * trim them once every zfs_txgs_per_trim txgs, it'd be best if we
	 * could sequence the TRIM commands from all metaslabs so that they
	 * don't all always pound the device in the same txg. We do so taking
	 * the txg number modulo txgs_per_trim and then skipping by
	 * txgs_per_trim. Thus, for the default 200 metaslabs and 32
	 * txgs_per_trim, we'll only be trimming ~6.25 metaslabs per txg.
	 */
	for (uint64_t i = txg % txgs_per_trim; i < ms_count; i += txgs_per_trim)
		metaslab_auto_trim(vd->vdev_ms[i], preserve_spilled);

	vti->vti_done_cb(vti->vti_done_arg);
}

/*
 * Determines the minimum sensible rate at which a manual TRIM can be
 * performed on a given spa and returns it (in bytes per second). The
 * value is calculated by assuming that TRIMming a metaslab should take
 * no more than 1000s. The exact value here is not important, we just want
 * to make sure that the calculated delay values in vdev_trim() aren't
 * too large (which might cause integer precision issues). Thus, on a
 * typical 200-metaslab vdev, the longest TRIM should take is about 55
 * hours. It *can* take longer if the device is really slow respond to
 * zio_trim() commands or it contains more than 200 metaslabs, or metaslab
 * sizes vary widely between top-level vdevs.
 */
uint64_t
vdev_trim_min_rate(spa_t *spa)
{
	uint64_t i, smallest_ms_sz = UINT64_MAX;

	/* find the smallest metaslab */
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
	for (i = 0; i < spa->spa_root_vdev->vdev_children; i++) {
		vdev_t *cvd = spa->spa_root_vdev->vdev_child[i];
		if (!vdev_is_concrete(cvd) || cvd->vdev_ms == NULL ||
		    cvd->vdev_ms[0] == NULL)
			continue;
		smallest_ms_sz = MIN(smallest_ms_sz, cvd->vdev_ms[0]->ms_size);
	}
	spa_config_exit(spa, SCL_CONFIG, FTAG);
	VERIFY(smallest_ms_sz != 0);

	/* minimum TRIM rate is 1/1000th of the smallest metaslab size */
	return (smallest_ms_sz / 1000);
}

/*
 * Update the aggregate statistics for a TRIM zio.
 */
void
vdev_trim_stat_update(zio_t *zio, uint64_t psize, vdev_trim_stat_flags_t flags)
{
	vdev_stat_trim_t *vsd = zio->io_dfl_stats;
	hrtime_t now = gethrtime();
	hrtime_t io_delta = io_delta = now - zio->io_timestamp;
	hrtime_t io_delay = now - zio->io_delay;

	if (flags & TRIM_STAT_OP) {
		vsd->vsd_ops++;
		vsd->vsd_bytes += psize;
	}

	if (flags & TRIM_STAT_RQ_HISTO) {
		vsd->vsd_ind_histo[RQ_HISTO(psize)]++;
	}

	if (flags & TRIM_STAT_L_HISTO) {
		vsd->vsd_queue_histo[L_HISTO(io_delta - io_delay)]++;
		vsd->vsd_disk_histo[L_HISTO(io_delay)]++;
		vsd->vsd_total_histo[L_HISTO(io_delta)]++;
	}
}

#if defined(_KERNEL)
EXPORT_SYMBOL(vdev_trim);
EXPORT_SYMBOL(vdev_trim_stop);
EXPORT_SYMBOL(vdev_trim_stop_all);
EXPORT_SYMBOL(vdev_trim_stop_wait);
EXPORT_SYMBOL(vdev_trim_restart);

/* XXX- Decide which module options to make available */
module_param(zfs_trim_enabled, int, 0644);
MODULE_PARM_DESC(zfs_trim, "Enable TRIM");

module_param(zfs_trim_sync, int, 0644);
MODULE_PARM_DESC(zfs_trim_sync, "Issue TRIM commands synchronously");
#endif
