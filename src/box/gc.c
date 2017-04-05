/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "gc.h"

#include <stdint.h>
#include <stdlib.h>

#include <small/region.h>

#include "diag.h"
#include "errcode.h"
#include "fiber.h"
#include "vclock.h"
#include "xlog.h"

#include "engine.h"		/* engine_collect_garbage() */
#include "replication.h"	/* INSTANCE_UUID */
#include "wal.h"		/* wal_collect_garbage() */

/** Garbage collection state. */
struct gc_state {
	/** Max LSN garbage collection has been called for. */
	int64_t lsn;
	/** Uncollected checkpoints, see checkpoint_info. */
	vclockset_t checkpoints;
	/** Number of tracked checkpoints. */
	int n_checkpoints;
};
static struct gc_state gc;

int
gc_init(const char *snap_dirname)
{
	gc.lsn = -1;
	vclockset_new(&gc.checkpoints);

	struct xdir dir;
	xdir_create(&dir, snap_dirname, SNAP, &INSTANCE_UUID);
	if (xdir_scan(&dir) < 0)
		goto fail;

	for (struct vclock *vclock = vclockset_first(&dir.index);
	     vclock != NULL; vclock = vclockset_next(&dir.index, vclock)) {
		if (gc_add_checkpoint(vclock) < 0)
			goto fail;
	}
	xdir_destroy(&dir);
	return 0;
fail:
	xdir_destroy(&dir);
	gc_free();
	return -1;
}

void
gc_free(void)
{
	struct vclock *vclock = vclockset_first(&gc.checkpoints);
	while (vclock != NULL) {
		struct vclock *next = vclockset_next(&gc.checkpoints, vclock);
		vclockset_remove(&gc.checkpoints, vclock);
		struct checkpoint_info *cpt = container_of(vclock,
				struct checkpoint_info, vclock);
		free(cpt);
		vclock = next;
	}
}

int
gc_add_checkpoint(const struct vclock *vclock)
{
	struct checkpoint_info *cpt;
	cpt = (struct checkpoint_info *)malloc(sizeof(*cpt));
	if (cpt == NULL) {
		diag_set(OutOfMemory, sizeof(*cpt),
			 "malloc", "struct checkpoint_info");
		return -1;
	}

	struct vclock *prev = vclockset_last(&gc.checkpoints);
	/*
	 * Do not allow to remove the last checkpoint,
	 * because we need it for recovery.
	 */
	cpt->pinned = 1;
	vclock_copy(&cpt->vclock, vclock);
	vclockset_insert(&gc.checkpoints, &cpt->vclock);
	gc.n_checkpoints++;

	if (prev != NULL) {
		assert(vclock_compare(vclock, prev) > 0);
		gc_unpin_checkpoint(prev);
	}
	return 0;
}

int64_t
gc_last_checkpoint(struct vclock *vclock)
{
	struct vclock *last = vclockset_last(&gc.checkpoints);
	if (last == NULL)
		return -1;
	vclock_copy(vclock, last);
	return vclock_sum(last);
}

int64_t
gc_pin_last_checkpoint(struct vclock *vclock)
{
	struct vclock *last = vclockset_last(&gc.checkpoints);
	if (last == NULL)
		return -1;
	struct checkpoint_info *cpt = container_of(last,
			struct checkpoint_info, vclock);
	/* The last checkpoint is always pinned. */
	assert(cpt->pinned > 0);
	cpt->pinned++;
	vclock_copy(vclock, last);
	return vclock_sum(last);
}

void
gc_unpin_checkpoint(struct vclock *vclock)
{
	struct vclock *cpt_vclock = vclockset_search(&gc.checkpoints, vclock);
	assert(cpt_vclock != NULL);
	struct checkpoint_info *cpt = container_of(cpt_vclock,
			struct checkpoint_info, vclock);
	assert(cpt->pinned > 0);
	cpt->pinned--;
	/* Retry gc when a checkpoint is unpinned. */
	if (cpt->pinned == 0)
		gc_run(gc.lsn);

}

void
gc_run(int64_t lsn)
{
	if (gc.lsn < lsn)
		gc.lsn = lsn;

	int64_t gc_lsn = -1;

	struct vclock *vclock = vclockset_first(&gc.checkpoints);
	while (vclock != NULL) {
		if (vclock_sum(vclock) >= lsn)
			break; /* all eligible checkpoints removed */

		struct checkpoint_info *cpt = container_of(vclock,
				struct checkpoint_info, vclock);
		if (cpt->pinned > 0)
			break; /* checkpoint still in use */

		struct vclock *next = vclockset_next(&gc.checkpoints, vclock);
		vclockset_remove(&gc.checkpoints, vclock);
		gc.n_checkpoints--;
		free(cpt);
		vclock = next;

		/* Include this checkpoint to gc. */
		gc_lsn = (vclock != NULL ? vclock_sum(vclock) : lsn);
	}

	if (gc_lsn >= 0) {
		wal_collect_garbage(gc_lsn);
		engine_collect_garbage(gc_lsn);
	}
}

int64_t
gc_lsn(void)
{
	return gc.lsn;
}

int
gc_list_checkpoints(struct checkpoint_info **p_checkpoints)
{
	if (gc.n_checkpoints == 0) {
		*p_checkpoints = NULL;
		return 0;
	}

	size_t size = sizeof(struct checkpoint_info) * gc.n_checkpoints;
	struct checkpoint_info *checkpoints = region_alloc(&fiber()->gc, size);
	if (checkpoints == NULL) {
		diag_set(OutOfMemory, size, "region", "checkpoint_info");
		return -1;
	}

	int n = 0;
	for (struct vclock *vclock = vclockset_first(&gc.checkpoints);
	     vclock != NULL; vclock = vclockset_next(&gc.checkpoints, vclock)) {
		assert(n < gc.n_checkpoints);
		checkpoints[n++] = *container_of(vclock,
				struct checkpoint_info, vclock);
	}

	assert(n == gc.n_checkpoints);
	*p_checkpoints = checkpoints;
	return gc.n_checkpoints;
}
