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

#ifndef _SYS_VDEV_TRIM_H
#define	_SYS_VDEV_TRIM_H

#include <sys/spa.h>

#ifdef	__cplusplus
extern "C" {
#endif

typedef struct vdev_trim_info {
	vdev_t *vti_vdev;
	uint64_t vti_txg;	/* ignored for manual trim */
	void (*vti_done_cb)(void *);
	void *vti_done_arg;
} vdev_trim_info_t;

typedef enum vdev_trim_stat_flags
{
	TRIM_STAT_OP		= 1 << 0,
	TRIM_STAT_RQ_HISTO	= 1 << 1,
	TRIM_STAT_L_HISTO	= 1 << 2,
} vdev_trim_stat_flags_t;

#define	TRIM_STAT_ALL	(TRIM_STAT_OP | TRIM_STAT_RQ_HISTO | TRIM_STAT_L_HISTO)

#define	ZIO_IS_TRIM(zio) \
	((zio)->io_type == ZIO_TYPE_IOCTL && (zio)->io_cmd == DKIOCFREE)

extern void vdev_trim(vdev_t *vd, uint64_t rate, boolean_t fulltrim);
extern void vdev_trim_stop(vdev_t *vd, vdev_trim_state_t tgt, list_t *vd_list);
extern void vdev_trim_stop_all(vdev_t *vd, vdev_trim_state_t tgt_state);
extern void vdev_trim_stop_wait(spa_t *spa, list_t *vd_list);
extern void vdev_trim_restart(vdev_t *vd);
extern void vdev_auto_trim(vdev_trim_info_t *vti);
extern uint64_t vdev_trim_min_rate(spa_t *spa);
extern void vdev_trim_stat_update(zio_t *zio, uint64_t psize,
    vdev_trim_stat_flags_t flags);

extern int zfs_trim_limit;
extern int zfs_trim_enabled;
extern uint64_t zfs_trim_min_extent_bytes;
extern uint64_t zfs_trim_max_extent_bytes;
extern int zfs_trim_sync;

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_VDEV_TRIM_H */
