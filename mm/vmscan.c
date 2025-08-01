// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, Stephen Tweedie.
 *  kswapd added: 7.1.96  sct
 *  Removed kswapd_ctl limits, and swap out as many pages as needed
 *  to bring the system back to freepages.high: 2.4.97, Rik van Riel.
 *  Zone aware kswapd started 02/00, Kanoj Sarcar (kanoj@sgi.com).
 *  Multiqueue VM started 5.8.00, Rik van Riel.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/vmpressure.h>
#include <linux/vmstat.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* for try_to_release_page(),
					buffer_heads_over_limit */
#include <linux/mm_inline.h>
#include <linux/backing-dev.h>
#include <linux/rmap.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/compaction.h>
#include <linux/notifier.h>
#include <linux/rwsem.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/memcontrol.h>
#include <linux/migrate.h>
#include <linux/delayacct.h>
#include <linux/sysctl.h>
#include <linux/oom.h>
#include <linux/pagevec.h>
#include <linux/prefetch.h>
#include <linux/printk.h>
#include <linux/dax.h>
#include <linux/psi.h>
#include <linux/pagewalk.h>
#include <linux/shmem_fs.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>

#include <asm/tlbflush.h>
#include <asm/div64.h>

#include <linux/swapops.h>
#include <linux/balloon_compaction.h>

#include "internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/vmscan.h>

EXPORT_TRACEPOINT_SYMBOL_GPL(mm_vmscan_direct_reclaim_begin);
EXPORT_TRACEPOINT_SYMBOL_GPL(mm_vmscan_direct_reclaim_end);
EXPORT_TRACEPOINT_SYMBOL_GPL(mm_vmscan_kswapd_wake);

#undef CREATE_TRACE_POINTS
#include <trace/hooks/vmscan.h>

struct scan_control {
	/* How many pages shrink_list() should reclaim */
	unsigned long nr_to_reclaim;

	/*
	 * Nodemask of nodes allowed by the caller. If NULL, all nodes
	 * are scanned.
	 */
	nodemask_t	*nodemask;

	/*
	 * The memory cgroup that hit its limit and as a result is the
	 * primary target of this reclaim invocation.
	 */
	struct mem_cgroup *target_mem_cgroup;

	/*
	 * Scan pressure balancing between anon and file LRUs
	 */
	unsigned long	anon_cost;
	unsigned long	file_cost;

	/* Can active pages be deactivated as part of reclaim? */
#define DEACTIVATE_ANON 1
#define DEACTIVATE_FILE 2
	unsigned int may_deactivate:2;
	unsigned int force_deactivate:1;
	unsigned int skipped_deactivate:1;

	/* Writepage batching in laptop mode; RECLAIM_WRITE */
	unsigned int may_writepage:1;

	/* Can mapped pages be reclaimed? */
	unsigned int may_unmap:1;

	/* Can pages be swapped as part of reclaim? */
	unsigned int may_swap:1;

	/*
	 * Cgroup memory below memory.low is protected as long as we
	 * don't threaten to OOM. If any cgroup is reclaimed at
	 * reduced force or passed over entirely due to its memory.low
	 * setting (memcg_low_skipped), and nothing is reclaimed as a
	 * result, then go back for one more cycle that reclaims the protected
	 * memory (memcg_low_reclaim) to avert OOM.
	 */
	unsigned int memcg_low_reclaim:1;
	unsigned int memcg_low_skipped:1;

	unsigned int hibernation_mode:1;

	/* One of the zones is ready for compaction */
	unsigned int compaction_ready:1;

	/* There is easily reclaimable cold cache in the current node */
	unsigned int cache_trim_mode:1;

	/* The file pages on the current node are dangerously low */
	unsigned int file_is_tiny:1;

	/* Always discard instead of demoting to lower tier memory */
	unsigned int no_demotion:1;

#ifdef CONFIG_LRU_GEN
	/* help kswapd make better choices among multiple memcgs */
	unsigned int memcgs_need_aging:1;
	unsigned long last_reclaimed;
#endif

	/* Allocation order */
	s8 order;

	/* Scan (total_size >> priority) pages at once */
	s8 priority;

	/* The highest zone to isolate pages for reclaim from */
	s8 reclaim_idx;

	/* This context's GFP mask */
	gfp_t gfp_mask;

	/* Incremented by the number of inactive pages that were scanned */
	unsigned long nr_scanned;

	/* Number of pages freed so far during a call to shrink_zones() */
	unsigned long nr_reclaimed;

	struct {
		unsigned int dirty;
		unsigned int unqueued_dirty;
		unsigned int congested;
		unsigned int writeback;
		unsigned int immediate;
		unsigned int file_taken;
		unsigned int taken;
	} nr;

	/* for recording the reclaimed slab by now */
	struct reclaim_state reclaim_state;
};

#ifdef ARCH_HAS_PREFETCHW
#define prefetchw_prev_lru_page(_page, _base, _field)			\
	do {								\
		if ((_page)->lru.prev != _base) {			\
			struct page *prev;				\
									\
			prev = lru_to_page(&(_page->lru));		\
			prefetchw(&prev->_field);			\
		}							\
	} while (0)
#else
#define prefetchw_prev_lru_page(_page, _base, _field) do { } while (0)
#endif

/*
 * From 0 .. 200.  Higher means more swappy.
 */
int vm_swappiness = 60;

static void set_task_reclaim_state(struct task_struct *task,
				   struct reclaim_state *rs)
{
	/* Check for an overwrite */
	WARN_ON_ONCE(rs && task->reclaim_state);

	/* Check for the nulling of an already-nulled member */
	WARN_ON_ONCE(!rs && !task->reclaim_state);

	task->reclaim_state = rs;
}

static LIST_HEAD(shrinker_list);
static DECLARE_RWSEM(shrinker_rwsem);

#ifdef CONFIG_MEMCG
static int shrinker_nr_max;

/* The shrinker_info is expanded in a batch of BITS_PER_LONG */
static inline int shrinker_map_size(int nr_items)
{
	return (DIV_ROUND_UP(nr_items, BITS_PER_LONG) * sizeof(unsigned long));
}

static inline int shrinker_defer_size(int nr_items)
{
	return (round_up(nr_items, BITS_PER_LONG) * sizeof(atomic_long_t));
}

static struct shrinker_info *shrinker_info_protected(struct mem_cgroup *memcg,
						     int nid)
{
	return rcu_dereference_protected(memcg->nodeinfo[nid]->shrinker_info,
					 lockdep_is_held(&shrinker_rwsem));
}

static int expand_one_shrinker_info(struct mem_cgroup *memcg,
				    int map_size, int defer_size,
				    int old_map_size, int old_defer_size)
{
	struct shrinker_info *new, *old;
	struct mem_cgroup_per_node *pn;
	int nid;
	int size = map_size + defer_size;

	for_each_node(nid) {
		pn = memcg->nodeinfo[nid];
		old = shrinker_info_protected(memcg, nid);
		/* Not yet online memcg */
		if (!old)
			return 0;

		new = kvmalloc_node(sizeof(*new) + size, GFP_KERNEL, nid);
		if (!new)
			return -ENOMEM;

		new->nr_deferred = (atomic_long_t *)(new + 1);
		new->map = (void *)new->nr_deferred + defer_size;

		/* map: set all old bits, clear all new bits */
		memset(new->map, (int)0xff, old_map_size);
		memset((void *)new->map + old_map_size, 0, map_size - old_map_size);
		/* nr_deferred: copy old values, clear all new values */
		memcpy(new->nr_deferred, old->nr_deferred, old_defer_size);
		memset((void *)new->nr_deferred + old_defer_size, 0,
		       defer_size - old_defer_size);

		rcu_assign_pointer(pn->shrinker_info, new);
		kvfree_rcu(old, rcu);
	}

	return 0;
}

void free_shrinker_info(struct mem_cgroup *memcg)
{
	struct mem_cgroup_per_node *pn;
	struct shrinker_info *info;
	int nid;

	for_each_node(nid) {
		pn = memcg->nodeinfo[nid];
		info = rcu_dereference_protected(pn->shrinker_info, true);
		kvfree(info);
		rcu_assign_pointer(pn->shrinker_info, NULL);
	}
}

int alloc_shrinker_info(struct mem_cgroup *memcg)
{
	struct shrinker_info *info;
	int nid, size, ret = 0;
	int map_size, defer_size = 0;

	down_write(&shrinker_rwsem);
	map_size = shrinker_map_size(shrinker_nr_max);
	defer_size = shrinker_defer_size(shrinker_nr_max);
	size = map_size + defer_size;
	for_each_node(nid) {
		info = kvzalloc_node(sizeof(*info) + size, GFP_KERNEL, nid);
		if (!info) {
			free_shrinker_info(memcg);
			ret = -ENOMEM;
			break;
		}
		info->nr_deferred = (atomic_long_t *)(info + 1);
		info->map = (void *)info->nr_deferred + defer_size;
		rcu_assign_pointer(memcg->nodeinfo[nid]->shrinker_info, info);
	}
	up_write(&shrinker_rwsem);

	return ret;
}

static inline bool need_expand(int nr_max)
{
	return round_up(nr_max, BITS_PER_LONG) >
	       round_up(shrinker_nr_max, BITS_PER_LONG);
}

static int expand_shrinker_info(int new_id)
{
	int ret = 0;
	int new_nr_max = new_id + 1;
	int map_size, defer_size = 0;
	int old_map_size, old_defer_size = 0;
	struct mem_cgroup *memcg;

	if (!need_expand(new_nr_max))
		goto out;

	if (!root_mem_cgroup)
		goto out;

	lockdep_assert_held(&shrinker_rwsem);

	map_size = shrinker_map_size(new_nr_max);
	defer_size = shrinker_defer_size(new_nr_max);
	old_map_size = shrinker_map_size(shrinker_nr_max);
	old_defer_size = shrinker_defer_size(shrinker_nr_max);

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		ret = expand_one_shrinker_info(memcg, map_size, defer_size,
					       old_map_size, old_defer_size);
		if (ret) {
			mem_cgroup_iter_break(NULL, memcg);
			goto out;
		}
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)) != NULL);
out:
	if (!ret)
		shrinker_nr_max = new_nr_max;

	return ret;
}

void set_shrinker_bit(struct mem_cgroup *memcg, int nid, int shrinker_id)
{
	if (shrinker_id >= 0 && memcg && !mem_cgroup_is_root(memcg)) {
		struct shrinker_info *info;

		rcu_read_lock();
		info = rcu_dereference(memcg->nodeinfo[nid]->shrinker_info);
		/* Pairs with smp mb in shrink_slab() */
		smp_mb__before_atomic();
		set_bit(shrinker_id, info->map);
		rcu_read_unlock();
	}
}

static DEFINE_IDR(shrinker_idr);

static int prealloc_memcg_shrinker(struct shrinker *shrinker)
{
	int id, ret = -ENOMEM;

	if (mem_cgroup_disabled())
		return -ENOSYS;

	down_write(&shrinker_rwsem);
	/* This may call shrinker, so it must use down_read_trylock() */
	id = idr_alloc(&shrinker_idr, shrinker, 0, 0, GFP_KERNEL);
	if (id < 0)
		goto unlock;

	if (id >= shrinker_nr_max) {
		if (expand_shrinker_info(id)) {
			idr_remove(&shrinker_idr, id);
			goto unlock;
		}
	}
	shrinker->id = id;
	ret = 0;
unlock:
	up_write(&shrinker_rwsem);
	return ret;
}

static void unregister_memcg_shrinker(struct shrinker *shrinker)
{
	int id = shrinker->id;

	BUG_ON(id < 0);

	lockdep_assert_held(&shrinker_rwsem);

	idr_remove(&shrinker_idr, id);
}

static long xchg_nr_deferred_memcg(int nid, struct shrinker *shrinker,
				   struct mem_cgroup *memcg)
{
	struct shrinker_info *info;

	info = shrinker_info_protected(memcg, nid);
	return atomic_long_xchg(&info->nr_deferred[shrinker->id], 0);
}

static long add_nr_deferred_memcg(long nr, int nid, struct shrinker *shrinker,
				  struct mem_cgroup *memcg)
{
	struct shrinker_info *info;

	info = shrinker_info_protected(memcg, nid);
	return atomic_long_add_return(nr, &info->nr_deferred[shrinker->id]);
}

void reparent_shrinker_deferred(struct mem_cgroup *memcg)
{
	int i, nid;
	long nr;
	struct mem_cgroup *parent;
	struct shrinker_info *child_info, *parent_info;

	parent = parent_mem_cgroup(memcg);
	if (!parent)
		parent = root_mem_cgroup;

	/* Prevent from concurrent shrinker_info expand */
	down_read(&shrinker_rwsem);
	for_each_node(nid) {
		child_info = shrinker_info_protected(memcg, nid);
		parent_info = shrinker_info_protected(parent, nid);
		for (i = 0; i < shrinker_nr_max; i++) {
			nr = atomic_long_read(&child_info->nr_deferred[i]);
			atomic_long_add(nr, &parent_info->nr_deferred[i]);
		}
	}
	up_read(&shrinker_rwsem);
}

static bool cgroup_reclaim(struct scan_control *sc)
{
	return sc->target_mem_cgroup;
}

/**
 * writeback_throttling_sane - is the usual dirty throttling mechanism available?
 * @sc: scan_control in question
 *
 * The normal page dirty throttling mechanism in balance_dirty_pages() is
 * completely broken with the legacy memcg and direct stalling in
 * shrink_page_list() is used for throttling instead, which lacks all the
 * niceties such as fairness, adaptive pausing, bandwidth proportional
 * allocation and configurability.
 *
 * This function tests whether the vmscan currently in progress can assume
 * that the normal dirty throttling mechanism is operational.
 */
static bool writeback_throttling_sane(struct scan_control *sc)
{
	if (!cgroup_reclaim(sc))
		return true;
#ifdef CONFIG_CGROUP_WRITEBACK
	if (cgroup_subsys_on_dfl(memory_cgrp_subsys))
		return true;
#endif
	return false;
}
#else
static int prealloc_memcg_shrinker(struct shrinker *shrinker)
{
	return -ENOSYS;
}

static void unregister_memcg_shrinker(struct shrinker *shrinker)
{
}

static long xchg_nr_deferred_memcg(int nid, struct shrinker *shrinker,
				   struct mem_cgroup *memcg)
{
	return 0;
}

static long add_nr_deferred_memcg(long nr, int nid, struct shrinker *shrinker,
				  struct mem_cgroup *memcg)
{
	return 0;
}

static bool cgroup_reclaim(struct scan_control *sc)
{
	return false;
}

static bool writeback_throttling_sane(struct scan_control *sc)
{
	return true;
}
#endif

static long xchg_nr_deferred(struct shrinker *shrinker,
			     struct shrink_control *sc)
{
	int nid = sc->nid;

	if (!(shrinker->flags & SHRINKER_NUMA_AWARE))
		nid = 0;

	if (sc->memcg &&
	    (shrinker->flags & SHRINKER_MEMCG_AWARE))
		return xchg_nr_deferred_memcg(nid, shrinker,
					      sc->memcg);

	return atomic_long_xchg(&shrinker->nr_deferred[nid], 0);
}


static long add_nr_deferred(long nr, struct shrinker *shrinker,
			    struct shrink_control *sc)
{
	int nid = sc->nid;

	if (!(shrinker->flags & SHRINKER_NUMA_AWARE))
		nid = 0;

	if (sc->memcg &&
	    (shrinker->flags & SHRINKER_MEMCG_AWARE))
		return add_nr_deferred_memcg(nr, nid, shrinker,
					     sc->memcg);

	return atomic_long_add_return(nr, &shrinker->nr_deferred[nid]);
}

static bool can_demote(int nid, struct scan_control *sc)
{
	if (!numa_demotion_enabled)
		return false;
	if (sc) {
		if (sc->no_demotion)
			return false;
		/* It is pointless to do demotion in memcg reclaim */
		if (cgroup_reclaim(sc))
			return false;
	}
	if (next_demotion_node(nid) == NUMA_NO_NODE)
		return false;

	return true;
}

static inline bool can_reclaim_anon_pages(struct mem_cgroup *memcg,
					  int nid,
					  struct scan_control *sc)
{
	if (memcg == NULL) {
		/*
		 * For non-memcg reclaim, is there
		 * space in any swap device?
		 */
		if (get_nr_swap_pages() > 0)
			return true;
	} else {
		/* Is the memcg below its swap limit? */
		if (mem_cgroup_get_nr_swap_pages(memcg) > 0)
			return true;
	}

	/*
	 * The page can not be swapped.
	 *
	 * Can it be reclaimed from this node via demotion?
	 */
	return can_demote(nid, sc);
}

/*
 * This misses isolated pages which are not accounted for to save counters.
 * As the data only determines if reclaim or compaction continues, it is
 * not expected that isolated pages will be a dominating factor.
 */
unsigned long zone_reclaimable_pages(struct zone *zone)
{
	unsigned long nr;

	nr = zone_page_state_snapshot(zone, NR_ZONE_INACTIVE_FILE) +
		zone_page_state_snapshot(zone, NR_ZONE_ACTIVE_FILE);
	if (can_reclaim_anon_pages(NULL, zone_to_nid(zone), NULL))
		nr += zone_page_state_snapshot(zone, NR_ZONE_INACTIVE_ANON) +
			zone_page_state_snapshot(zone, NR_ZONE_ACTIVE_ANON);

	return nr;
}

/**
 * lruvec_lru_size -  Returns the number of pages on the given LRU list.
 * @lruvec: lru vector
 * @lru: lru to use
 * @zone_idx: zones to consider (use MAX_NR_ZONES for the whole LRU list)
 */
static unsigned long lruvec_lru_size(struct lruvec *lruvec, enum lru_list lru,
				     int zone_idx)
{
	unsigned long size = 0;
	int zid;

	for (zid = 0; zid <= zone_idx && zid < MAX_NR_ZONES; zid++) {
		struct zone *zone = &lruvec_pgdat(lruvec)->node_zones[zid];

		if (!managed_zone(zone))
			continue;

		if (!mem_cgroup_disabled())
			size += mem_cgroup_get_zone_lru_size(lruvec, lru, zid);
		else
			size += zone_page_state(zone, NR_ZONE_LRU_BASE + lru);
	}
	return size;
}

/*
 * Add a shrinker callback to be called from the vm.
 */
int prealloc_shrinker(struct shrinker *shrinker)
{
	unsigned int size;
	int err;

	if (shrinker->flags & SHRINKER_MEMCG_AWARE) {
		err = prealloc_memcg_shrinker(shrinker);
		if (err != -ENOSYS)
			return err;

		shrinker->flags &= ~SHRINKER_MEMCG_AWARE;
	}

	size = sizeof(*shrinker->nr_deferred);
	if (shrinker->flags & SHRINKER_NUMA_AWARE)
		size *= nr_node_ids;

	shrinker->nr_deferred = kzalloc(size, GFP_KERNEL);
	if (!shrinker->nr_deferred)
		return -ENOMEM;

	return 0;
}

void free_prealloced_shrinker(struct shrinker *shrinker)
{
	if (shrinker->flags & SHRINKER_MEMCG_AWARE) {
		down_write(&shrinker_rwsem);
		unregister_memcg_shrinker(shrinker);
		up_write(&shrinker_rwsem);
		return;
	}

	kfree(shrinker->nr_deferred);
	shrinker->nr_deferred = NULL;
}

void register_shrinker_prepared(struct shrinker *shrinker)
{
	down_write(&shrinker_rwsem);
	list_add_tail(&shrinker->list, &shrinker_list);
	shrinker->flags |= SHRINKER_REGISTERED;
	up_write(&shrinker_rwsem);
}

int register_shrinker(struct shrinker *shrinker)
{
	int err = prealloc_shrinker(shrinker);

	if (err)
		return err;
	register_shrinker_prepared(shrinker);
	return 0;
}
EXPORT_SYMBOL(register_shrinker);

/*
 * Remove one
 */
void unregister_shrinker(struct shrinker *shrinker)
{
	if (!(shrinker->flags & SHRINKER_REGISTERED))
		return;

	down_write(&shrinker_rwsem);
	list_del(&shrinker->list);
	shrinker->flags &= ~SHRINKER_REGISTERED;
	if (shrinker->flags & SHRINKER_MEMCG_AWARE)
		unregister_memcg_shrinker(shrinker);
	up_write(&shrinker_rwsem);

	kfree(shrinker->nr_deferred);
	shrinker->nr_deferred = NULL;
}
EXPORT_SYMBOL(unregister_shrinker);

#define SHRINK_BATCH 128

static unsigned long do_shrink_slab(struct shrink_control *shrinkctl,
				    struct shrinker *shrinker, int priority)
{
	unsigned long freed = 0;
	unsigned long long delta;
	long total_scan;
	long freeable;
	long nr;
	long new_nr;
	long batch_size = shrinker->batch ? shrinker->batch
					  : SHRINK_BATCH;
	long scanned = 0, next_deferred;

	trace_android_vh_do_shrink_slab(shrinker, shrinkctl, priority);

	freeable = shrinker->count_objects(shrinker, shrinkctl);
	if (freeable == 0 || freeable == SHRINK_EMPTY)
		return freeable;

	/*
	 * copy the current shrinker scan count into a local variable
	 * and zero it so that other concurrent shrinker invocations
	 * don't also do this scanning work.
	 */
	nr = xchg_nr_deferred(shrinker, shrinkctl);

	if (shrinker->seeks) {
		delta = freeable >> priority;
		delta *= 4;
		do_div(delta, shrinker->seeks);
	} else {
		/*
		 * These objects don't require any IO to create. Trim
		 * them aggressively under memory pressure to keep
		 * them from causing refetches in the IO caches.
		 */
		delta = freeable / 2;
	}

	total_scan = nr >> priority;
	total_scan += delta;
	total_scan = min(total_scan, (2 * freeable));

	trace_mm_shrink_slab_start(shrinker, shrinkctl, nr,
				   freeable, delta, total_scan, priority);

	/*
	 * Normally, we should not scan less than batch_size objects in one
	 * pass to avoid too frequent shrinker calls, but if the slab has less
	 * than batch_size objects in total and we are really tight on memory,
	 * we will try to reclaim all available objects, otherwise we can end
	 * up failing allocations although there are plenty of reclaimable
	 * objects spread over several slabs with usage less than the
	 * batch_size.
	 *
	 * We detect the "tight on memory" situations by looking at the total
	 * number of objects we want to scan (total_scan). If it is greater
	 * than the total number of objects on slab (freeable), we must be
	 * scanning at high prio and therefore should try to reclaim as much as
	 * possible.
	 */
	while (total_scan >= batch_size ||
	       total_scan >= freeable) {
		unsigned long ret;
		unsigned long nr_to_scan = min(batch_size, total_scan);

		shrinkctl->nr_to_scan = nr_to_scan;
		shrinkctl->nr_scanned = nr_to_scan;
		ret = shrinker->scan_objects(shrinker, shrinkctl);
		if (ret == SHRINK_STOP)
			break;
		freed += ret;

		count_vm_events(SLABS_SCANNED, shrinkctl->nr_scanned);
		total_scan -= shrinkctl->nr_scanned;
		scanned += shrinkctl->nr_scanned;

		cond_resched();
	}

	/*
	 * The deferred work is increased by any new work (delta) that wasn't
	 * done, decreased by old deferred work that was done now.
	 *
	 * And it is capped to two times of the freeable items.
	 */
	next_deferred = max_t(long, (nr + delta - scanned), 0);
	next_deferred = min(next_deferred, (2 * freeable));

	/*
	 * move the unused scan count back into the shrinker in a
	 * manner that handles concurrent updates.
	 */
	new_nr = add_nr_deferred(next_deferred, shrinker, shrinkctl);

	trace_mm_shrink_slab_end(shrinker, shrinkctl->nid, freed, nr, new_nr, total_scan);
	return freed;
}

#ifdef CONFIG_MEMCG
static unsigned long shrink_slab_memcg(gfp_t gfp_mask, int nid,
			struct mem_cgroup *memcg, int priority)
{
	struct shrinker_info *info;
	unsigned long ret, freed = 0;
	int i;

	if (!mem_cgroup_online(memcg))
		return 0;

	if (!down_read_trylock(&shrinker_rwsem))
		return 0;

	info = shrinker_info_protected(memcg, nid);
	if (unlikely(!info))
		goto unlock;

	for_each_set_bit(i, info->map, shrinker_nr_max) {
		struct shrink_control sc = {
			.gfp_mask = gfp_mask,
			.nid = nid,
			.memcg = memcg,
		};
		struct shrinker *shrinker;

		shrinker = idr_find(&shrinker_idr, i);
		if (unlikely(!shrinker || !(shrinker->flags & SHRINKER_REGISTERED))) {
			if (!shrinker)
				clear_bit(i, info->map);
			continue;
		}

		/* Call non-slab shrinkers even though kmem is disabled */
		if (!memcg_kmem_enabled() &&
		    !(shrinker->flags & SHRINKER_NONSLAB))
			continue;

		ret = do_shrink_slab(&sc, shrinker, priority);
		if (ret == SHRINK_EMPTY) {
			clear_bit(i, info->map);
			/*
			 * After the shrinker reported that it had no objects to
			 * free, but before we cleared the corresponding bit in
			 * the memcg shrinker map, a new object might have been
			 * added. To make sure, we have the bit set in this
			 * case, we invoke the shrinker one more time and reset
			 * the bit if it reports that it is not empty anymore.
			 * The memory barrier here pairs with the barrier in
			 * set_shrinker_bit():
			 *
			 * list_lru_add()     shrink_slab_memcg()
			 *   list_add_tail()    clear_bit()
			 *   <MB>               <MB>
			 *   set_bit()          do_shrink_slab()
			 */
			smp_mb__after_atomic();
			ret = do_shrink_slab(&sc, shrinker, priority);
			if (ret == SHRINK_EMPTY)
				ret = 0;
			else
				set_shrinker_bit(memcg, nid, i);
		}
		freed += ret;

		if (rwsem_is_contended(&shrinker_rwsem)) {
			freed = freed ? : 1;
			break;
		}
	}
unlock:
	up_read(&shrinker_rwsem);
	return freed;
}
#else /* CONFIG_MEMCG */
static unsigned long shrink_slab_memcg(gfp_t gfp_mask, int nid,
			struct mem_cgroup *memcg, int priority)
{
	return 0;
}
#endif /* CONFIG_MEMCG */

/**
 * shrink_slab - shrink slab caches
 * @gfp_mask: allocation context
 * @nid: node whose slab caches to target
 * @memcg: memory cgroup whose slab caches to target
 * @priority: the reclaim priority
 *
 * Call the shrink functions to age shrinkable caches.
 *
 * @nid is passed along to shrinkers with SHRINKER_NUMA_AWARE set,
 * unaware shrinkers will receive a node id of 0 instead.
 *
 * @memcg specifies the memory cgroup to target. Unaware shrinkers
 * are called only if it is the root cgroup.
 *
 * @priority is sc->priority, we take the number of objects and >> by priority
 * in order to get the scan target.
 *
 * Returns the number of reclaimed slab objects.
 */
unsigned long shrink_slab(gfp_t gfp_mask, int nid,
				 struct mem_cgroup *memcg,
				 int priority)
{
	unsigned long ret, freed = 0;
	struct shrinker *shrinker;
	bool bypass = false;

	trace_android_vh_shrink_slab_bypass(gfp_mask, nid, memcg, priority, &bypass);
	if (bypass)
		return 0;

	/*
	 * The root memcg might be allocated even though memcg is disabled
	 * via "cgroup_disable=memory" boot parameter.  This could make
	 * mem_cgroup_is_root() return false, then just run memcg slab
	 * shrink, but skip global shrink.  This may result in premature
	 * oom.
	 */
	if (!mem_cgroup_disabled() && !mem_cgroup_is_root(memcg))
		return shrink_slab_memcg(gfp_mask, nid, memcg, priority);

	if (!down_read_trylock(&shrinker_rwsem))
		goto out;

	list_for_each_entry(shrinker, &shrinker_list, list) {
		struct shrink_control sc = {
			.gfp_mask = gfp_mask,
			.nid = nid,
			.memcg = memcg,
		};

		ret = do_shrink_slab(&sc, shrinker, priority);
		if (ret == SHRINK_EMPTY)
			ret = 0;
		freed += ret;
		/*
		 * Bail out if someone want to register a new shrinker to
		 * prevent the registration from being stalled for long periods
		 * by parallel ongoing shrinking.
		 */
		if (rwsem_is_contended(&shrinker_rwsem)) {
			freed = freed ? : 1;
			break;
		}
	}

	up_read(&shrinker_rwsem);
out:
	cond_resched();
	return freed;
}
EXPORT_SYMBOL_GPL(shrink_slab);

void drop_slab_node(int nid)
{
	unsigned long freed;
	int shift = 0;

	do {
		struct mem_cgroup *memcg = NULL;

		if (fatal_signal_pending(current))
			return;

		freed = 0;
		memcg = mem_cgroup_iter(NULL, NULL, NULL);
		do {
			freed += shrink_slab(GFP_KERNEL, nid, memcg, 0);
		} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)) != NULL);
	} while ((freed >> shift++) > 1);
}

void drop_slab(void)
{
	int nid;

	for_each_online_node(nid)
		drop_slab_node(nid);
}

static inline int is_page_cache_freeable(struct page *page)
{
	/*
	 * A freeable page cache page is referenced only by the caller
	 * that isolated the page, the page cache and optional buffer
	 * heads at page->private.
	 */
	int page_cache_pins = thp_nr_pages(page);
	return page_count(page) - page_has_private(page) == 1 + page_cache_pins;
}

static int may_write_to_inode(struct inode *inode)
{
	if (current->flags & PF_SWAPWRITE)
		return 1;
	if (!inode_write_congested(inode))
		return 1;
	if (inode_to_bdi(inode) == current->backing_dev_info)
		return 1;
	return 0;
}

/*
 * We detected a synchronous write error writing a page out.  Probably
 * -ENOSPC.  We need to propagate that into the address_space for a subsequent
 * fsync(), msync() or close().
 *
 * The tricky part is that after writepage we cannot touch the mapping: nothing
 * prevents it from being freed up.  But we have a ref on the page and once
 * that page is locked, the mapping is pinned.
 *
 * We're allowed to run sleeping lock_page() here because we know the caller has
 * __GFP_FS.
 */
static void handle_write_error(struct address_space *mapping,
				struct page *page, int error)
{
	lock_page(page);
	if (page_mapping(page) == mapping)
		mapping_set_error(mapping, error);
	unlock_page(page);
}

/* possible outcome of pageout() */
typedef enum {
	/* failed to write page out, page is locked */
	PAGE_KEEP,
	/* move page to the active list, page is locked */
	PAGE_ACTIVATE,
	/* page has been sent to the disk successfully, page is unlocked */
	PAGE_SUCCESS,
	/* page is clean and locked */
	PAGE_CLEAN,
} pageout_t;

/*
 * pageout is called by shrink_page_list() for each dirty page.
 * Calls ->writepage().
 */
static pageout_t pageout(struct page *page, struct address_space *mapping)
{
	/*
	 * If the page is dirty, only perform writeback if that write
	 * will be non-blocking.  To prevent this allocation from being
	 * stalled by pagecache activity.  But note that there may be
	 * stalls if we need to run get_block().  We could test
	 * PagePrivate for that.
	 *
	 * If this process is currently in __generic_file_write_iter() against
	 * this page's queue, we can perform writeback even if that
	 * will block.
	 *
	 * If the page is swapcache, write it back even if that would
	 * block, for some throttling. This happens by accident, because
	 * swap_backing_dev_info is bust: it doesn't reflect the
	 * congestion state of the swapdevs.  Easy to fix, if needed.
	 */
	if (!is_page_cache_freeable(page))
		return PAGE_KEEP;
	if (!mapping) {
		/*
		 * Some data journaling orphaned pages can have
		 * page->mapping == NULL while being dirty with clean buffers.
		 */
		if (page_has_private(page)) {
			if (try_to_free_buffers(page)) {
				ClearPageDirty(page);
				pr_info("%s: orphaned page\n", __func__);
				return PAGE_CLEAN;
			}
		}
		return PAGE_KEEP;
	}
	if (mapping->a_ops->writepage == NULL)
		return PAGE_ACTIVATE;
	if (!may_write_to_inode(mapping->host))
		return PAGE_KEEP;

	if (clear_page_dirty_for_io(page)) {
		int res;
		struct writeback_control wbc = {
			.sync_mode = WB_SYNC_NONE,
			.nr_to_write = SWAP_CLUSTER_MAX,
			.range_start = 0,
			.range_end = LLONG_MAX,
			.for_reclaim = 1,
		};

		SetPageReclaim(page);
		res = mapping->a_ops->writepage(page, &wbc);
		if (res < 0)
			handle_write_error(mapping, page, res);
		if (res == AOP_WRITEPAGE_ACTIVATE) {
			ClearPageReclaim(page);
			return PAGE_ACTIVATE;
		}

		if (!PageWriteback(page)) {
			/* synchronous write or broken a_ops? */
			ClearPageReclaim(page);
		}
		trace_mm_vmscan_writepage(page);
		inc_node_page_state(page, NR_VMSCAN_WRITE);
		return PAGE_SUCCESS;
	}

	return PAGE_CLEAN;
}

/*
 * Same as remove_mapping, but if the page is removed from the mapping, it
 * gets returned with a refcount of 0.
 */
static int __remove_mapping(struct address_space *mapping, struct page *page,
			    bool reclaimed, struct mem_cgroup *target_memcg)
{
	int refcount;
	void *shadow = NULL;

	BUG_ON(!PageLocked(page));
	BUG_ON(mapping != page_mapping(page));

	xa_lock_irq(&mapping->i_pages);
	/*
	 * The non racy check for a busy page.
	 *
	 * Must be careful with the order of the tests. When someone has
	 * a ref to the page, it may be possible that they dirty it then
	 * drop the reference. So if PageDirty is tested before page_count
	 * here, then the following race may occur:
	 *
	 * get_user_pages(&page);
	 * [user mapping goes away]
	 * write_to(page);
	 *				!PageDirty(page)    [good]
	 * SetPageDirty(page);
	 * put_page(page);
	 *				!page_count(page)   [good, discard it]
	 *
	 * [oops, our write_to data is lost]
	 *
	 * Reversing the order of the tests ensures such a situation cannot
	 * escape unnoticed. The smp_rmb is needed to ensure the page->flags
	 * load is not satisfied before that of page->_refcount.
	 *
	 * Note that if SetPageDirty is always performed via set_page_dirty,
	 * and thus under the i_pages lock, then this ordering is not required.
	 */
	refcount = 1 + compound_nr(page);
	if (!page_ref_freeze(page, refcount))
		goto cannot_free;
	/* note: atomic_cmpxchg in page_ref_freeze provides the smp_rmb */
	if (unlikely(PageDirty(page))) {
		page_ref_unfreeze(page, refcount);
		goto cannot_free;
	}

	if (PageSwapCache(page)) {
		swp_entry_t swap = { .val = page_private(page) };

		/* get a shadow entry before mem_cgroup_swapout() clears page_memcg() */
		if (reclaimed && !mapping_exiting(mapping))
			shadow = workingset_eviction(page, target_memcg);
		mem_cgroup_swapout(page, swap);
		__delete_from_swap_cache(page, swap, shadow);
		xa_unlock_irq(&mapping->i_pages);
		put_swap_page(page, swap);
	} else {
		void (*freepage)(struct page *);

		freepage = mapping->a_ops->freepage;
		/*
		 * Remember a shadow entry for reclaimed file cache in
		 * order to detect refaults, thus thrashing, later on.
		 *
		 * But don't store shadows in an address space that is
		 * already exiting.  This is not just an optimization,
		 * inode reclaim needs to empty out the radix tree or
		 * the nodes are lost.  Don't plant shadows behind its
		 * back.
		 *
		 * We also don't store shadows for DAX mappings because the
		 * only page cache pages found in these are zero pages
		 * covering holes, and because we don't want to mix DAX
		 * exceptional entries and shadow exceptional entries in the
		 * same address_space.
		 */
		if (reclaimed && page_is_file_lru(page) &&
		    !mapping_exiting(mapping) && !dax_mapping(mapping))
			shadow = workingset_eviction(page, target_memcg);
		__delete_from_page_cache(page, shadow);
		xa_unlock_irq(&mapping->i_pages);

		if (freepage != NULL)
			freepage(page);
	}

	return 1;

cannot_free:
	xa_unlock_irq(&mapping->i_pages);
	return 0;
}

/*
 * Attempt to detach a locked page from its ->mapping.  If it is dirty or if
 * someone else has a ref on the page, abort and return 0.  If it was
 * successfully detached, return 1.  Assumes the caller has a single ref on
 * this page.
 */
int remove_mapping(struct address_space *mapping, struct page *page)
{
	if (__remove_mapping(mapping, page, false, NULL)) {
		/*
		 * Unfreezing the refcount with 1 rather than 2 effectively
		 * drops the pagecache ref for us without requiring another
		 * atomic operation.
		 */
		page_ref_unfreeze(page, 1);
		return 1;
	}
	return 0;
}

/**
 * putback_lru_page - put previously isolated page onto appropriate LRU list
 * @page: page to be put back to appropriate lru list
 *
 * Add previously isolated @page to appropriate LRU list.
 * Page may still be unevictable for other reasons.
 *
 * lru_lock must not be held, interrupts must be enabled.
 */
void putback_lru_page(struct page *page)
{
	lru_cache_add(page);
	put_page(page);		/* drop ref from isolate */
}

enum page_references {
	PAGEREF_RECLAIM,
	PAGEREF_RECLAIM_CLEAN,
	PAGEREF_KEEP,
	PAGEREF_ACTIVATE,
};

static enum page_references page_check_references(struct page *page,
						  struct scan_control *sc)
{
	int referenced_ptes, referenced_page;
	unsigned long vm_flags;
	bool should_protect = false;
	bool trylock_fail = false;
	int ret = 0;

	trace_android_vh_page_should_be_protected(page, &should_protect);
#ifdef CONFIG_CONT_PTE_HUGEPAGE
	if (ContPteHugePageSkipMassiveMapped(page))
		should_protect = 1;
#endif
	if (unlikely(should_protect))
		return PAGEREF_ACTIVATE;

	trace_android_vh_page_trylock_set(page);
	trace_android_vh_check_page_look_around_ref(page, &ret);
	if (ret)
		return ret;
	referenced_ptes = page_referenced(page, 1, sc->target_mem_cgroup,
					  &vm_flags);
	referenced_page = TestClearPageReferenced(page);
	trace_android_vh_page_trylock_get_result(page, &trylock_fail);
	if (trylock_fail)
		return PAGEREF_KEEP;
	/*
	 * Mlock lost the isolation race with us.  Let try_to_unmap()
	 * move the page to the unevictable list.
	 */
	if (vm_flags & VM_LOCKED)
		return PAGEREF_RECLAIM;

	/* rmap lock contention: rotate */
	if (referenced_ptes == -1)
		return PAGEREF_KEEP;

	if (referenced_ptes) {
		/*
		 * All mapped pages start out with page table
		 * references from the instantiating fault, so we need
		 * to look twice if a mapped file page is used more
		 * than once.
		 *
		 * Mark it and spare it for another trip around the
		 * inactive list.  Another page table reference will
		 * lead to its activation.
		 *
		 * Note: the mark is set for activated pages as well
		 * so that recently deactivated but used pages are
		 * quickly recovered.
		 */
		SetPageReferenced(page);

		if (referenced_page || referenced_ptes > 1)
			return PAGEREF_ACTIVATE;

		/*
		 * Activate file-backed executable pages after first usage.
		 */
		if ((vm_flags & VM_EXEC) && !PageSwapBacked(page))
			return PAGEREF_ACTIVATE;

		return PAGEREF_KEEP;
	}

	/* Reclaim if clean, defer dirty pages to writeback */
	if (referenced_page && !PageSwapBacked(page))
		return PAGEREF_RECLAIM_CLEAN;

	return PAGEREF_RECLAIM;
}

/* Check if a page is dirty or under writeback */
static void page_check_dirty_writeback(struct page *page,
				       bool *dirty, bool *writeback)
{
	struct address_space *mapping;

	/*
	 * Anonymous pages are not handled by flushers and must be written
	 * from reclaim context. Do not stall reclaim based on them
	 */
	if (!page_is_file_lru(page) ||
	    (PageAnon(page) && !PageSwapBacked(page))) {
		*dirty = false;
		*writeback = false;
		return;
	}

	/* By default assume that the page flags are accurate */
	*dirty = PageDirty(page);
	*writeback = PageWriteback(page);

	/* Verify dirty/writeback state if the filesystem supports it */
	if (!page_has_private(page))
		return;

	mapping = page_mapping(page);
	if (mapping && mapping->a_ops->is_dirty_writeback)
		mapping->a_ops->is_dirty_writeback(page, dirty, writeback);
}

static struct page *alloc_demote_page(struct page *page, unsigned long node)
{
	struct migration_target_control mtc = {
		/*
		 * Allocate from 'node', or fail quickly and quietly.
		 * When this happens, 'page' will likely just be discarded
		 * instead of migrated.
		 */
		.gfp_mask = (GFP_HIGHUSER_MOVABLE & ~__GFP_RECLAIM) |
			    __GFP_THISNODE  | __GFP_NOWARN |
			    __GFP_NOMEMALLOC | GFP_NOWAIT,
		.nid = node
	};

	return alloc_migration_target(page, (unsigned long)&mtc);
}

/*
 * Take pages on @demote_list and attempt to demote them to
 * another node.  Pages which are not demoted are left on
 * @demote_pages.
 */
static unsigned int demote_page_list(struct list_head *demote_pages,
				     struct pglist_data *pgdat)
{
	int target_nid = next_demotion_node(pgdat->node_id);
	unsigned int nr_succeeded;
	int err;

	if (list_empty(demote_pages))
		return 0;

	if (target_nid == NUMA_NO_NODE)
		return 0;

	/* Demotion ignores all cpuset and mempolicy settings */
	err = migrate_pages(demote_pages, alloc_demote_page, NULL,
			    target_nid, MIGRATE_ASYNC, MR_DEMOTION,
			    &nr_succeeded);

	if (current_is_kswapd())
		__count_vm_events(PGDEMOTE_KSWAPD, nr_succeeded);
	else
		__count_vm_events(PGDEMOTE_DIRECT, nr_succeeded);

	return nr_succeeded;
}

/*
 * shrink_page_list() returns the number of reclaimed pages
 */
static unsigned int shrink_page_list(struct list_head *page_list,
				     struct pglist_data *pgdat,
				     struct scan_control *sc,
				     struct reclaim_stat *stat,
				     bool ignore_references)
{
	LIST_HEAD(ret_pages);
	LIST_HEAD(free_pages);
	LIST_HEAD(demote_pages);
	unsigned int nr_reclaimed = 0;
	unsigned int pgactivate = 0;
	bool do_demote_pass;
	bool page_trylock_result;

	memset(stat, 0, sizeof(*stat));
	cond_resched();
	do_demote_pass = can_demote(pgdat->node_id, sc);

retry:
	while (!list_empty(page_list)) {
		struct address_space *mapping;
		struct page *page;
		enum page_references references = PAGEREF_RECLAIM;
		bool dirty, writeback, may_enter_fs;
		unsigned int nr_pages;

		cond_resched();

		page = lru_to_page(page_list);
		list_del(&page->lru);

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
		CHP_BUG_ON(sc->gfp_mask & POOL_USER_ALLOC_MASK && !ContPteCMAHugePageHead(page));
#endif
		if (!trylock_page(page))
			goto keep;

#ifdef CONFIG_CONT_PTE_HUGEPAGE
		CHP_BUG_ON(PageCont(page) && !ContPteHugePageHead(page));

		/*
		 * because we don't split file-thp during reclamation, we have to
		 * keep double mapped pages but they are quite few
		 */
		if (ContPteHugePageHead(page) && ContPteHugePageDoubleMap(page)) {
			pr_debug("Shrink_page:Skip doublemap pages in memory reclamation- page:%p\n", page);
			goto keep_locked;
		}
#endif

		VM_BUG_ON_PAGE(PageActive(page), page);

		nr_pages = compound_nr(page);

		/* Account the number of base pages even though THP */
		sc->nr_scanned += nr_pages;

		if (unlikely(!page_evictable(page)))
			goto activate_locked;

		if (!sc->may_unmap && page_mapped(page))
			goto keep_locked;

		/* page_update_gen() tried to promote this page? */
		if (lru_gen_enabled() && !ignore_references &&
		    page_mapped(page) && PageReferenced(page))
			goto keep_locked;

		may_enter_fs = (sc->gfp_mask & __GFP_FS) ||
			(PageSwapCache(page) && (sc->gfp_mask & __GFP_IO));

		/*
		 * The number of dirty pages determines if a node is marked
		 * reclaim_congested which affects wait_iff_congested. kswapd
		 * will stall and start writing pages if the tail of the LRU
		 * is all dirty unqueued pages.
		 */
		page_check_dirty_writeback(page, &dirty, &writeback);
		if (dirty || writeback)
			stat->nr_dirty++;

		if (dirty && !writeback)
			stat->nr_unqueued_dirty++;

		/*
		 * Treat this page as congested if the underlying BDI is or if
		 * pages are cycling through the LRU so quickly that the
		 * pages marked for immediate reclaim are making it to the
		 * end of the LRU a second time.
		 */
		mapping = page_mapping(page);
		if (((dirty || writeback) && mapping &&
		     inode_write_congested(mapping->host)) ||
		    (writeback && PageReclaim(page)))
			stat->nr_congested++;

		/*
		 * If a page at the tail of the LRU is under writeback, there
		 * are three cases to consider.
		 *
		 * 1) If reclaim is encountering an excessive number of pages
		 *    under writeback and this page is both under writeback and
		 *    PageReclaim then it indicates that pages are being queued
		 *    for IO but are being recycled through the LRU before the
		 *    IO can complete. Waiting on the page itself risks an
		 *    indefinite stall if it is impossible to writeback the
		 *    page due to IO error or disconnected storage so instead
		 *    note that the LRU is being scanned too quickly and the
		 *    caller can stall after page list has been processed.
		 *
		 * 2) Global or new memcg reclaim encounters a page that is
		 *    not marked for immediate reclaim, or the caller does not
		 *    have __GFP_FS (or __GFP_IO if it's simply going to swap,
		 *    not to fs). In this case mark the page for immediate
		 *    reclaim and continue scanning.
		 *
		 *    Require may_enter_fs because we would wait on fs, which
		 *    may not have submitted IO yet. And the loop driver might
		 *    enter reclaim, and deadlock if it waits on a page for
		 *    which it is needed to do the write (loop masks off
		 *    __GFP_IO|__GFP_FS for this reason); but more thought
		 *    would probably show more reasons.
		 *
		 * 3) Legacy memcg encounters a page that is already marked
		 *    PageReclaim. memcg does not have any dirty pages
		 *    throttling so we could easily OOM just because too many
		 *    pages are in writeback and there is nothing else to
		 *    reclaim. Wait for the writeback to complete.
		 *
		 * In cases 1) and 2) we activate the pages to get them out of
		 * the way while we continue scanning for clean pages on the
		 * inactive list and refilling from the active list. The
		 * observation here is that waiting for disk writes is more
		 * expensive than potentially causing reloads down the line.
		 * Since they're marked for immediate reclaim, they won't put
		 * memory pressure on the cache working set any longer than it
		 * takes to write them to disk.
		 */
		if (PageWriteback(page)) {
			/* Case 1 above */
			if (current_is_kswapd() &&
			    PageReclaim(page) &&
			    test_bit(PGDAT_WRITEBACK, &pgdat->flags)) {
				stat->nr_immediate++;
				goto activate_locked;

			/* Case 2 above */
			} else if (writeback_throttling_sane(sc) ||
			    !PageReclaim(page) || !may_enter_fs) {
				/*
				 * This is slightly racy - end_page_writeback()
				 * might have just cleared PageReclaim, then
				 * setting PageReclaim here end up interpreted
				 * as PageReadahead - but that does not matter
				 * enough to care.  What we do want is for this
				 * page to have PageReclaim set next time memcg
				 * reclaim reaches the tests above, so it will
				 * then wait_on_page_writeback() to avoid OOM;
				 * and it's also appropriate in global reclaim.
				 */
				SetPageReclaim(page);
				stat->nr_writeback++;
				goto activate_locked;

			/* Case 3 above */
			} else {
				unlock_page(page);
				wait_on_page_writeback(page);
				/* then go back and try same page again */
				list_add_tail(&page->lru, page_list);
				continue;
			}
		}

		if (!ignore_references)
			references = page_check_references(page, sc);

		switch (references) {
		case PAGEREF_ACTIVATE:
			goto activate_locked;
		case PAGEREF_KEEP:
			stat->nr_ref_keep += nr_pages;
			goto keep_locked;
		case PAGEREF_RECLAIM:
		case PAGEREF_RECLAIM_CLEAN:
			; /* try to reclaim the page below */
		}

		/*
		 * Before reclaiming the page, try to relocate
		 * its contents to another node.
		 */
		if (do_demote_pass &&
		    (thp_migration_supported() || !PageTransHuge(page))) {
			list_add(&page->lru, &demote_pages);
			unlock_page(page);
			continue;
		}

		/*
		 * Anonymous process memory has backing store?
		 * Try to allocate it some swap space here.
		 * Lazyfree page could be freed directly
		 */
		if (PageAnon(page) && PageSwapBacked(page)) {
			if (!PageSwapCache(page)) {
				if (!(sc->gfp_mask & __GFP_IO))
					goto keep_locked;
				if (page_maybe_dma_pinned(page))
					goto keep_locked;
				if (PageTransHuge(page)) {
					/* cannot split THP, skip it */
					if (!can_split_huge_page(page, NULL))
						goto activate_locked;

#ifdef CONFIG_CONT_PTE_HUGEPAGE
					if (ContPteHugePageHead(page) && ContPteHugePageDoubleMap(page)) {
						pr_warn_ratelimited("@@@@%s anon page:%pK mapcount-0 anon:%d swapback:%d mapped:%d\n",
							__func__, page, PageAnon(page), PageSwapBacked(page), page_mapped(page));
						goto activate_locked;
					}
#else
					/*
					 * Split pages without a PMD map right
					 * away. Chances are some or all of the
					 * tail pages can be freed without IO.
					 */
					if (!compound_mapcount(page) &&
					    split_huge_page_to_list(page,
								    page_list))
						goto activate_locked;
#endif
				}
				if (!add_to_swap(page)) {
#ifndef CONFIG_CONT_PTE_HUGEPAGE
					if (!PageTransHuge(page))
						goto activate_locked_split;
#else
					/*
					 * FIXME: For cont-pte hugepages, if swap fails to be added,
					 * we do not split them. However, if the swap partition is
					 * fragmented, there are no consecutive 16 slots, splitting
					 * into small pages may reclaim more memory?
					 */
					goto activate_locked;
#endif
					/* Fallback to swap normal pages */
					if (split_huge_page_to_list(page,
								    page_list))
						goto activate_locked;
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
					count_vm_event(THP_SWPOUT_FALLBACK);
#endif
					if (!add_to_swap(page))
						goto activate_locked_split;
				}

				may_enter_fs = true;

				/* Adding to swap updated mapping */
				mapping = page_mapping(page);
			}
		} else if (unlikely(PageTransHuge(page))) {
			/* Split file THP, for cont-pte pages, we reclaim them as a whole */
#ifdef CONFIG_CONT_PTE_HUGEPAGE
			if (!ContPteHugePageHead(page) && split_huge_page_to_list(page, page_list))
#else
			if (split_huge_page_to_list(page, page_list))
#endif
				goto keep_locked;
		}

		/*
		 * THP may get split above, need minus tail pages and update
		 * nr_pages to avoid accounting tail pages twice.
		 *
		 * The tail pages that are added into swap cache successfully
		 * reach here.
		 */
		if ((nr_pages > 1) && !PageTransHuge(page)) {
			sc->nr_scanned -= (nr_pages - 1);
			nr_pages = 1;
		}

		/*
		 * The page is mapped into the page tables of one or more
		 * processes. Try to unmap it here.
		 */
		if (page_mapped(page)) {
			enum ttu_flags flags = TTU_BATCH_FLUSH;
			bool was_swapbacked = PageSwapBacked(page);

			if (unlikely(PageTransHuge(page)))
				flags |= TTU_SPLIT_HUGE_PMD;
			if (!ignore_references)
				trace_android_vh_page_trylock_set(page);
			try_to_unmap(page, flags);
#ifdef CONFIG_CONT_PTE_HUGEPAGE
			/* don't struggle with doublemap cont_pte pages */
			if ((ContPteHugePageHead(page) && ContPteHugePageDoubleMap(page)) || page_mapped(page)) {
#else
			if (page_mapped(page)) {
#endif
				stat->nr_unmap_fail += nr_pages;
				if (!was_swapbacked && PageSwapBacked(page))
					stat->nr_lazyfree_fail += nr_pages;
				goto activate_locked;
			}
		}

		if (PageDirty(page)) {
			/*
			 * Only kswapd can writeback filesystem pages
			 * to avoid risk of stack overflow. But avoid
			 * injecting inefficient single-page IO into
			 * flusher writeback as much as possible: only
			 * write pages when we've encountered many
			 * dirty pages, and when we've already scanned
			 * the rest of the LRU for clean pages and see
			 * the same dirty pages again (PageReclaim).
			 */
			if (page_is_file_lru(page) &&
			    (!current_is_kswapd() || !PageReclaim(page) ||
			     !test_bit(PGDAT_DIRTY, &pgdat->flags))) {
				/*
				 * Immediately reclaim when written back.
				 * Similar in principal to deactivate_page()
				 * except we already have the page isolated
				 * and know it's dirty
				 */
				inc_node_page_state(page, NR_VMSCAN_IMMEDIATE);
				SetPageReclaim(page);

				goto activate_locked;
			}

			if (references == PAGEREF_RECLAIM_CLEAN)
				goto keep_locked;
			if (!may_enter_fs)
				goto keep_locked;
			if (!sc->may_writepage)
				goto keep_locked;

			/*
			 * Page is dirty. Flush the TLB if a writable entry
			 * potentially exists to avoid CPU writes after IO
			 * starts and then write it out here.
			 */
			try_to_unmap_flush_dirty();
			switch (pageout(page, mapping)) {
			case PAGE_KEEP:
				goto keep_locked;
			case PAGE_ACTIVATE:
				goto activate_locked;
			case PAGE_SUCCESS:
				stat->nr_pageout += thp_nr_pages(page);

				if (PageWriteback(page))
					goto keep;
				if (PageDirty(page))
					goto keep;

				/*
				 * A synchronous write - probably a ramdisk.  Go
				 * ahead and try to reclaim the page.
				 */
				if (!trylock_page(page))
					goto keep;
				if (PageDirty(page) || PageWriteback(page))
					goto keep_locked;
				mapping = page_mapping(page);
				fallthrough;
			case PAGE_CLEAN:
				; /* try to free the page below */
			}
		}

		/*
		 * If the page has buffers, try to free the buffer mappings
		 * associated with this page. If we succeed we try to free
		 * the page as well.
		 *
		 * We do this even if the page is PageDirty().
		 * try_to_release_page() does not perform I/O, but it is
		 * possible for a page to have PageDirty set, but it is actually
		 * clean (all its buffers are clean).  This happens if the
		 * buffers were written out directly, with submit_bh(). ext3
		 * will do this, as well as the blockdev mapping.
		 * try_to_release_page() will discover that cleanness and will
		 * drop the buffers and mark the page clean - it can be freed.
		 *
		 * Rarely, pages can have buffers and no ->mapping.  These are
		 * the pages which were not successfully invalidated in
		 * truncate_cleanup_page().  We try to drop those buffers here
		 * and if that worked, and the page is no longer mapped into
		 * process address space (page_count == 1) it can be freed.
		 * Otherwise, leave the page on the LRU so it is swappable.
		 */
		if (page_has_private(page)) {
			if (!try_to_release_page(page, sc->gfp_mask))
				goto activate_locked;
			if (!mapping && page_count(page) == 1) {
				unlock_page(page);
				if (put_page_testzero(page))
					goto free_it;
				else {
					/*
					 * rare race with speculative reference.
					 * the speculative reference will free
					 * this page shortly, so we may
					 * increment nr_reclaimed here (and
					 * leave it off the LRU).
					 */
					trace_android_vh_page_trylock_clear(page);
					nr_reclaimed++;
					continue;
				}
			}
		}

		if (PageAnon(page) && !PageSwapBacked(page)) {
			/* follow __remove_mapping for reference */
			if (!page_ref_freeze(page, 1))
				goto keep_locked;
			/*
			 * The page has only one reference left, which is
			 * from the isolation. After the caller puts the
			 * page back on lru and drops the reference, the
			 * page will be freed anyway. It doesn't matter
			 * which lru it goes. So we don't bother checking
			 * PageDirty here.
			 */
			count_vm_event(PGLAZYFREED);
			count_memcg_page_event(page, PGLAZYFREED);
		} else if (!mapping || !__remove_mapping(mapping, page, true,
							 sc->target_mem_cgroup))
			goto keep_locked;

		unlock_page(page);
free_it:
		/*
		 * THP may get swapped out in a whole, need account
		 * all base pages.
		 */
		nr_reclaimed += nr_pages;

		/* For debugging, detect the subpages' reclamation */
#ifdef CONFIG_CONT_PTE_HUGEPAGE
		WARN_ON(PageCont(page) && !PageHead(page));
#endif
		/*
		 * Is there need to periodically free_page_list? It would
		 * appear not as the counts should be low
		 */
		trace_android_vh_page_trylock_clear(page);
		if (unlikely(PageTransHuge(page))) {
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
			if (sc->gfp_mask & POOL_USER_ALLOC_MASK &&
			    ContPteCMAHugePageHead(page))
				pr_debug_ratelimited("@%s:%d page:%lx comm:%s pid:%d nr_reclaimed:%d @\n",
						     __func__, __LINE__, page,
						     current->comm,
						     current->pid,
						     nr_reclaimed);
#endif
			destroy_compound_page(page);
		}
		else
			list_add(&page->lru, &free_pages);
		continue;

activate_locked_split:
		/*
		 * The tail pages that are failed to add into swap cache
		 * reach here.  Fixup nr_scanned and nr_pages.
		 */
		if (nr_pages > 1) {
			sc->nr_scanned -= (nr_pages - 1);
			nr_pages = 1;
		}
activate_locked:
		/* Not a candidate for swapping, so reclaim swap space. */
		if (PageSwapCache(page) && (mem_cgroup_swap_full(page) ||
						PageMlocked(page)))
			try_to_free_swap(page);
		VM_BUG_ON_PAGE(PageActive(page), page);
		if (!PageMlocked(page)) {
			int type = page_is_file_lru(page);
			SetPageActive(page);
			stat->nr_activate[type] += nr_pages;
			count_memcg_page_event(page, PGACTIVATE);
		}
keep_locked:
		/*
		 * The page with trylock-bit will be added ret_pages and
		 * handled in trace_android_vh_handle_failed_page_trylock.
		 * If the page carried with trylock-bit after unlocked by
		 * shrink_page_list will cause some error-issues in other
		 * scene, so clear trylock-bit here.
		 * trace_android_vh_page_trylock_get_result will clear
		 * trylock-bit and return if page tyrlock failed in
		 * reclaim-process. Here we just want to clear trylock-bit
		 * so that ignore page_trylock_result.
		 * TODO: trace_android_vh_page_trylock_get_result should be
		 * changed to a different hook which correctly reflects the
		 * usage here, which is to clear the try-lock bit.
		 */
		trace_android_vh_page_trylock_get_result(page, &page_trylock_result);
		unlock_page(page);
keep:
		list_add(&page->lru, &ret_pages);
		VM_BUG_ON_PAGE(PageLRU(page) || PageUnevictable(page), page);
	}
	/* 'page_list' is always empty here */

	/* Migrate pages selected for demotion */
	nr_reclaimed += demote_page_list(&demote_pages, pgdat);
	/* Pages that could not be demoted are still in @demote_pages */
	if (!list_empty(&demote_pages)) {
		/* Pages which failed to demoted go back on @page_list for retry: */
		list_splice_init(&demote_pages, page_list);
		do_demote_pass = false;
		goto retry;
	}

	pgactivate = stat->nr_activate[0] + stat->nr_activate[1];

	mem_cgroup_uncharge_list(&free_pages);
	try_to_unmap_flush();
	free_unref_page_list(&free_pages);

	list_splice(&ret_pages, page_list);
	count_vm_events(PGACTIVATE, pgactivate);

	return nr_reclaimed;
}

unsigned int reclaim_clean_pages_from_list(struct zone *zone,
					    struct list_head *page_list)
{
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.may_unmap = 1,
	};
	struct reclaim_stat stat;
	unsigned int nr_reclaimed;
	struct page *page, *next;
	LIST_HEAD(clean_pages);
	unsigned int noreclaim_flag;

	list_for_each_entry_safe(page, next, page_list, lru) {
		if (!PageHuge(page) && page_is_file_lru(page) &&
		    !PageDirty(page) && !__PageMovable(page) &&
		    !PageUnevictable(page)) {
			ClearPageActive(page);
			list_move(&page->lru, &clean_pages);
		}
	}

	/*
	 * We should be safe here since we are only dealing with file pages and
	 * we are not kswapd and therefore cannot write dirty file pages. But
	 * call memalloc_noreclaim_save() anyway, just in case these conditions
	 * change in the future.
	 */
	noreclaim_flag = memalloc_noreclaim_save();
	nr_reclaimed = shrink_page_list(&clean_pages, zone->zone_pgdat, &sc,
					&stat, true);
	memalloc_noreclaim_restore(noreclaim_flag);

	list_splice(&clean_pages, page_list);
	mod_node_page_state(zone->zone_pgdat, NR_ISOLATED_FILE,
			    -(long)nr_reclaimed);
	/*
	 * Since lazyfree pages are isolated from file LRU from the beginning,
	 * they will rotate back to anonymous LRU in the end if it failed to
	 * discard so isolated count will be mismatched.
	 * Compensate the isolated count for both LRU lists.
	 */
	mod_node_page_state(zone->zone_pgdat, NR_ISOLATED_ANON,
			    stat.nr_lazyfree_fail);
	mod_node_page_state(zone->zone_pgdat, NR_ISOLATED_FILE,
			    -(long)stat.nr_lazyfree_fail);
	return nr_reclaimed;
}

/*
 * Update LRU sizes after isolating pages. The LRU size updates must
 * be complete before mem_cgroup_update_lru_size due to a sanity check.
 */
static __always_inline void update_lru_sizes(struct lruvec *lruvec,
			enum lru_list lru, unsigned long *nr_zone_taken)
{
	int zid;

	for (zid = 0; zid < MAX_NR_ZONES; zid++) {
		if (!nr_zone_taken[zid])
			continue;

		update_lru_size(lruvec, lru, zid, -nr_zone_taken[zid]);
	}

}

#ifdef CONFIG_CMA
/*
 * It is waste of effort to scan and reclaim CMA pages if it is not available
 * for current allocation context. Kswapd can not be enrolled as it can not
 * distinguish this scenario by using sc->gfp_mask = GFP_KERNEL
 */
static bool skip_cma(struct page *page, struct scan_control *sc)
{
#ifdef CONFIG_CONT_PTE_HUGEPAGE
	/*
	 * if skip cma in shrink_inactive_list which would not isolate cma
	 * page. much threads would stuck in too_many_isolated and no chance
	 * to tigger lmkd or oom killer.
	 */
	return false;
#else
	return !current_is_kswapd() &&
			gfp_migratetype(sc->gfp_mask) != MIGRATE_MOVABLE &&
			get_pageblock_migratetype(page) == MIGRATE_CMA;
#endif
}
#else
static bool skip_cma(struct page *page, struct scan_control *sc)
{
	return false;
}
#endif

/*
 * Isolating page from the lruvec to fill in @dst list by nr_to_scan times.
 *
 * lruvec->lru_lock is heavily contended.  Some of the functions that
 * shrink the lists perform better by taking out a batch of pages
 * and working on them outside the LRU lock.
 *
 * For pagecache intensive workloads, this function is the hottest
 * spot in the kernel (apart from copy_*_user functions).
 *
 * Lru_lock must be held before calling this function.
 *
 * @nr_to_scan:	The number of eligible pages to look through on the list.
 * @lruvec:	The LRU vector to pull pages from.
 * @dst:	The temp list to put pages on to.
 * @nr_scanned:	The number of pages that were scanned.
 * @sc:		The scan_control struct for this reclaim session
 * @lru:	LRU list id for isolating
 *
 * returns how many pages were moved onto *@dst.
 */
static unsigned long isolate_lru_pages(unsigned long nr_to_scan,
		struct lruvec *lruvec, struct list_head *dst,
		unsigned long *nr_scanned, struct scan_control *sc,
		enum lru_list lru)
{
	struct list_head *src = &lruvec->lists[lru];
	unsigned long nr_taken = 0;
	unsigned long nr_zone_taken[MAX_NR_ZONES] = { 0 };
	unsigned long nr_skipped[MAX_NR_ZONES] = { 0, };
	unsigned long skipped = 0;
	unsigned long scan, total_scan, nr_pages;
	LIST_HEAD(pages_skipped);
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
	bool chp_reclaim = !!(sc->gfp_mask & POOL_USER_ALLOC_MASK);
#endif

	total_scan = 0;
	scan = 0;
	while (scan < nr_to_scan && !list_empty(src)) {
		struct list_head *move_to = src;
		struct page *page;

		page = lru_to_page(src);
		prefetchw_prev_lru_page(page, src, flags);

		nr_pages = compound_nr(page);
		total_scan += nr_pages;

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
		if ((chp_reclaim && !ContPteCMAHugePageHead(page)) ||
		    (!chp_reclaim && ContPteCMAHugePageHead(page)) ||
		    (chp_reclaim && !is_chp_lruvec(lruvec)) ||
		    (!chp_reclaim && is_chp_lruvec(lruvec))) {
			pr_err("@@@%s:%d comm:%s pid:%d nr_to_reclaim:%ld nr_scanned:%ld nr_reclaimed:%ld"
			       "lruvec:%lx is_chp_lruvec:%d lru:%d  ContPteCMAHugePageHead:%d POOL_USER_ALLOC:%d @\n",
			       __func__, __LINE__, current->comm, current->pid, sc->nr_to_reclaim, sc->nr_scanned,
			       sc->nr_reclaimed, lruvec, is_chp_lruvec(lruvec), lru, ContPteCMAHugePageHead(page),
			       chp_reclaim);
			CHP_BUG_ON_EMERGENCY(1);
		}

		CHP_BUG_ON(chp_reclaim && sc->order != HPAGE_CONT_PTE_ORDER);
#endif
		if (page_zonenum(page) > sc->reclaim_idx ||
				skip_cma(page, sc)) {
			nr_skipped[page_zonenum(page)] += nr_pages;
			move_to = &pages_skipped;
			goto move;
		}

		/*
		 * Do not count skipped pages because that makes the function
		 * return with no isolated pages if the LRU mostly contains
		 * ineligible pages.  This causes the VM to not reclaim any
		 * pages, triggering a premature OOM.
		 * Account all tail pages of THP.
		 */
		scan += nr_pages;

		if (!PageLRU(page))
			goto move;
		if (!sc->may_unmap && page_mapped(page))
			goto move;

		/*
		 * Be careful not to clear PageLRU until after we're
		 * sure the page is not being freed elsewhere -- the
		 * page release code relies on it.
		 */
		if (unlikely(!get_page_unless_zero(page)))
			goto move;

		if (!TestClearPageLRU(page)) {
			/* Another thread is already isolating this page */
			put_page(page);
			goto move;
		}

		nr_taken += nr_pages;
		nr_zone_taken[page_zonenum(page)] += nr_pages;
		move_to = dst;
move:
		list_move(&page->lru, move_to);
		trace_android_vh_del_page_from_lrulist(page, false, lru);
	}

	/*
	 * Splice any skipped pages to the start of the LRU list. Note that
	 * this disrupts the LRU order when reclaiming for lower zones but
	 * we cannot splice to the tail. If we did then the SWAP_CLUSTER_MAX
	 * scanning would soon rescan the same pages to skip and put the
	 * system at risk of premature OOM.
	 */
	if (!list_empty(&pages_skipped)) {
		int zid;

		list_splice(&pages_skipped, src);
		for (zid = 0; zid < MAX_NR_ZONES; zid++) {
			if (!nr_skipped[zid])
				continue;

			__count_zid_vm_events(PGSCAN_SKIP, zid, nr_skipped[zid]);
			skipped += nr_skipped[zid];
		}
	}
	*nr_scanned = total_scan;
	trace_mm_vmscan_lru_isolate(sc->reclaim_idx, sc->order, nr_to_scan,
				    total_scan, skipped, nr_taken,
				    sc->may_unmap ? 0 : ISOLATE_UNMAPPED, lru);
	update_lru_sizes(lruvec, lru, nr_zone_taken);
	return nr_taken;
}

/**
 * isolate_lru_page - tries to isolate a page from its LRU list
 * @page: page to isolate from its LRU list
 *
 * Isolates a @page from an LRU list, clears PageLRU and adjusts the
 * vmstat statistic corresponding to whatever LRU list the page was on.
 *
 * Returns 0 if the page was removed from an LRU list.
 * Returns -EBUSY if the page was not on an LRU list.
 *
 * The returned page will have PageLRU() cleared.  If it was found on
 * the active list, it will have PageActive set.  If it was found on
 * the unevictable list, it will have the PageUnevictable bit set. That flag
 * may need to be cleared by the caller before letting the page go.
 *
 * The vmstat statistic corresponding to the list on which the page was
 * found will be decremented.
 *
 * Restrictions:
 *
 * (1) Must be called with an elevated refcount on the page. This is a
 *     fundamental difference from isolate_lru_pages (which is called
 *     without a stable reference).
 * (2) the lru_lock must not be held.
 * (3) interrupts must be enabled.
 */
int isolate_lru_page(struct page *page)
{
	int ret = -EBUSY;

	VM_BUG_ON_PAGE(!page_count(page), page);
	WARN_RATELIMIT(PageTail(page), "trying to isolate tail page");

	if (TestClearPageLRU(page)) {
		struct lruvec *lruvec;

		get_page(page);
		lruvec = lock_page_lruvec_irq(page);
		del_page_from_lru_list(page, lruvec);
		unlock_page_lruvec_irq(lruvec);
		ret = 0;
	}

	return ret;
}

/*
 * A direct reclaimer may isolate SWAP_CLUSTER_MAX pages from the LRU list and
 * then get rescheduled. When there are massive number of tasks doing page
 * allocation, such sleeping direct reclaimers may keep piling up on each CPU,
 * the LRU list will go small and be scanned faster than necessary, leading to
 * unnecessary swapping, thrashing and OOM.
 */
static int too_many_isolated(struct pglist_data *pgdat, int file,
		struct scan_control *sc)
{
	unsigned long inactive, isolated;

	if (current_is_kswapd())
		return 0;

	if (!writeback_throttling_sane(sc))
		return 0;

	if (file) {
		inactive = node_page_state(pgdat, NR_INACTIVE_FILE);
		isolated = node_page_state(pgdat, NR_ISOLATED_FILE);
	} else {
		inactive = node_page_state(pgdat, NR_INACTIVE_ANON);
		isolated = node_page_state(pgdat, NR_ISOLATED_ANON);
	}

	/*
	 * GFP_NOIO/GFP_NOFS callers are allowed to isolate more pages, so they
	 * won't get blocked by normal direct-reclaimers, forming a circular
	 * deadlock.
	 */
	if ((sc->gfp_mask & (__GFP_IO | __GFP_FS)) == (__GFP_IO | __GFP_FS))
		inactive >>= 3;

	return isolated > inactive;
}

/*
 * move_pages_to_lru() moves pages from private @list to appropriate LRU list.
 * On return, @list is reused as a list of pages to be freed by the caller.
 *
 * Returns the number of pages moved to the given lruvec.
 */
static unsigned int move_pages_to_lru(struct lruvec *lruvec,
				      struct list_head *list)
{
	int nr_pages, nr_moved = 0;
	LIST_HEAD(pages_to_free);
	struct page *page;

	while (!list_empty(list)) {
		page = lru_to_page(list);
		VM_BUG_ON_PAGE(PageLRU(page), page);
		list_del(&page->lru);
		if (unlikely(!page_evictable(page))) {
			spin_unlock_irq(&lruvec->lru_lock);
			putback_lru_page(page);
			spin_lock_irq(&lruvec->lru_lock);
			continue;
		}

		/*
		 * The SetPageLRU needs to be kept here for list integrity.
		 * Otherwise:
		 *   #0 move_pages_to_lru             #1 release_pages
		 *   if !put_page_testzero
		 *				      if (put_page_testzero())
		 *				        !PageLRU //skip lru_lock
		 *     SetPageLRU()
		 *     list_add(&page->lru,)
		 *                                        list_add(&page->lru,)
		 */
		SetPageLRU(page);

		if (unlikely(put_page_testzero(page))) {
			__clear_page_lru_flags(page);

			if (unlikely(PageCompound(page))) {
				spin_unlock_irq(&lruvec->lru_lock);
				destroy_compound_page(page);
				spin_lock_irq(&lruvec->lru_lock);
			} else
				list_add(&page->lru, &pages_to_free);

			continue;
		}

		/*
		 * All pages were isolated from the same lruvec (and isolation
		 * inhibits memcg migration).
		 */
		VM_BUG_ON_PAGE(!page_matches_lruvec(page, lruvec), page);
		add_page_to_lru_list(page, lruvec);
		nr_pages = thp_nr_pages(page);
		nr_moved += nr_pages;
		if (PageActive(page))
			workingset_age_nonresident(lruvec, nr_pages);
	}

	/*
	 * To save our caller's stack, now use input list for pages to free.
	 */
	list_splice(&pages_to_free, list);

	return nr_moved;
}

/*
 * If a kernel thread (such as nfsd for loop-back mounts) services
 * a backing device by writing to the page cache it sets PF_LOCAL_THROTTLE.
 * In that case we should only throttle if the backing device it is
 * writing to is congested.  In other cases it is safe to throttle.
 */
static int current_may_throttle(void)
{
	return !(current->flags & PF_LOCAL_THROTTLE) ||
		current->backing_dev_info == NULL ||
		bdi_write_congested(current->backing_dev_info);
}

/*
 * shrink_inactive_list() is a helper for shrink_node().  It returns the number
 * of reclaimed pages
 */
static unsigned long
shrink_inactive_list(unsigned long nr_to_scan, struct lruvec *lruvec,
		     struct scan_control *sc, enum lru_list lru)
{
	LIST_HEAD(page_list);
	unsigned long nr_scanned;
	unsigned int nr_reclaimed = 0;
	unsigned long nr_taken;
	struct reclaim_stat stat;
	bool file = is_file_lru(lru);
	enum vm_event_item item;
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	bool stalled = false;
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	bool chp_reclaim = !!(sc->gfp_mask & POOL_USER_ALLOC_MASK);
#endif
	while (unlikely(too_many_isolated(pgdat, file, sc))) {
		if (stalled)
			return 0;

		/* wait a bit for the reclaimer. */
		msleep(100);
		stalled = true;

		/* We are about to die and free our memory. Return now. */
		if (fatal_signal_pending(current))
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
			return chp_reclaim ?  CHP_SWAP_CLUSTER_MAX : SWAP_CLUSTER_MAX;
#else
			return SWAP_CLUSTER_MAX;
#endif
	}
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	/* compound pages like cont_pte have got LRU drained in lru_cache_add */
	if (!chp_reclaim)
#endif
	lru_add_drain();

	spin_lock_irq(&lruvec->lru_lock);

	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &page_list,
				     &nr_scanned, sc, lru);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);
	item = current_is_kswapd() ? PGSCAN_KSWAPD : PGSCAN_DIRECT;
	if (!cgroup_reclaim(sc))
		__count_vm_events(item, nr_scanned);
	__count_memcg_events(lruvec_memcg(lruvec), item, nr_scanned);
	__count_vm_events(PGSCAN_ANON + file, nr_scanned);

	spin_unlock_irq(&lruvec->lru_lock);

	if (nr_taken == 0)
		return 0;

	nr_reclaimed = shrink_page_list(&page_list, pgdat, sc, &stat, false);
	trace_android_vh_handle_failed_page_trylock(&page_list);

	spin_lock_irq(&lruvec->lru_lock);
	move_pages_to_lru(lruvec, &page_list);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);
	item = current_is_kswapd() ? PGSTEAL_KSWAPD : PGSTEAL_DIRECT;
	if (!cgroup_reclaim(sc))
		__count_vm_events(item, nr_reclaimed);
	__count_memcg_events(lruvec_memcg(lruvec), item, nr_reclaimed);
	__count_vm_events(PGSTEAL_ANON + file, nr_reclaimed);
	spin_unlock_irq(&lruvec->lru_lock);

	lru_note_cost(lruvec, file, stat.nr_pageout);
	mem_cgroup_uncharge_list(&page_list);
	free_unref_page_list(&page_list);

	/*
	 * If dirty pages are scanned that are not queued for IO, it
	 * implies that flushers are not doing their job. This can
	 * happen when memory pressure pushes dirty pages to the end of
	 * the LRU before the dirty limits are breached and the dirty
	 * data has expired. It can also happen when the proportion of
	 * dirty pages grows not through writes but through memory
	 * pressure reclaiming all the clean cache. And in some cases,
	 * the flushers simply cannot keep up with the allocation
	 * rate. Nudge the flusher threads in case they are asleep.
	 */
	if (stat.nr_unqueued_dirty == nr_taken)
		wakeup_flusher_threads(WB_REASON_VMSCAN);

	sc->nr.dirty += stat.nr_dirty;
	sc->nr.congested += stat.nr_congested;
	sc->nr.unqueued_dirty += stat.nr_unqueued_dirty;
	sc->nr.writeback += stat.nr_writeback;
	sc->nr.immediate += stat.nr_immediate;
	sc->nr.taken += nr_taken;
	if (file)
		sc->nr.file_taken += nr_taken;

	trace_mm_vmscan_lru_shrink_inactive(pgdat->node_id,
			nr_scanned, nr_reclaimed, &stat, sc->priority, file);
	return nr_reclaimed;
}

/*
 * shrink_active_list() moves pages from the active LRU to the inactive LRU.
 *
 * We move them the other way if the page is referenced by one or more
 * processes.
 *
 * If the pages are mostly unmapped, the processing is fast and it is
 * appropriate to hold lru_lock across the whole operation.  But if
 * the pages are mapped, the processing is slow (page_referenced()), so
 * we should drop lru_lock around each page.  It's impossible to balance
 * this, so instead we remove the pages from the LRU while processing them.
 * It is safe to rely on PG_active against the non-LRU pages in here because
 * nobody will play with that bit on a non-LRU page.
 *
 * The downside is that we have to touch page->_refcount against each page.
 * But we had to alter page->flags anyway.
 */
static void shrink_active_list(unsigned long nr_to_scan,
			       struct lruvec *lruvec,
			       struct scan_control *sc,
			       enum lru_list lru)
{
	unsigned long nr_taken;
	unsigned long nr_scanned;
	unsigned long vm_flags;
	LIST_HEAD(l_hold);	/* The pages which were snipped off */
	LIST_HEAD(l_active);
	LIST_HEAD(l_inactive);
	struct page *page;
	unsigned nr_deactivate, nr_activate;
	unsigned nr_rotated = 0;
	int file = is_file_lru(lru);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	bool bypass = false;
	bool should_protect = false;

	lru_add_drain();

	spin_lock_irq(&lruvec->lru_lock);

	nr_taken = isolate_lru_pages(nr_to_scan, lruvec, &l_hold,
				     &nr_scanned, sc, lru);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);

	if (!cgroup_reclaim(sc))
		__count_vm_events(PGREFILL, nr_scanned);
	__count_memcg_events(lruvec_memcg(lruvec), PGREFILL, nr_scanned);

	spin_unlock_irq(&lruvec->lru_lock);

	while (!list_empty(&l_hold)) {
		cond_resched();
		page = lru_to_page(&l_hold);
		list_del(&page->lru);

		if (unlikely(!page_evictable(page))) {
			putback_lru_page(page);
			continue;
		}

		if (unlikely(buffer_heads_over_limit)) {
			if (page_has_private(page) && trylock_page(page)) {
				if (page_has_private(page))
					try_to_release_page(page, 0);
				unlock_page(page);
			}
		}

		trace_android_vh_page_should_be_protected(page, &should_protect);
		if (unlikely(should_protect)) {
			nr_rotated += thp_nr_pages(page);
			list_add(&page->lru, &l_active);
			continue;
		}

		trace_android_vh_page_referenced_check_bypass(page, nr_to_scan, lru, &bypass);

#ifdef CONFIG_CONT_PTE_HUGEPAGE
		if (ContPteHugePageSkipMassiveMapped(page))
			bypass = 1;

		/* DoubleMap page don't make page_referenced */
		if (ContPteHugePageHead(page) && PageDoubleMap(page))
			bypass = 1;
#endif
		if (bypass)
			goto skip_page_referenced;
		trace_android_vh_page_trylock_set(page);
		/* Referenced or rmap lock contention: rotate */
		if (page_referenced(page, 0, sc->target_mem_cgroup,
				     &vm_flags) != 0) {
			/*
			 * Identify referenced, file-backed active pages and
			 * give them one more trip around the active list. So
			 * that executable code get better chances to stay in
			 * memory under moderate memory pressure.  Anon pages
			 * are not likely to be evicted by use-once streaming
			 * IO, plus JVM can create lots of anon VM_EXEC pages,
			 * so we ignore them here.
			 */
			if ((vm_flags & VM_EXEC) && page_is_file_lru(page)) {
				trace_android_vh_page_trylock_clear(page);
				nr_rotated += thp_nr_pages(page);
				list_add(&page->lru, &l_active);
				continue;
			}
		}
		trace_android_vh_page_trylock_clear(page);
skip_page_referenced:
		ClearPageActive(page);	/* we are de-activating */
		SetPageWorkingset(page);
		list_add(&page->lru, &l_inactive);
	}

	/*
	 * Move pages back to the lru list.
	 */
	spin_lock_irq(&lruvec->lru_lock);

	nr_activate = move_pages_to_lru(lruvec, &l_active);
	nr_deactivate = move_pages_to_lru(lruvec, &l_inactive);
	/* Keep all free pages in l_active list */
	list_splice(&l_inactive, &l_active);

	__count_vm_events(PGDEACTIVATE, nr_deactivate);
	__count_memcg_events(lruvec_memcg(lruvec), PGDEACTIVATE, nr_deactivate);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);
	spin_unlock_irq(&lruvec->lru_lock);

	mem_cgroup_uncharge_list(&l_active);
	free_unref_page_list(&l_active);
	trace_mm_vmscan_lru_shrink_active(pgdat->node_id, nr_taken, nr_activate,
			nr_deactivate, nr_rotated, sc->priority, file);
}

unsigned long reclaim_pages(struct list_head *page_list)
{
	int nid = NUMA_NO_NODE;
	unsigned int nr_reclaimed = 0;
	LIST_HEAD(node_page_list);
	struct reclaim_stat dummy_stat;
	struct page *page;
	unsigned int noreclaim_flag;
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = 1,
		.no_demotion = 1,
	};

	noreclaim_flag = memalloc_noreclaim_save();

	while (!list_empty(page_list)) {
		page = lru_to_page(page_list);
		if (nid == NUMA_NO_NODE) {
			nid = page_to_nid(page);
			INIT_LIST_HEAD(&node_page_list);
		}

		if (nid == page_to_nid(page)) {
			ClearPageActive(page);
			list_move(&page->lru, &node_page_list);
			continue;
		}

		nr_reclaimed += shrink_page_list(&node_page_list,
						NODE_DATA(nid),
						&sc, &dummy_stat, false);
		while (!list_empty(&node_page_list)) {
			page = lru_to_page(&node_page_list);
			list_del(&page->lru);
			putback_lru_page(page);
		}

		nid = NUMA_NO_NODE;
	}

	if (!list_empty(&node_page_list)) {
		nr_reclaimed += shrink_page_list(&node_page_list,
						NODE_DATA(nid),
						&sc, &dummy_stat, false);
		while (!list_empty(&node_page_list)) {
			page = lru_to_page(&node_page_list);
			list_del(&page->lru);
			putback_lru_page(page);
		}
	}

	memalloc_noreclaim_restore(noreclaim_flag);

	return nr_reclaimed;
}
EXPORT_SYMBOL_GPL(reclaim_pages);

static unsigned long shrink_list(enum lru_list lru, unsigned long nr_to_scan,
				 struct lruvec *lruvec, struct scan_control *sc)
{
	if (is_active_lru(lru)) {
		if (sc->may_deactivate & (1 << is_file_lru(lru)))
			shrink_active_list(nr_to_scan, lruvec, sc, lru);
		else
			sc->skipped_deactivate = 1;
		return 0;
	}

	return shrink_inactive_list(nr_to_scan, lruvec, sc, lru);
}

/*
 * The inactive anon list should be small enough that the VM never has
 * to do too much work.
 *
 * The inactive file list should be small enough to leave most memory
 * to the established workingset on the scan-resistant active list,
 * but large enough to avoid thrashing the aggregate readahead window.
 *
 * Both inactive lists should also be large enough that each inactive
 * page has a chance to be referenced again before it is reclaimed.
 *
 * If that fails and refaulting is observed, the inactive list grows.
 *
 * The inactive_ratio is the target ratio of ACTIVE to INACTIVE pages
 * on this LRU, maintained by the pageout code. An inactive_ratio
 * of 3 means 3:1 or 25% of the pages are kept on the inactive list.
 *
 * total     target    max
 * memory    ratio     inactive
 * -------------------------------------
 *   10MB       1         5MB
 *  100MB       1        50MB
 *    1GB       3       250MB
 *   10GB      10       0.9GB
 *  100GB      31         3GB
 *    1TB     101        10GB
 *   10TB     320        32GB
 */
static bool inactive_is_low(struct lruvec *lruvec, enum lru_list inactive_lru)
{
	enum lru_list active_lru = inactive_lru + LRU_ACTIVE;
	unsigned long inactive, active;
	unsigned long inactive_ratio;
	unsigned long gb;

#if defined(CONFIG_CONT_PTE_HUGEPAGE) &&  CONFIG_CONT_PTE_HUGEPAGE_LRU
	/*
	 * NOTE: Since lruvec_stat statistics are delayed,
	 * we use lru_zone_size to calculate the size of
	 * lru here!
	 */
	int zid;

	inactive = active = 0;

	if (!is_chp_lruvec(lruvec)) {
		struct mem_cgroup_per_node *mz;

		mz = container_of(lruvec, struct mem_cgroup_per_node, lruvec);

		for (zid = 0; zid < MAX_NR_ZONES; zid++) {
			inactive +=  READ_ONCE(mz->lru_zone_size[zid][NR_LRU_BASE + inactive_lru]);
			active  += READ_ONCE(mz->lru_zone_size[zid][NR_LRU_BASE + active_lru]);
		}
	} else {
		struct chp_lruvec *chp_lruvec;

		chp_lruvec = container_of(lruvec, struct chp_lruvec, lruvec);

		for (zid = 0; zid < MAX_NR_ZONES; zid++) {
			inactive +=  READ_ONCE(chp_lruvec->lru_zone_size[zid][NR_LRU_BASE + inactive_lru]);
			active  += READ_ONCE(chp_lruvec->lru_zone_size[zid][NR_LRU_BASE + active_lru]);
		}
	}
#else
	inactive = lruvec_page_state(lruvec, NR_LRU_BASE + inactive_lru);
	active = lruvec_page_state(lruvec, NR_LRU_BASE + active_lru);
#endif

	gb = (inactive + active) >> (30 - PAGE_SHIFT);
	if (gb)
		inactive_ratio = int_sqrt(10 * gb);
	else
		inactive_ratio = 1;

	trace_android_vh_tune_inactive_ratio(&inactive_ratio, is_file_lru(inactive_lru));

	return inactive * inactive_ratio < active;
}

enum scan_balance {
	SCAN_EQUAL,
	SCAN_FRACT,
	SCAN_ANON,
	SCAN_FILE,
};

static void prepare_scan_count(pg_data_t *pgdat, struct scan_control *sc)
{
	unsigned long file;
	struct lruvec *target_lruvec;

	if (lru_gen_enabled())
		return;

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
	if (sc->gfp_mask & POOL_USER_ALLOC_MASK)
		target_lruvec = mem_cgroup_chp_lruvec(sc->target_mem_cgroup, pgdat);
	else
#endif
		target_lruvec = mem_cgroup_lruvec(sc->target_mem_cgroup, pgdat);

	/*
	 * Flush the memory cgroup stats, so that we read accurate per-memcg
	 * lruvec stats for heuristics.
	 */
	mem_cgroup_flush_stats();

	/*
	 * Determine the scan balance between anon and file LRUs.
	 */
	spin_lock_irq(&target_lruvec->lru_lock);
	sc->anon_cost = target_lruvec->anon_cost;
	sc->file_cost = target_lruvec->file_cost;
	spin_unlock_irq(&target_lruvec->lru_lock);

	/*
	 * Target desirable inactive:active list ratios for the anon
	 * and file LRU lists.
	 */
	if (!sc->force_deactivate) {
		unsigned long refaults;

		refaults = lruvec_page_state(target_lruvec,
				WORKINGSET_ACTIVATE_ANON);
		if (refaults != target_lruvec->refaults[0] ||
			inactive_is_low(target_lruvec, LRU_INACTIVE_ANON))
			sc->may_deactivate |= DEACTIVATE_ANON;
		else
			sc->may_deactivate &= ~DEACTIVATE_ANON;

		/*
		 * When refaults are being observed, it means a new
		 * workingset is being established. Deactivate to get
		 * rid of any stale active pages quickly.
		 */
		refaults = lruvec_page_state(target_lruvec,
				WORKINGSET_ACTIVATE_FILE);
		if (refaults != target_lruvec->refaults[1] ||
		    inactive_is_low(target_lruvec, LRU_INACTIVE_FILE))
			sc->may_deactivate |= DEACTIVATE_FILE;
		else
			sc->may_deactivate &= ~DEACTIVATE_FILE;
	} else
		sc->may_deactivate = DEACTIVATE_ANON | DEACTIVATE_FILE;

	/*
	 * If we have plenty of inactive file pages that aren't
	 * thrashing, try to reclaim those first before touching
	 * anonymous pages.
	 */
	file = lruvec_page_state(target_lruvec, NR_INACTIVE_FILE);
	if (file >> sc->priority && !(sc->may_deactivate & DEACTIVATE_FILE))
		sc->cache_trim_mode = 1;
	else
		sc->cache_trim_mode = 0;

	/*
	 * Prevent the reclaimer from falling into the cache trap: as
	 * cache pages start out inactive, every cache fault will tip
	 * the scan balance towards the file LRU.  And as the file LRU
	 * shrinks, so does the window for rotation from references.
	 * This means we have a runaway feedback loop where a tiny
	 * thrashing file LRU becomes infinitely more attractive than
	 * anon pages.  Try to detect this based on file LRU size.
	 */
	if (!cgroup_reclaim(sc)) {
		unsigned long total_high_wmark = 0;
		unsigned long free, anon;
		int z;

		free = sum_zone_node_page_state(pgdat->node_id, NR_FREE_PAGES);
		file = node_page_state(pgdat, NR_ACTIVE_FILE) +
			   node_page_state(pgdat, NR_INACTIVE_FILE);

		for (z = 0; z < MAX_NR_ZONES; z++) {
			struct zone *zone = &pgdat->node_zones[z];

			if (!managed_zone(zone))
				continue;

			total_high_wmark += high_wmark_pages(zone);
		}

		/*
		 * Consider anon: if that's low too, this isn't a
		 * runaway file reclaim problem, but rather just
		 * extreme pressure. Reclaim as per usual then.
		 */
		anon = node_page_state(pgdat, NR_INACTIVE_ANON);

		sc->file_is_tiny =
			file + free <= total_high_wmark &&
			!(sc->may_deactivate & DEACTIVATE_ANON) &&
			anon >> sc->priority;
	}
}

/*
 * Determine how aggressively the anon and file LRU lists should be
 * scanned.  The relative value of each set of LRU lists is determined
 * by looking at the fraction of the pages scanned we did rotate back
 * onto the active list instead of evict.
 *
 * nr[0] = anon inactive pages to scan; nr[1] = anon active pages to scan
 * nr[2] = file inactive pages to scan; nr[3] = file active pages to scan
 */
static void get_scan_count(struct lruvec *lruvec, struct scan_control *sc,
			   unsigned long *nr)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	unsigned long anon_cost, file_cost, total_cost;
	int swappiness = mem_cgroup_swappiness(memcg);
	u64 fraction[ANON_AND_FILE];
	u64 denominator = 0;	/* gcc */
	enum scan_balance scan_balance;
	unsigned long ap, fp;
	enum lru_list lru;
	bool balance_anon_file_reclaim = false;
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	bool chp_reclaim = !!(sc->gfp_mask & POOL_USER_ALLOC_MASK);
#endif

	/* If we have no swap space, do not bother scanning anon pages. */
	if (!sc->may_swap || !can_reclaim_anon_pages(memcg, pgdat->node_id, sc)) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	trace_android_vh_tune_swappiness(&swappiness);

	/*
	 * Global reclaim will swap to prevent OOM even with no
	 * swappiness, but memcg users want to use this knob to
	 * disable swapping for individual groups completely when
	 * using the memory controller's swap limit feature would be
	 * too expensive.
	 */
	if (cgroup_reclaim(sc) && !swappiness) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	/*
	 * Do not apply any pressure balancing cleverness when the
	 * system is close to OOM, scan both anon and file equally
	 * (unless the swappiness setting disagrees with swapping).
	 */
	if (!sc->priority && swappiness) {
		scan_balance = SCAN_EQUAL;
		goto out;
	}

	/*
	 * If the system is almost out of file pages, force-scan anon.
	 */
	if (sc->file_is_tiny) {
		scan_balance = SCAN_ANON;
		goto out;
	}

	trace_android_rvh_set_balance_anon_file_reclaim(&balance_anon_file_reclaim);

	/*
	 * If there is enough inactive page cache, we do not reclaim
	 * anything from the anonymous working right now. But when balancing
	 * anon and page cache files for reclaim, allow swapping of anon pages
	 * even if there are a number of inactive file cache pages.
	 */
	if (!balance_anon_file_reclaim && sc->cache_trim_mode) {
		scan_balance = SCAN_FILE;
		goto out;
	}

	scan_balance = SCAN_FRACT;
	/*
	 * Calculate the pressure balance between anon and file pages.
	 *
	 * The amount of pressure we put on each LRU is inversely
	 * proportional to the cost of reclaiming each list, as
	 * determined by the share of pages that are refaulting, times
	 * the relative IO cost of bringing back a swapped out
	 * anonymous page vs reloading a filesystem page (swappiness).
	 *
	 * Although we limit that influence to ensure no list gets
	 * left behind completely: at least a third of the pressure is
	 * applied, before swappiness.
	 *
	 * With swappiness at 100, anon and file have equal IO cost.
	 */
	total_cost = sc->anon_cost + sc->file_cost;
	anon_cost = total_cost + sc->anon_cost;
	file_cost = total_cost + sc->file_cost;
	total_cost = anon_cost + file_cost;

	ap = swappiness * (total_cost + 1);
	ap /= anon_cost + 1;

	fp = (200 - swappiness) * (total_cost + 1);
	fp /= file_cost + 1;

	fraction[0] = ap;
	fraction[1] = fp;
	denominator = ap + fp;
out:
	trace_android_vh_tune_scan_type((char *)(&scan_balance));
	trace_android_vh_tune_memcg_scan_type(memcg, (char *)(&scan_balance));
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	/*
	 * When the file hugepages is small,
	 * the file hugepage is not reclaimed
	 */
	if (chp_reclaim) {
		if (global_node_page_state(NR_FILE_THPS) * HPAGE_CONT_PTE_NR < POOL_FILE_HUGEPAGES_LIMIT) {
			pr_debug_ratelimited("@%s:%d filehugepages:%ld limit:%ld cgroup_reclaim:%d @\n",
					     __func__, __LINE__,
					     global_node_page_state(NR_FILE_THPS) * HPAGE_CONT_PTE_NR,
					     POOL_FILE_HUGEPAGES_LIMIT,
					     cgroup_reclaim(sc));
			scan_balance = SCAN_ANON;
		}
	}
#endif
	for_each_evictable_lru(lru) {
		int file = is_file_lru(lru);
		unsigned long lruvec_size;
		unsigned long low, min;
		unsigned long scan;

		lruvec_size = lruvec_lru_size(lruvec, lru, sc->reclaim_idx);
		mem_cgroup_protection(sc->target_mem_cgroup, memcg,
				      &min, &low);

		if (min || low) {
			/*
			 * Scale a cgroup's reclaim pressure by proportioning
			 * its current usage to its memory.low or memory.min
			 * setting.
			 *
			 * This is important, as otherwise scanning aggression
			 * becomes extremely binary -- from nothing as we
			 * approach the memory protection threshold, to totally
			 * nominal as we exceed it.  This results in requiring
			 * setting extremely liberal protection thresholds. It
			 * also means we simply get no protection at all if we
			 * set it too low, which is not ideal.
			 *
			 * If there is any protection in place, we reduce scan
			 * pressure by how much of the total memory used is
			 * within protection thresholds.
			 *
			 * There is one special case: in the first reclaim pass,
			 * we skip over all groups that are within their low
			 * protection. If that fails to reclaim enough pages to
			 * satisfy the reclaim goal, we come back and override
			 * the best-effort low protection. However, we still
			 * ideally want to honor how well-behaved groups are in
			 * that case instead of simply punishing them all
			 * equally. As such, we reclaim them based on how much
			 * memory they are using, reducing the scan pressure
			 * again by how much of the total memory used is under
			 * hard protection.
			 */
			unsigned long cgroup_size = mem_cgroup_size(memcg);
			unsigned long protection;

			/* memory.low scaling, make sure we retry before OOM */
			if (!sc->memcg_low_reclaim && low > min) {
				protection = low;
				sc->memcg_low_skipped = 1;
			} else {
				protection = min;
			}

			/* Avoid TOCTOU with earlier protection check */
			cgroup_size = max(cgroup_size, protection);

			scan = lruvec_size - lruvec_size * protection /
				(cgroup_size + 1);

			/*
			 * Minimally target SWAP_CLUSTER_MAX pages to keep
			 * reclaim moving forwards, avoiding decrementing
			 * sc->priority further than desirable.
			 */
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
			if (sc->gfp_mask & POOL_USER_ALLOC_MASK)
				scan = max(scan, CHP_SWAP_CLUSTER_MAX);
			else
#endif
				scan = max(scan, SWAP_CLUSTER_MAX);
		} else {
			scan = lruvec_size;
		}

		scan >>= sc->priority;

		/*
		 * If the cgroup's already been deleted, make sure to
		 * scrape out the remaining cache.
		 */
		if (!scan && !mem_cgroup_online(memcg)) {
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
			if (sc->gfp_mask & POOL_USER_ALLOC_MASK)
				scan = min(lruvec_size, CHP_SWAP_CLUSTER_MAX);
			else
#endif
				scan = min(lruvec_size, SWAP_CLUSTER_MAX);
			}

		switch (scan_balance) {
		case SCAN_EQUAL:
			/* Scan lists relative to size */
			break;
		case SCAN_FRACT:
			/*
			 * Scan types proportional to swappiness and
			 * their relative recent reclaim efficiency.
			 * Make sure we don't miss the last page on
			 * the offlined memory cgroups because of a
			 * round-off error.
			 */
			scan = mem_cgroup_online(memcg) ?
			       div64_u64(scan * fraction[file], denominator) :
			       DIV64_U64_ROUND_UP(scan * fraction[file],
						  denominator);
			break;
		case SCAN_FILE:
		case SCAN_ANON:
			/* Scan one type exclusively */
			if ((scan_balance == SCAN_FILE) != file)
				scan = 0;
			break;
		default:
			/* Look ma, no brain */
			BUG();
		}

		nr[lru] = scan;
	}
}

/*
 * Anonymous LRU management is a waste if there is
 * ultimately no way to reclaim the memory.
 */
static bool can_age_anon_pages(struct pglist_data *pgdat,
			       struct scan_control *sc)
{
	/* Aging the anon LRU is valuable if swap is present: */
	if (total_swap_pages > 0)
		return true;

	/* Also valuable if anon pages can be demoted: */
	return can_demote(pgdat->node_id, sc);
}

#ifdef CONFIG_LRU_GEN

#if defined(CONFIG_LRU_GEN_ENABLED) && !defined(CONFIG_CONT_PTE_HUGEPAGE)
DEFINE_STATIC_KEY_ARRAY_TRUE(lru_gen_caps, NR_LRU_GEN_CAPS);
#define get_cap(cap)	static_branch_likely(&lru_gen_caps[cap])
#else
DEFINE_STATIC_KEY_ARRAY_FALSE(lru_gen_caps, NR_LRU_GEN_CAPS);
#define get_cap(cap)	static_branch_unlikely(&lru_gen_caps[cap])
#endif

/******************************************************************************
 *                          shorthand helpers
 ******************************************************************************/

#define LRU_REFS_FLAGS	(BIT(PG_referenced) | BIT(PG_workingset))

#define DEFINE_MAX_SEQ(lruvec)						\
	unsigned long max_seq = READ_ONCE((lruvec)->lrugen.max_seq)

#define DEFINE_MIN_SEQ(lruvec)						\
	unsigned long min_seq[ANON_AND_FILE] = {			\
		READ_ONCE((lruvec)->lrugen.min_seq[LRU_GEN_ANON]),	\
		READ_ONCE((lruvec)->lrugen.min_seq[LRU_GEN_FILE]),	\
	}

#define for_each_gen_type_zone(gen, type, zone)				\
	for ((gen) = 0; (gen) < MAX_NR_GENS; (gen)++)			\
		for ((type) = 0; (type) < ANON_AND_FILE; (type)++)	\
			for ((zone) = 0; (zone) < MAX_NR_ZONES; (zone)++)

static struct lruvec *get_lruvec(struct mem_cgroup *memcg, int nid)
{
	struct pglist_data *pgdat = NODE_DATA(nid);

#ifdef CONFIG_MEMCG
	if (memcg) {
		struct lruvec *lruvec = &memcg->nodeinfo[nid]->lruvec;

		/* for hotadd_new_pgdat() */
		if (!lruvec->pgdat)
			lruvec->pgdat = pgdat;

		return lruvec;
	}
#endif
	VM_WARN_ON_ONCE(!mem_cgroup_disabled());

	return pgdat ? &pgdat->__lruvec : NULL;
}

static int get_swappiness(struct lruvec *lruvec, struct scan_control *sc)
{
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	if (!can_demote(pgdat->node_id, sc) &&
		mem_cgroup_get_nr_swap_pages(memcg) <= 0)
		return 0;

	return mem_cgroup_swappiness(memcg);
}

static int get_nr_gens(struct lruvec *lruvec, int type)
{
	return lruvec->lrugen.max_seq - lruvec->lrugen.min_seq[type] + 1;
}

static bool __maybe_unused seq_is_valid(struct lruvec *lruvec)
{
	/* see the comment on lru_gen_struct */
	return get_nr_gens(lruvec, LRU_GEN_FILE) >= MIN_NR_GENS &&
	       get_nr_gens(lruvec, LRU_GEN_FILE) <= get_nr_gens(lruvec, LRU_GEN_ANON) &&
	       get_nr_gens(lruvec, LRU_GEN_ANON) <= MAX_NR_GENS;
}

/******************************************************************************
 *                          mm_struct list
 ******************************************************************************/

static struct lru_gen_mm_list *get_mm_list(struct mem_cgroup *memcg)
{
	static struct lru_gen_mm_list mm_list = {
		.fifo = LIST_HEAD_INIT(mm_list.fifo),
		.lock = __SPIN_LOCK_UNLOCKED(mm_list.lock),
	};

#ifdef CONFIG_MEMCG
	if (memcg)
		return &memcg->mm_list;
#endif
	VM_WARN_ON_ONCE(!mem_cgroup_disabled());

	return &mm_list;
}

void lru_gen_add_mm(struct mm_struct *mm)
{
	int nid;
	struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
	struct lru_gen_mm_list *mm_list = get_mm_list(memcg);

	VM_WARN_ON_ONCE(!list_empty(&mm->lru_gen.list));
#ifdef CONFIG_MEMCG
	VM_WARN_ON_ONCE(mm->lru_gen.memcg);
	mm->lru_gen.memcg = memcg;
#endif
	spin_lock(&mm_list->lock);

	for_each_node_state(nid, N_MEMORY) {
		struct lruvec *lruvec = get_lruvec(memcg, nid);

		if (!lruvec)
			continue;

		/* the first addition since the last iteration */
		if (lruvec->mm_state.tail == &mm_list->fifo)
			lruvec->mm_state.tail = &mm->lru_gen.list;
	}

	list_add_tail(&mm->lru_gen.list, &mm_list->fifo);

	spin_unlock(&mm_list->lock);
}

void lru_gen_del_mm(struct mm_struct *mm)
{
	int nid;
	struct lru_gen_mm_list *mm_list;
	struct mem_cgroup *memcg = NULL;

	if (list_empty(&mm->lru_gen.list))
		return;

#ifdef CONFIG_MEMCG
	memcg = mm->lru_gen.memcg;
#endif
	mm_list = get_mm_list(memcg);

	spin_lock(&mm_list->lock);

	for_each_node(nid) {
		struct lruvec *lruvec = get_lruvec(memcg, nid);

		if (!lruvec)
			continue;

		/* where the current iteration continues after */
		if (lruvec->mm_state.head == &mm->lru_gen.list)
			lruvec->mm_state.head = lruvec->mm_state.head->prev;

		/* where the last iteration ended before */
		if (lruvec->mm_state.tail == &mm->lru_gen.list)
			lruvec->mm_state.tail = lruvec->mm_state.tail->next;
	}

	list_del_init(&mm->lru_gen.list);

	spin_unlock(&mm_list->lock);

#ifdef CONFIG_MEMCG
	mem_cgroup_put(mm->lru_gen.memcg);
	mm->lru_gen.memcg = NULL;
#endif
}

#ifdef CONFIG_MEMCG
void lru_gen_migrate_mm(struct mm_struct *mm)
{
	struct mem_cgroup *memcg;
	struct task_struct *task = rcu_dereference_protected(mm->owner, true);

	VM_WARN_ON_ONCE(task->mm != mm);
	lockdep_assert_held(&task->alloc_lock);

	/* for mm_update_next_owner() */
	if (mem_cgroup_disabled())
		return;

	/* migration can happen before addition */
	if (!mm->lru_gen.memcg)
		return;

	rcu_read_lock();
	memcg = mem_cgroup_from_task(task);
	rcu_read_unlock();
	if (memcg == mm->lru_gen.memcg)
		return;

	VM_WARN_ON_ONCE(list_empty(&mm->lru_gen.list));

	lru_gen_del_mm(mm);
	lru_gen_add_mm(mm);
}
#endif

/*
 * Bloom filters with m=1<<15, k=2 and the false positive rates of ~1/5 when
 * n=10,000 and ~1/2 when n=20,000, where, conventionally, m is the number of
 * bits in a bitmap, k is the number of hash functions and n is the number of
 * inserted items.
 *
 * Page table walkers use one of the two filters to reduce their search space.
 * To get rid of non-leaf entries that no longer have enough leaf entries, the
 * aging uses the double-buffering technique to flip to the other filter each
 * time it produces a new generation. For non-leaf entries that have enough
 * leaf entries, the aging carries them over to the next generation in
 * walk_pmd_range(); the eviction also report them when walking the rmap
 * in lru_gen_look_around().
 *
 * For future optimizations:
 * 1. It's not necessary to keep both filters all the time. The spare one can be
 *    freed after the RCU grace period and reallocated if needed again.
 * 2. And when reallocating, it's worth scaling its size according to the number
 *    of inserted entries in the other filter, to reduce the memory overhead on
 *    small systems and false positives on large systems.
 * 3. Jenkins' hash function is an alternative to Knuth's.
 */
#define BLOOM_FILTER_SHIFT	15

static inline int filter_gen_from_seq(unsigned long seq)
{
	return seq % NR_BLOOM_FILTERS;
}

static void get_item_key(void *item, int *key)
{
	u32 hash = hash_ptr(item, BLOOM_FILTER_SHIFT * 2);

	BUILD_BUG_ON(BLOOM_FILTER_SHIFT * 2 > BITS_PER_TYPE(u32));

	key[0] = hash & (BIT(BLOOM_FILTER_SHIFT) - 1);
	key[1] = hash >> BLOOM_FILTER_SHIFT;
}

static void reset_bloom_filter(struct lruvec *lruvec, unsigned long seq)
{
	unsigned long *filter;
	int gen = filter_gen_from_seq(seq);

	filter = lruvec->mm_state.filters[gen];
	if (filter) {
		bitmap_clear(filter, 0, BIT(BLOOM_FILTER_SHIFT));
		return;
	}

	filter = bitmap_zalloc(BIT(BLOOM_FILTER_SHIFT),
			       __GFP_HIGH | __GFP_NOMEMALLOC | __GFP_NOWARN);
	WRITE_ONCE(lruvec->mm_state.filters[gen], filter);
}

static void update_bloom_filter(struct lruvec *lruvec, unsigned long seq, void *item)
{
	int key[2];
	unsigned long *filter;
	int gen = filter_gen_from_seq(seq);

	filter = READ_ONCE(lruvec->mm_state.filters[gen]);
	if (!filter)
		return;

	get_item_key(item, key);

	if (!test_bit(key[0], filter))
		set_bit(key[0], filter);
	if (!test_bit(key[1], filter))
		set_bit(key[1], filter);
}

static bool test_bloom_filter(struct lruvec *lruvec, unsigned long seq, void *item)
{
	int key[2];
	unsigned long *filter;
	int gen = filter_gen_from_seq(seq);

	filter = READ_ONCE(lruvec->mm_state.filters[gen]);
	if (!filter)
		return true;

	get_item_key(item, key);

	return test_bit(key[0], filter) && test_bit(key[1], filter);
}

static void reset_mm_stats(struct lruvec *lruvec, struct lru_gen_mm_walk *walk, bool last)
{
	int i;
	int hist;

	lockdep_assert_held(&get_mm_list(lruvec_memcg(lruvec))->lock);

	if (walk) {
		hist = lru_hist_from_seq(walk->max_seq);

		for (i = 0; i < NR_MM_STATS; i++) {
			WRITE_ONCE(lruvec->mm_state.stats[hist][i],
				   lruvec->mm_state.stats[hist][i] + walk->mm_stats[i]);
			walk->mm_stats[i] = 0;
		}
	}

	if (NR_HIST_GENS > 1 && last) {
		hist = lru_hist_from_seq(lruvec->mm_state.seq + 1);

		for (i = 0; i < NR_MM_STATS; i++)
			WRITE_ONCE(lruvec->mm_state.stats[hist][i], 0);
	}
}

static bool should_skip_mm(struct mm_struct *mm, struct lru_gen_mm_walk *walk)
{
	int type;
	unsigned long size = 0;
	struct pglist_data *pgdat = lruvec_pgdat(walk->lruvec);
	int key = pgdat->node_id;

	if (!walk->full_scan && !node_isset(key, mm->lru_gen.nodes))
		return true;

	node_clear(key, mm->lru_gen.nodes);

	for (type = !walk->can_swap; type < ANON_AND_FILE; type++) {
		size += type ? get_mm_counter(mm, MM_FILEPAGES) :
			       get_mm_counter(mm, MM_ANONPAGES) +
			       get_mm_counter(mm, MM_SHMEMPAGES);
	}

	if (size < MIN_LRU_BATCH)
		return true;

	return !mmget_not_zero(mm);
}

static bool iterate_mm_list(struct lruvec *lruvec, struct lru_gen_mm_walk *walk,
			    struct mm_struct **iter)
{
	bool first = false;
	bool last = false;
	struct mm_struct *mm = NULL;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	struct lru_gen_mm_list *mm_list = get_mm_list(memcg);
	struct lru_gen_mm_state *mm_state = &lruvec->mm_state;

	/*
	 * mm_state->seq is incremented after each iteration of mm_list. There
	 * are three interesting cases for this page table walker:
	 * 1. It tries to start a new iteration with a stale max_seq: there is
	 *    nothing left to do.
	 * 2. It started the next iteration: it needs to reset the Bloom filter
	 *    so that a fresh set of PTE tables can be recorded.
	 * 3. It ended the current iteration: it needs to reset the mm stats
	 *    counters and tell its caller to increment max_seq.
	 */
	spin_lock(&mm_list->lock);

	VM_WARN_ON_ONCE(mm_state->seq + 1 < walk->max_seq);

	if (walk->max_seq <= mm_state->seq)
		goto done;

	if (!mm_state->head)
		mm_state->head = &mm_list->fifo;

	if (mm_state->head == &mm_list->fifo)
		first = true;

	do {
		mm_state->head = mm_state->head->next;
		if (mm_state->head == &mm_list->fifo) {
			WRITE_ONCE(mm_state->seq, mm_state->seq + 1);
			last = true;
			break;
		}

		/* force scan for those added after the last iteration */
		if (!mm_state->tail || mm_state->tail == mm_state->head) {
			mm_state->tail = mm_state->head->next;
			walk->full_scan = true;
		}

		mm = list_entry(mm_state->head, struct mm_struct, lru_gen.list);
		if (should_skip_mm(mm, walk))
			mm = NULL;
	} while (!mm);
done:
	if (*iter || last)
		reset_mm_stats(lruvec, walk, last);

	spin_unlock(&mm_list->lock);

	if (mm && first)
		reset_bloom_filter(lruvec, walk->max_seq + 1);

	if (*iter)
		mmput_async(*iter);

	*iter = mm;

	return last;
}

static bool iterate_mm_list_nowalk(struct lruvec *lruvec, unsigned long max_seq)
{
	bool success = false;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	struct lru_gen_mm_list *mm_list = get_mm_list(memcg);
	struct lru_gen_mm_state *mm_state = &lruvec->mm_state;

	spin_lock(&mm_list->lock);

	VM_WARN_ON_ONCE(mm_state->seq + 1 < max_seq);

	if (max_seq > mm_state->seq) {
		mm_state->head = NULL;
		mm_state->tail = NULL;
		WRITE_ONCE(mm_state->seq, mm_state->seq + 1);
		reset_mm_stats(lruvec, NULL, true);
		success = true;
	}

	spin_unlock(&mm_list->lock);

	return success;
}

/******************************************************************************
 *                          refault feedback loop
 ******************************************************************************/

/*
 * A feedback loop based on Proportional-Integral-Derivative (PID) controller.
 *
 * The P term is refaulted/(evicted+protected) from a tier in the generation
 * currently being evicted; the I term is the exponential moving average of the
 * P term over the generations previously evicted, using the smoothing factor
 * 1/2; the D term isn't supported.
 *
 * The setpoint (SP) is always the first tier of one type; the process variable
 * (PV) is either any tier of the other type or any other tier of the same
 * type.
 *
 * The error is the difference between the SP and the PV; the correction is to
 * turn off protection when SP>PV or turn on protection when SP<PV.
 *
 * For future optimizations:
 * 1. The D term may discount the other two terms over time so that long-lived
 *    generations can resist stale information.
 */
struct ctrl_pos {
	unsigned long refaulted;
	unsigned long total;
	int gain;
};

static void read_ctrl_pos(struct lruvec *lruvec, int type, int tier, int gain,
			  struct ctrl_pos *pos)
{
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	int hist = lru_hist_from_seq(lrugen->min_seq[type]);

	pos->refaulted = lrugen->avg_refaulted[type][tier] +
			 atomic_long_read(&lrugen->refaulted[hist][type][tier]);
	pos->total = lrugen->avg_total[type][tier] +
		     atomic_long_read(&lrugen->evicted[hist][type][tier]);
	if (tier)
		pos->total += lrugen->protected[hist][type][tier - 1];
	pos->gain = gain;
}

static void reset_ctrl_pos(struct lruvec *lruvec, int type, bool carryover)
{
	int hist, tier;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	bool clear = carryover ? NR_HIST_GENS == 1 : NR_HIST_GENS > 1;
	unsigned long seq = carryover ? lrugen->min_seq[type] : lrugen->max_seq + 1;

	lockdep_assert_held(&lruvec->lru_lock);

	if (!carryover && !clear)
		return;

	hist = lru_hist_from_seq(seq);

	for (tier = 0; tier < MAX_NR_TIERS; tier++) {
		if (carryover) {
			unsigned long sum;

			sum = lrugen->avg_refaulted[type][tier] +
			      atomic_long_read(&lrugen->refaulted[hist][type][tier]);
			WRITE_ONCE(lrugen->avg_refaulted[type][tier], sum / 2);

			sum = lrugen->avg_total[type][tier] +
			      atomic_long_read(&lrugen->evicted[hist][type][tier]);
			if (tier)
				sum += lrugen->protected[hist][type][tier - 1];
			WRITE_ONCE(lrugen->avg_total[type][tier], sum / 2);
		}

		if (clear) {
			atomic_long_set(&lrugen->refaulted[hist][type][tier], 0);
			atomic_long_set(&lrugen->evicted[hist][type][tier], 0);
			if (tier)
				WRITE_ONCE(lrugen->protected[hist][type][tier - 1], 0);
		}
	}
}

static bool positive_ctrl_err(struct ctrl_pos *sp, struct ctrl_pos *pv)
{
	/*
	 * Return true if the PV has a limited number of refaults or a lower
	 * refaulted/total than the SP.
	 */
	return pv->refaulted < MIN_LRU_BATCH ||
	       pv->refaulted * (sp->total + MIN_LRU_BATCH) * sp->gain <=
	       (sp->refaulted + 1) * pv->total * pv->gain;
}

/******************************************************************************
 *                          the aging
 ******************************************************************************/

/* promote pages accessed through page tables */
static int page_update_gen(struct page *page, int gen)
{
	unsigned long new_flags, old_flags = READ_ONCE(page->flags);

	VM_WARN_ON_ONCE(gen >= MAX_NR_GENS);
	VM_WARN_ON_ONCE(!rcu_read_lock_held());

	do {
		/* lru_gen_del_page() has isolated this page? */
		if (!(old_flags & LRU_GEN_MASK)) {
			/* for shrink_page_list() */
			new_flags = old_flags | BIT(PG_referenced);
			continue;
		}

		new_flags = old_flags & ~(LRU_GEN_MASK | LRU_REFS_MASK | LRU_REFS_FLAGS);
		new_flags |= (gen + 1UL) << LRU_GEN_PGOFF;
	} while (!try_cmpxchg(&page->flags, &old_flags, new_flags));

	return ((old_flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;
}

/* protect pages accessed multiple times through file descriptors */
static int page_inc_gen(struct lruvec *lruvec, struct page *page, bool reclaiming)
{
	int type = page_is_file_lru(page);
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	int new_gen, old_gen = lru_gen_from_seq(lrugen->min_seq[type]);
	unsigned long new_flags, old_flags = READ_ONCE(page->flags);

	VM_WARN_ON_ONCE_PAGE(!(old_flags & LRU_GEN_MASK), page);

	do {
		new_gen = ((old_flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;
		/* page_update_gen() has promoted this page? */
		if (new_gen >= 0 && new_gen != old_gen)
			return new_gen;

		new_gen = (old_gen + 1) % MAX_NR_GENS;

		new_flags = old_flags & ~(LRU_GEN_MASK | LRU_REFS_MASK | LRU_REFS_FLAGS);
		new_flags |= (new_gen + 1UL) << LRU_GEN_PGOFF;
		/* for end_page_writeback() */
		if (reclaiming)
			new_flags |= BIT(PG_reclaim);
	} while (!try_cmpxchg(&page->flags, &old_flags, new_flags));

	lru_gen_update_size(lruvec, page, old_gen, new_gen);

	return new_gen;
}

static void update_batch_size(struct lru_gen_mm_walk *walk, struct page *page,
			      int old_gen, int new_gen)
{
	int type = page_is_file_lru(page);
	int zone = page_zonenum(page);
	int delta = thp_nr_pages(page);

	VM_WARN_ON_ONCE(old_gen >= MAX_NR_GENS);
	VM_WARN_ON_ONCE(new_gen >= MAX_NR_GENS);

	walk->batched++;

	walk->nr_pages[old_gen][type][zone] -= delta;
	walk->nr_pages[new_gen][type][zone] += delta;
}

static void reset_batch_size(struct lruvec *lruvec, struct lru_gen_mm_walk *walk)
{
	int gen, type, zone;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	walk->batched = 0;

	for_each_gen_type_zone(gen, type, zone) {
		enum lru_list lru = type * LRU_INACTIVE_FILE;
		int delta = walk->nr_pages[gen][type][zone];

		if (!delta)
			continue;

		walk->nr_pages[gen][type][zone] = 0;
		WRITE_ONCE(lrugen->nr_pages[gen][type][zone],
			   lrugen->nr_pages[gen][type][zone] + delta);

		if (lru_gen_is_active(lruvec, gen))
			lru += LRU_ACTIVE;
		__update_lru_size(lruvec, lru, zone, delta);
	}
}

static int should_skip_vma(unsigned long start, unsigned long end, struct mm_walk *args)
{
	struct address_space *mapping;
	struct vm_area_struct *vma = args->vma;
	struct lru_gen_mm_walk *walk = args->private;

	if (!vma_is_accessible(vma))
		return true;

	if (is_vm_hugetlb_page(vma))
		return true;

	if (vma->vm_flags & (VM_LOCKED | VM_SPECIAL | VM_SEQ_READ | VM_RAND_READ))
		return true;

	if (vma == get_gate_vma(vma->vm_mm))
		return true;

	if (vma_is_anonymous(vma))
		return !walk->can_swap;

	if (WARN_ON_ONCE(!vma->vm_file || !vma->vm_file->f_mapping))
		return true;

	mapping = vma->vm_file->f_mapping;
	if (mapping_unevictable(mapping))
		return true;

	if (shmem_mapping(mapping))
		return !walk->can_swap;

	/* to exclude special mappings like dax, etc. */
	return !mapping->a_ops->readpage;
}

/*
 * Some userspace memory allocators map many single-page VMAs. Instead of
 * returning back to the PGD table for each of such VMAs, finish an entire PMD
 * table to reduce zigzags and improve cache performance.
 */
static bool get_next_vma(unsigned long mask, unsigned long size, struct mm_walk *args,
			 unsigned long *vm_start, unsigned long *vm_end)
{
	unsigned long start = round_up(*vm_end, size);
	unsigned long end = (start | ~mask) + 1;

	VM_WARN_ON_ONCE(mask & size);
	VM_WARN_ON_ONCE((start & mask) != (*vm_start & mask));

	while (args->vma) {
		if (start >= args->vma->vm_end) {
			args->vma = args->vma->vm_next;
			continue;
		}

		if (end && end <= args->vma->vm_start)
			return false;

		if (should_skip_vma(args->vma->vm_start, args->vma->vm_end, args)) {
			args->vma = args->vma->vm_next;
			continue;
		}

		*vm_start = max(start, args->vma->vm_start);
		*vm_end = min(end - 1, args->vma->vm_end - 1) + 1;

		return true;
	}

	return false;
}

static unsigned long get_pte_pfn(pte_t pte, struct vm_area_struct *vma, unsigned long addr)
{
	unsigned long pfn = pte_pfn(pte);

	VM_WARN_ON_ONCE(addr < vma->vm_start || addr >= vma->vm_end);

	if (!pte_present(pte) || is_zero_pfn(pfn))
		return -1;

	if (WARN_ON_ONCE(pte_devmap(pte) || pte_special(pte)))
		return -1;

	if (WARN_ON_ONCE(!pfn_valid(pfn)))
		return -1;

	return pfn;
}

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) || defined(CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG)
static unsigned long get_pmd_pfn(pmd_t pmd, struct vm_area_struct *vma, unsigned long addr)
{
	unsigned long pfn = pmd_pfn(pmd);

	VM_WARN_ON_ONCE(addr < vma->vm_start || addr >= vma->vm_end);

	if (!pmd_present(pmd) || is_huge_zero_pmd(pmd))
		return -1;

	if (WARN_ON_ONCE(pmd_devmap(pmd)))
		return -1;

	if (WARN_ON_ONCE(!pfn_valid(pfn)))
		return -1;

	return pfn;
}
#endif

static struct page *get_pfn_page(unsigned long pfn, struct mem_cgroup *memcg,
				 struct pglist_data *pgdat, bool can_swap)
{
	struct page *page;

	/* try to avoid unnecessary memory loads */
	if (pfn < pgdat->node_start_pfn || pfn >= pgdat_end_pfn(pgdat))
		return NULL;

	page = compound_head(pfn_to_page(pfn));
	if (page_to_nid(page) != pgdat->node_id)
		return NULL;

	if (page_memcg_rcu(page) != memcg)
		return NULL;

	/* file VMAs can contain anon pages from COW */
	if (!page_is_file_lru(page) && !can_swap)
		return NULL;

	return page;
}

static bool suitable_to_scan(int total, int young)
{
	int n = clamp_t(int, cache_line_size() / sizeof(pte_t), 2, 8);

	/* suitable if the average number of young PTEs per cacheline is >=1 */
	return young * n >= total;
}

static bool walk_pte_range(pmd_t *pmd, unsigned long start, unsigned long end,
			   struct mm_walk *args)
{
	int i;
	pte_t *pte;
	spinlock_t *ptl;
	unsigned long addr;
	int total = 0;
	int young = 0;
	struct lru_gen_mm_walk *walk = args->private;
	struct mem_cgroup *memcg = lruvec_memcg(walk->lruvec);
	struct pglist_data *pgdat = lruvec_pgdat(walk->lruvec);
	int old_gen, new_gen = lru_gen_from_seq(walk->max_seq);

	VM_WARN_ON_ONCE(pmd_leaf(*pmd));

	ptl = pte_lockptr(args->mm, pmd);
	if (!spin_trylock(ptl))
		return false;

	arch_enter_lazy_mmu_mode();

	pte = pte_offset_map(pmd, start & PMD_MASK);
restart:
	for (i = pte_index(start), addr = start; addr != end; i++, addr += PAGE_SIZE) {
		unsigned long pfn;
		struct page *page;

		total++;
		walk->mm_stats[MM_LEAF_TOTAL]++;

		pfn = get_pte_pfn(pte[i], args->vma, addr);
		if (pfn == -1)
			continue;

		if (!pte_young(pte[i])) {
			walk->mm_stats[MM_LEAF_OLD]++;
			continue;
		}

		page = get_pfn_page(pfn, memcg, pgdat, walk->can_swap);
		if (!page)
			continue;

		if (!ptep_test_and_clear_young(args->vma, addr, pte + i))
			VM_WARN_ON_ONCE(true);

		young++;
		walk->mm_stats[MM_LEAF_YOUNG]++;

		if (pte_dirty(pte[i]) && !PageDirty(page) &&
		    !(PageAnon(page) && PageSwapBacked(page) &&
		      !PageSwapCache(page)))
			set_page_dirty(page);

		old_gen = page_update_gen(page, new_gen);
		if (old_gen >= 0 && old_gen != new_gen)
			update_batch_size(walk, page, old_gen, new_gen);
	}

	if (i < PTRS_PER_PTE && get_next_vma(PMD_MASK, PAGE_SIZE, args, &start, &end))
		goto restart;

	pte_unmap(pte);

	arch_leave_lazy_mmu_mode();
	spin_unlock(ptl);

	return suitable_to_scan(total, young);
}

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) || defined(CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG)
static void walk_pmd_range_locked(pud_t *pud, unsigned long next, struct vm_area_struct *vma,
				  struct mm_walk *args, unsigned long *bitmap, unsigned long *start)
{
	int i;
	pmd_t *pmd;
	spinlock_t *ptl;
	struct lru_gen_mm_walk *walk = args->private;
	struct mem_cgroup *memcg = lruvec_memcg(walk->lruvec);
	struct pglist_data *pgdat = lruvec_pgdat(walk->lruvec);
	int old_gen, new_gen = lru_gen_from_seq(walk->max_seq);

	VM_WARN_ON_ONCE(pud_leaf(*pud));

	/* try to batch at most 1+MIN_LRU_BATCH+1 entries */
	if (*start == -1) {
		*start = next;
		return;
	}

	i = next == -1 ? 0 : pmd_index(next) - pmd_index(*start);
	if (i && i <= MIN_LRU_BATCH) {
		__set_bit(i - 1, bitmap);
		return;
	}

	pmd = pmd_offset(pud, *start);

	ptl = pmd_lockptr(args->mm, pmd);
	if (!spin_trylock(ptl))
		goto done;

	arch_enter_lazy_mmu_mode();

	do {
		unsigned long pfn;
		struct page *page;
		unsigned long addr = i ? (*start & PMD_MASK) + i * PMD_SIZE : *start;

		pfn = get_pmd_pfn(pmd[i], vma, addr);
		if (pfn == -1)
			goto next;

		if (!pmd_trans_huge(pmd[i])) {
			if (IS_ENABLED(CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG) &&
			    get_cap(LRU_GEN_NONLEAF_YOUNG))
				pmdp_test_and_clear_young(vma, addr, pmd + i);
			goto next;
		}

		page = get_pfn_page(pfn, memcg, pgdat, walk->can_swap);
		if (!page)
			goto next;

		if (!pmdp_test_and_clear_young(vma, addr, pmd + i))
			goto next;

		walk->mm_stats[MM_LEAF_YOUNG]++;

		if (pmd_dirty(pmd[i]) && !PageDirty(page) &&
		    !(PageAnon(page) && PageSwapBacked(page) &&
		      !PageSwapCache(page)))
			set_page_dirty(page);

		old_gen = page_update_gen(page, new_gen);
		if (old_gen >= 0 && old_gen != new_gen)
			update_batch_size(walk, page, old_gen, new_gen);
next:
		i = i > MIN_LRU_BATCH ? 0 : find_next_bit(bitmap, MIN_LRU_BATCH, i) + 1;
	} while (i <= MIN_LRU_BATCH);

	arch_leave_lazy_mmu_mode();
	spin_unlock(ptl);
done:
	*start = -1;
	bitmap_zero(bitmap, MIN_LRU_BATCH);
}
#else
static void walk_pmd_range_locked(pud_t *pud, unsigned long next, struct vm_area_struct *vma,
				  struct mm_walk *args, unsigned long *bitmap, unsigned long *start)
{
}
#endif

static void walk_pmd_range(pud_t *pud, unsigned long start, unsigned long end,
			   struct mm_walk *args)
{
	int i;
	pmd_t *pmd;
	unsigned long next;
	unsigned long addr;
	struct vm_area_struct *vma;
	unsigned long pos = -1;
	struct lru_gen_mm_walk *walk = args->private;
	unsigned long bitmap[BITS_TO_LONGS(MIN_LRU_BATCH)] = {};

	VM_WARN_ON_ONCE(pud_leaf(*pud));

	/*
	 * Finish an entire PMD in two passes: the first only reaches to PTE
	 * tables to avoid taking the PMD lock; the second, if necessary, takes
	 * the PMD lock to clear the accessed bit in PMD entries.
	 */
	pmd = pmd_offset(pud, start & PUD_MASK);
restart:
	/* walk_pte_range() may call get_next_vma() */
	vma = args->vma;
	for (i = pmd_index(start), addr = start; addr != end; i++, addr = next) {
		pmd_t val = pmd_read_atomic(pmd + i);

		/* for pmd_read_atomic() */
		barrier();

		next = pmd_addr_end(addr, end);

		if (!pmd_present(val) || is_huge_zero_pmd(val)) {
			walk->mm_stats[MM_LEAF_TOTAL]++;
			continue;
		}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		if (pmd_trans_huge(val)) {
			unsigned long pfn = pmd_pfn(val);
			struct pglist_data *pgdat = lruvec_pgdat(walk->lruvec);

			walk->mm_stats[MM_LEAF_TOTAL]++;

			if (!pmd_young(val)) {
				walk->mm_stats[MM_LEAF_OLD]++;
				continue;
			}

			/* try to avoid unnecessary memory loads */
			if (pfn < pgdat->node_start_pfn || pfn >= pgdat_end_pfn(pgdat))
				continue;

			walk_pmd_range_locked(pud, addr, vma, args, bitmap, &pos);
			continue;
		}
#endif
		walk->mm_stats[MM_NONLEAF_TOTAL]++;

#ifdef CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG
		if (get_cap(LRU_GEN_NONLEAF_YOUNG)) {
			if (!pmd_young(val))
				continue;

			walk_pmd_range_locked(pud, addr, vma, args, bitmap, &pos);
		}
#endif
		if (!walk->full_scan && !test_bloom_filter(walk->lruvec, walk->max_seq, pmd + i))
			continue;

		walk->mm_stats[MM_NONLEAF_FOUND]++;

		if (!walk_pte_range(&val, addr, next, args))
			continue;

		walk->mm_stats[MM_NONLEAF_ADDED]++;

		/* carry over to the next generation */
		update_bloom_filter(walk->lruvec, walk->max_seq + 1, pmd + i);
	}

	walk_pmd_range_locked(pud, -1, vma, args, bitmap, &pos);

	if (i < PTRS_PER_PMD && get_next_vma(PUD_MASK, PMD_SIZE, args, &start, &end))
		goto restart;
}

static int walk_pud_range(p4d_t *p4d, unsigned long start, unsigned long end,
			  struct mm_walk *args)
{
	int i;
	pud_t *pud;
	unsigned long addr;
	unsigned long next;
	struct lru_gen_mm_walk *walk = args->private;

	VM_WARN_ON_ONCE(p4d_leaf(*p4d));

	pud = pud_offset(p4d, start & P4D_MASK);
restart:
	for (i = pud_index(start), addr = start; addr != end; i++, addr = next) {
		pud_t val = READ_ONCE(pud[i]);

		next = pud_addr_end(addr, end);

		if (!pud_present(val) || WARN_ON_ONCE(pud_leaf(val)))
			continue;

		walk_pmd_range(&val, addr, next, args);

		if (need_resched() || walk->batched >= MAX_LRU_BATCH) {
			end = (addr | ~PUD_MASK) + 1;
			goto done;
		}
	}

	if (i < PTRS_PER_PUD && get_next_vma(P4D_MASK, PUD_SIZE, args, &start, &end))
		goto restart;

	end = round_up(end, P4D_SIZE);
done:
	if (!end || !args->vma)
		return 1;

	walk->next_addr = max(end, args->vma->vm_start);

	return -EAGAIN;
}

static void walk_mm(struct lruvec *lruvec, struct mm_struct *mm, struct lru_gen_mm_walk *walk)
{
	static const struct mm_walk_ops mm_walk_ops = {
		.test_walk = should_skip_vma,
		.p4d_entry = walk_pud_range,
	};

	int err;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);

	walk->next_addr = FIRST_USER_ADDRESS;

	do {
		DEFINE_MAX_SEQ(lruvec);

		err = -EBUSY;

		/* another thread might have called inc_max_seq() */
		if (walk->max_seq != max_seq)
			break;

		/* page_update_gen() requires stable page_memcg() */
		if (!mem_cgroup_trylock_pages(memcg))
			break;

		/* the caller might be holding the lock for write */
		if (mmap_read_trylock(mm)) {
			err = walk_page_range(mm, walk->next_addr, ULONG_MAX, &mm_walk_ops, walk);

			mmap_read_unlock(mm);
		}

		mem_cgroup_unlock_pages();

		if (walk->batched) {
			spin_lock_irq(&lruvec->lru_lock);
			reset_batch_size(lruvec, walk);
			spin_unlock_irq(&lruvec->lru_lock);
		}

		cond_resched();
	} while (err == -EAGAIN);
}

static struct lru_gen_mm_walk *set_mm_walk(struct pglist_data *pgdat)
{
	struct lru_gen_mm_walk *walk = current->reclaim_state->mm_walk;

	if (pgdat && current_is_kswapd()) {
		VM_WARN_ON_ONCE(walk);

		walk = &pgdat->mm_walk;
	} else if (!pgdat && !walk) {
		VM_WARN_ON_ONCE(current_is_kswapd());

		walk = kzalloc(sizeof(*walk), __GFP_HIGH | __GFP_NOMEMALLOC | __GFP_NOWARN);
	}

	current->reclaim_state->mm_walk = walk;

	return walk;
}

static void clear_mm_walk(void)
{
	struct lru_gen_mm_walk *walk = current->reclaim_state->mm_walk;

	VM_WARN_ON_ONCE(walk && memchr_inv(walk->nr_pages, 0, sizeof(walk->nr_pages)));
	VM_WARN_ON_ONCE(walk && memchr_inv(walk->mm_stats, 0, sizeof(walk->mm_stats)));

	current->reclaim_state->mm_walk = NULL;

	if (!current_is_kswapd())
		kfree(walk);
}

static bool inc_min_seq(struct lruvec *lruvec, int type, bool can_swap)
{
	int zone;
	int remaining = MAX_LRU_BATCH;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	int new_gen, old_gen = lru_gen_from_seq(lrugen->min_seq[type]);

	if (type == LRU_GEN_ANON && !can_swap)
		goto done;

	/* prevent cold/hot inversion if full_scan is true */
	for (zone = 0; zone < MAX_NR_ZONES; zone++) {
		struct list_head *head = &lrugen->lists[old_gen][type][zone];

		while (!list_empty(head)) {
			struct page *page = lru_to_page(head);

			VM_WARN_ON_ONCE_PAGE(PageUnevictable(page), page);
			VM_WARN_ON_ONCE_PAGE(PageActive(page), page);
			VM_WARN_ON_ONCE_PAGE(page_is_file_lru(page) != type, page);
			VM_WARN_ON_ONCE_PAGE(page_zonenum(page) != zone, page);

			new_gen = page_inc_gen(lruvec, page, false);
			list_move_tail(&page->lru, &lrugen->lists[new_gen][type][zone]);

			if (!--remaining)
				return false;
		}
	}
done:
	reset_ctrl_pos(lruvec, type, true);
	WRITE_ONCE(lrugen->min_seq[type], lrugen->min_seq[type] + 1);

	return true;
}

static bool try_to_inc_min_seq(struct lruvec *lruvec, bool can_swap)
{
	int gen, type, zone;
	bool success = false;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	DEFINE_MIN_SEQ(lruvec);

	VM_WARN_ON_ONCE(!seq_is_valid(lruvec));

	/* find the oldest populated generation */
	for (type = !can_swap; type < ANON_AND_FILE; type++) {
		while (min_seq[type] + MIN_NR_GENS <= lrugen->max_seq) {
			gen = lru_gen_from_seq(min_seq[type]);

			for (zone = 0; zone < MAX_NR_ZONES; zone++) {
				if (!list_empty(&lrugen->lists[gen][type][zone]))
					goto next;
			}

			min_seq[type]++;
		}
next:
		;
	}

	/* see the comment on lru_gen_struct */
	if (can_swap) {
		min_seq[LRU_GEN_ANON] = min(min_seq[LRU_GEN_ANON], min_seq[LRU_GEN_FILE]);
		min_seq[LRU_GEN_FILE] = max(min_seq[LRU_GEN_ANON], lrugen->min_seq[LRU_GEN_FILE]);
	}

	for (type = !can_swap; type < ANON_AND_FILE; type++) {
		if (min_seq[type] == lrugen->min_seq[type])
			continue;

		reset_ctrl_pos(lruvec, type, true);
		WRITE_ONCE(lrugen->min_seq[type], min_seq[type]);
		success = true;
	}

	return success;
}

static void inc_max_seq(struct lruvec *lruvec, bool can_swap, bool full_scan)
{
	int prev, next;
	int type, zone;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
restart:
	spin_lock_irq(&lruvec->lru_lock);

	VM_WARN_ON_ONCE(!seq_is_valid(lruvec));

	for (type = ANON_AND_FILE - 1; type >= 0; type--) {
		if (get_nr_gens(lruvec, type) != MAX_NR_GENS)
			continue;

		VM_WARN_ON_ONCE(!full_scan && (type == LRU_GEN_FILE || can_swap));

		while (!inc_min_seq(lruvec, type, can_swap)) {
			spin_unlock_irq(&lruvec->lru_lock);
			cond_resched();
			spin_lock_irq(&lruvec->lru_lock);
		}
		if (inc_min_seq(lruvec, type, can_swap))
			continue;

		spin_unlock_irq(&lruvec->lru_lock);
		cond_resched();
		goto restart;

	}

	/*
	 * Update the active/inactive LRU sizes for compatibility. Both sides of
	 * the current max_seq need to be covered, since max_seq+1 can overlap
	 * with min_seq[LRU_GEN_ANON] if swapping is constrained. And if they do
	 * overlap, cold/hot inversion happens.
	 */
	prev = lru_gen_from_seq(lrugen->max_seq - 1);
	next = lru_gen_from_seq(lrugen->max_seq + 1);

	for (type = 0; type < ANON_AND_FILE; type++) {
		for (zone = 0; zone < MAX_NR_ZONES; zone++) {
			enum lru_list lru = type * LRU_INACTIVE_FILE;
			long delta = lrugen->nr_pages[prev][type][zone] -
				     lrugen->nr_pages[next][type][zone];

			if (!delta)
				continue;

			__update_lru_size(lruvec, lru, zone, delta);
			__update_lru_size(lruvec, lru + LRU_ACTIVE, zone, -delta);
		}
	}

	for (type = 0; type < ANON_AND_FILE; type++)
		reset_ctrl_pos(lruvec, type, false);

	WRITE_ONCE(lrugen->timestamps[next], jiffies);
	/* make sure preceding modifications appear */
	smp_store_release(&lrugen->max_seq, lrugen->max_seq + 1);

	spin_unlock_irq(&lruvec->lru_lock);
}

static bool try_to_inc_max_seq(struct lruvec *lruvec, unsigned long max_seq,
			       struct scan_control *sc, bool can_swap, bool full_scan)
{
	bool success;
	struct lru_gen_mm_walk *walk;
	struct mm_struct *mm = NULL;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	VM_WARN_ON_ONCE(max_seq > READ_ONCE(lrugen->max_seq));

	/* see the comment in iterate_mm_list() */
	if (max_seq <= READ_ONCE(lruvec->mm_state.seq)) {
		success = false;
		goto done;
	}

	/*
	 * If the hardware doesn't automatically set the accessed bit, fallback
	 * to lru_gen_look_around(), which only clears the accessed bit in a
	 * handful of PTEs. Spreading the work out over a period of time usually
	 * is less efficient, but it avoids bursty page faults.
	 */
	if (!full_scan && !(arch_has_hw_pte_young() && get_cap(LRU_GEN_MM_WALK))) {
		success = iterate_mm_list_nowalk(lruvec, max_seq);
		goto done;
	}

	walk = set_mm_walk(NULL);
	if (!walk) {
		success = iterate_mm_list_nowalk(lruvec, max_seq);
		goto done;
	}

	walk->lruvec = lruvec;
	walk->max_seq = max_seq;
	walk->can_swap = can_swap;
	walk->full_scan = full_scan;

	do {
		success = iterate_mm_list(lruvec, walk, &mm);
		if (mm)
			walk_mm(lruvec, mm, walk);
	} while (mm);
done:
	if (success)
		inc_max_seq(lruvec, can_swap, full_scan);

	return success;
}

static bool should_run_aging(struct lruvec *lruvec, unsigned long max_seq, unsigned long *min_seq,
			     struct scan_control *sc, bool can_swap, unsigned long *nr_to_scan)
{
	int gen, type, zone;
	unsigned long old = 0;
	unsigned long young = 0;
	unsigned long total = 0;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);

	for (type = !can_swap; type < ANON_AND_FILE; type++) {
		unsigned long seq;

		for (seq = min_seq[type]; seq <= max_seq; seq++) {
			unsigned long size = 0;

			gen = lru_gen_from_seq(seq);

			for (zone = 0; zone < MAX_NR_ZONES; zone++)
				size += max_t(long, READ_ONCE(lrugen->nr_pages[gen][type][zone]),
						0);

			total += size;
			if (seq == max_seq)
				young += size;
			else if (seq + MIN_NR_GENS == max_seq)
				old += size;
		}
	}

	/* try to scrape all its memory if this memcg was deleted */
	*nr_to_scan = mem_cgroup_online(memcg) ? (total >> sc->priority) : total;

	/*
	 * The aging tries to be lazy to reduce the overhead, while the eviction
	 * stalls when the number of generations reaches MIN_NR_GENS. Hence, the
	 * ideal number of generations is MIN_NR_GENS+1.
	 */
	if (min_seq[!can_swap] + MIN_NR_GENS > max_seq)
		return true;
	if (min_seq[!can_swap] + MIN_NR_GENS < max_seq)
		return false;

	/*
	 * It's also ideal to spread pages out evenly, i.e., 1/(MIN_NR_GENS+1)
	 * of the total number of pages for each generation. A reasonable range
	 * for this average portion is [1/MIN_NR_GENS, 1/(MIN_NR_GENS+2)]. The
	 * aging cares about the upper bound of hot pages, while the eviction
	 * cares about the lower bound of cold pages.
	 */
	if (young * MIN_NR_GENS > total)
		return true;
	if (old * (MIN_NR_GENS + 2) < total)
		return true;

	return false;
}

static bool age_lruvec(struct lruvec *lruvec, struct scan_control *sc, unsigned long min_ttl)
{
	bool need_aging;
	unsigned long nr_to_scan;
	int swappiness = get_swappiness(lruvec, sc);
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	DEFINE_MAX_SEQ(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	VM_WARN_ON_ONCE(sc->memcg_low_reclaim);

	mem_cgroup_calculate_protection(NULL, memcg);

	if (mem_cgroup_below_min(memcg))
		return false;

	need_aging = should_run_aging(lruvec, max_seq, min_seq, sc, swappiness, &nr_to_scan);

	if (min_ttl) {
		int gen = lru_gen_from_seq(min_seq[LRU_GEN_FILE]);
		unsigned long birth = READ_ONCE(lruvec->lrugen.timestamps[gen]);

		if (time_is_after_jiffies(birth + min_ttl))
			return false;

		/* the size is likely too small to be helpful */
		if (!nr_to_scan && sc->priority != DEF_PRIORITY)
			return false;
	}

	if (need_aging)
		try_to_inc_max_seq(lruvec, max_seq, sc, swappiness, false);

	return true;
}

/* to protect the working set of the last N jiffies */
static unsigned long lru_gen_min_ttl __read_mostly;

static void lru_gen_age_node(struct pglist_data *pgdat, struct scan_control *sc)
{
	struct mem_cgroup *memcg;
	bool success = false;
	unsigned long min_ttl = READ_ONCE(lru_gen_min_ttl);

	VM_WARN_ON_ONCE(!current_is_kswapd());

	sc->last_reclaimed = sc->nr_reclaimed;

	/*
	 * To reduce the chance of going into the aging path, which can be
	 * costly, optimistically skip it if the flag below was cleared in the
	 * eviction path. This improves the overall performance when multiple
	 * memcgs are available.
	 */
	if (!sc->memcgs_need_aging) {
		sc->memcgs_need_aging = true;
		return;
	}

	set_mm_walk(pgdat);

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		/* FIXME: chp doesn't care */
		struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);

		if (age_lruvec(lruvec, sc, min_ttl))
			success = true;

		cond_resched();
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));

	clear_mm_walk();

	/* check the order to exclude compaction-induced reclaim */
	if (success || !min_ttl || sc->order)
		return;

	/*
	 * The main goal is to OOM kill if every generation from all memcgs is
	 * younger than min_ttl. However, another possibility is all memcgs are
	 * either below min or empty.
	 */
	if (mutex_trylock(&oom_lock)) {
		struct oom_control oc = {
			.gfp_mask = sc->gfp_mask,
		};

		out_of_memory(&oc);

		mutex_unlock(&oom_lock);
	}
}

/*
 * This function exploits spatial locality when shrink_page_list() walks the
 * rmap. It scans the adjacent PTEs of a young PTE and promotes hot pages. If
 * the scan was done cacheline efficiently, it adds the PMD entry pointing to
 * the PTE table to the Bloom filter. This forms a feedback loop between the
 * eviction and the aging.
 */
void lru_gen_look_around(struct page_vma_mapped_walk *pvmw)
{
	int i;
	pte_t *pte;
	unsigned long start;
	unsigned long end;
	unsigned long addr;
	struct lru_gen_mm_walk *walk;
	int young = 0;
	unsigned long bitmap[BITS_TO_LONGS(MIN_LRU_BATCH)] = {};
	struct page *page = pvmw->page;
	bool can_swap = !page_is_file_lru(page);
	struct mem_cgroup *memcg = page_memcg(page);
	struct pglist_data *pgdat = page_pgdat(page);
	/* FIXME: chp doesn't care */
	struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
	DEFINE_MAX_SEQ(lruvec);
	int old_gen, new_gen = lru_gen_from_seq(max_seq);

	lockdep_assert_held(pvmw->ptl);
	VM_WARN_ON_ONCE_PAGE(PageLRU(page), page);

	if (spin_is_contended(pvmw->ptl))
		return;

	/* avoid taking the LRU lock under the PTL when possible */
	walk = current->reclaim_state ? current->reclaim_state->mm_walk : NULL;

	start = max(pvmw->address & PMD_MASK, pvmw->vma->vm_start);
	end = min(pvmw->address | ~PMD_MASK, pvmw->vma->vm_end - 1) + 1;

	if (end - start > MIN_LRU_BATCH * PAGE_SIZE) {
		if (pvmw->address - start < MIN_LRU_BATCH * PAGE_SIZE / 2)
			end = start + MIN_LRU_BATCH * PAGE_SIZE;
		else if (end - pvmw->address < MIN_LRU_BATCH * PAGE_SIZE / 2)
			start = end - MIN_LRU_BATCH * PAGE_SIZE;
		else {
			start = pvmw->address - MIN_LRU_BATCH * PAGE_SIZE / 2;
			end = pvmw->address + MIN_LRU_BATCH * PAGE_SIZE / 2;
		}
	}

	pte = pvmw->pte - (pvmw->address - start) / PAGE_SIZE;

	rcu_read_lock();
	arch_enter_lazy_mmu_mode();

	for (i = 0, addr = start; addr != end; i++, addr += PAGE_SIZE) {
		unsigned long pfn;

		pfn = get_pte_pfn(pte[i], pvmw->vma, addr);
		if (pfn == -1)
			continue;

		if (!pte_young(pte[i]))
			continue;

		page = get_pfn_page(pfn, memcg, pgdat, can_swap);
		if (!page)
			continue;

		if (!ptep_test_and_clear_young(pvmw->vma, addr, pte + i))
			VM_WARN_ON_ONCE(true);

		young++;

		if (pte_dirty(pte[i]) && !PageDirty(page) &&
		    !(PageAnon(page) && PageSwapBacked(page) &&
		      !PageSwapCache(page)))
			set_page_dirty(page);

		old_gen = page_lru_gen(page);
		if (old_gen < 0)
			SetPageReferenced(page);
		else if (old_gen != new_gen)
			__set_bit(i, bitmap);
	}

	arch_leave_lazy_mmu_mode();
	rcu_read_unlock();

	/* feedback from rmap walkers to page table walkers */
	if (suitable_to_scan(i, young))
		update_bloom_filter(lruvec, max_seq, pvmw->pmd);

	if (!walk && bitmap_weight(bitmap, MIN_LRU_BATCH) < PAGEVEC_SIZE) {
		for_each_set_bit(i, bitmap, MIN_LRU_BATCH) {
			page = pte_page(pte[i]);
			activate_page(page);
		}
		return;
	}

	/* page_update_gen() requires stable page_memcg() */
	if (!mem_cgroup_trylock_pages(memcg))
		return;

	if (!walk) {
		spin_lock_irq(&lruvec->lru_lock);
		new_gen = lru_gen_from_seq(lruvec->lrugen.max_seq);
	}

	for_each_set_bit(i, bitmap, MIN_LRU_BATCH) {
		page = compound_head(pte_page(pte[i]));
		if (page_memcg_rcu(page) != memcg)
			continue;

		old_gen = page_update_gen(page, new_gen);
		if (old_gen < 0 || old_gen == new_gen)
			continue;

		if (walk)
			update_batch_size(walk, page, old_gen, new_gen);
		else
			lru_gen_update_size(lruvec, page, old_gen, new_gen);
	}

	if (!walk)
		spin_unlock_irq(&lruvec->lru_lock);

	mem_cgroup_unlock_pages();
}

/******************************************************************************
 *                          the eviction
 ******************************************************************************/

static bool sort_page(struct lruvec *lruvec, struct page *page, struct scan_control *sc,
		       int tier_idx)
{
	bool success;
	int gen = page_lru_gen(page);
	int type = page_is_file_lru(page);
	int zone = page_zonenum(page);
	int delta = thp_nr_pages(page);
	int refs = page_lru_refs(page);
	int tier = lru_tier_from_refs(refs);
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	VM_WARN_ON_ONCE_PAGE(gen >= MAX_NR_GENS, page);

	/* unevictable */
	if (!page_evictable(page)) {
		success = lru_gen_del_page(lruvec, page, true);
		VM_WARN_ON_ONCE_PAGE(!success, page);
		SetPageUnevictable(page);
		add_page_to_lru_list(page, lruvec);
		__count_vm_events(UNEVICTABLE_PGCULLED, delta);
		return true;
	}

	/* dirty lazyfree */
	if (type == LRU_GEN_FILE && PageAnon(page) && PageDirty(page)) {
		success = lru_gen_del_page(lruvec, page, true);
		VM_WARN_ON_ONCE_PAGE(!success, page);
		SetPageSwapBacked(page);
		add_page_to_lru_list_tail(page, lruvec);
		return true;
	}

	/* promoted */
	if (gen != lru_gen_from_seq(lrugen->min_seq[type])) {
		list_move(&page->lru, &lrugen->lists[gen][type][zone]);
		return true;
	}

	/* protected */
	if (tier > tier_idx) {
		int hist = lru_hist_from_seq(lrugen->min_seq[type]);

		gen = page_inc_gen(lruvec, page, false);
		list_move_tail(&page->lru, &lrugen->lists[gen][type][zone]);

		WRITE_ONCE(lrugen->protected[hist][type][tier - 1],
			   lrugen->protected[hist][type][tier - 1] + delta);
		return true;
	}

	/* ineligible */
	if (zone > sc->reclaim_idx || skip_cma(page, sc)) {
		gen = page_inc_gen(lruvec, page, false);
		list_move_tail(&page->lru, &lrugen->lists[gen][type][zone]);
		return true;
	}

	/* waiting for writeback */
	if (PageLocked(page) || PageWriteback(page) ||
	    (type == LRU_GEN_FILE && PageDirty(page))) {
		gen = page_inc_gen(lruvec, page, true);
		list_move(&page->lru, &lrugen->lists[gen][type][zone]);
		return true;
	}

	return false;
}

static bool isolate_page(struct lruvec *lruvec, struct page *page, struct scan_control *sc)
{
	bool success;

	/* unmapping inhibited */
	if (!sc->may_unmap && page_mapped(page))
		return false;

	/* swapping inhibited */
	if (!(sc->may_writepage && (sc->gfp_mask & __GFP_IO)) &&
	    (PageDirty(page) ||
	     (PageAnon(page) && !PageSwapCache(page))))
		return false;

	/* raced with release_pages() */
	if (!get_page_unless_zero(page))
		return false;

	/* raced with another isolation */
	if (!TestClearPageLRU(page)) {
		put_page(page);
		return false;
	}

	/* see the comment on MAX_NR_TIERS */
	if (!PageReferenced(page))
		set_mask_bits(&page->flags, LRU_REFS_MASK | LRU_REFS_FLAGS, 0);

	/* for shrink_page_list() */
	ClearPageReclaim(page);
	ClearPageReferenced(page);

	success = lru_gen_del_page(lruvec, page, true);
	VM_WARN_ON_ONCE_PAGE(!success, page);

	return true;
}

static int scan_pages(struct lruvec *lruvec, struct scan_control *sc,
		      int type, int tier, struct list_head *list)
{
	int i;
	int gen;
	enum vm_event_item item;
	int sorted = 0;
	int scanned = 0;
	int isolated = 0;
	int remaining = MAX_LRU_BATCH;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);

	VM_WARN_ON_ONCE(!list_empty(list));

	if (get_nr_gens(lruvec, type) == MIN_NR_GENS)
		return 0;

	gen = lru_gen_from_seq(lrugen->min_seq[type]);

	for (i = MAX_NR_ZONES; i > 0; i--) {
		LIST_HEAD(moved);
		int skipped = 0;
		int zone = (sc->reclaim_idx + i) % MAX_NR_ZONES;
		struct list_head *head = &lrugen->lists[gen][type][zone];

		while (!list_empty(head)) {
			struct page *page = lru_to_page(head);
			int delta = thp_nr_pages(page);

			VM_WARN_ON_ONCE_PAGE(PageUnevictable(page), page);
			VM_WARN_ON_ONCE_PAGE(PageActive(page), page);
			VM_WARN_ON_ONCE_PAGE(page_is_file_lru(page) != type, page);
			VM_WARN_ON_ONCE_PAGE(page_zonenum(page) != zone, page);

			scanned += delta;

			if (sort_page(lruvec, page, sc, tier))
				sorted += delta;
			else if (isolate_page(lruvec, page, sc)) {
				list_add(&page->lru, list);
				isolated += delta;
			} else {
				list_move(&page->lru, &moved);
				skipped += delta;
			}

			if (!--remaining || max(isolated, skipped) >= MIN_LRU_BATCH)
				break;
		}

		if (skipped) {
			list_splice(&moved, head);
			__count_zid_vm_events(PGSCAN_SKIP, zone, skipped);
		}

		if (!remaining || isolated >= MIN_LRU_BATCH)
			break;
	}

	item = current_is_kswapd() ? PGSCAN_KSWAPD : PGSCAN_DIRECT;
	if (!cgroup_reclaim(sc)) {
		__count_vm_events(item, isolated);
		__count_vm_events(PGREFILL, sorted);
	}
	__count_memcg_events(memcg, item, isolated);
	__count_memcg_events(memcg, PGREFILL, sorted);
	__count_vm_events(PGSCAN_ANON + type, isolated);

	/*
	 * There might not be eligible pages due to reclaim_idx, may_unmap and
	 * may_writepage. Check the remaining to prevent livelock if it's not
	 * making progress.
	 */
	return isolated || !remaining ? scanned : 0;
}

static int get_tier_idx(struct lruvec *lruvec, int type)
{
	int tier;
	struct ctrl_pos sp, pv;

	/*
	 * To leave a margin for fluctuations, use a larger gain factor (1:2).
	 * This value is chosen because any other tier would have at least twice
	 * as many refaults as the first tier.
	 */
	read_ctrl_pos(lruvec, type, 0, 1, &sp);
	for (tier = 1; tier < MAX_NR_TIERS; tier++) {
		read_ctrl_pos(lruvec, type, tier, 2, &pv);
		if (!positive_ctrl_err(&sp, &pv))
			break;
	}

	return tier - 1;
}

static int get_type_to_scan(struct lruvec *lruvec, int swappiness, int *tier_idx)
{
	int type, tier;
	struct ctrl_pos sp, pv;
	int gain[ANON_AND_FILE] = { swappiness, 200 - swappiness };

	/*
	 * Compare the first tier of anon with that of file to determine which
	 * type to scan. Also need to compare other tiers of the selected type
	 * with the first tier of the other type to determine the last tier (of
	 * the selected type) to evict.
	 */
	read_ctrl_pos(lruvec, LRU_GEN_ANON, 0, gain[LRU_GEN_ANON], &sp);
	read_ctrl_pos(lruvec, LRU_GEN_FILE, 0, gain[LRU_GEN_FILE], &pv);
	type = positive_ctrl_err(&sp, &pv);

	read_ctrl_pos(lruvec, !type, 0, gain[!type], &sp);
	for (tier = 1; tier < MAX_NR_TIERS; tier++) {
		read_ctrl_pos(lruvec, type, tier, gain[type], &pv);
		if (!positive_ctrl_err(&sp, &pv))
			break;
	}

	*tier_idx = tier - 1;

	return type;
}

static int isolate_pages(struct lruvec *lruvec, struct scan_control *sc, int swappiness,
			 int *type_scanned, struct list_head *list)
{
	int i;
	int type;
	int scanned;
	int tier = -1;
	DEFINE_MIN_SEQ(lruvec);

	/*
	 * Try to make the obvious choice first. When anon and file are both
	 * available from the same generation, interpret swappiness 1 as file
	 * first and 200 as anon first.
	 */
	if (!swappiness)
		type = LRU_GEN_FILE;
	else if (min_seq[LRU_GEN_ANON] < min_seq[LRU_GEN_FILE])
		type = LRU_GEN_ANON;
	else if (swappiness == 1)
		type = LRU_GEN_FILE;
	else if (swappiness == 200)
		type = LRU_GEN_ANON;
	else
		type = get_type_to_scan(lruvec, swappiness, &tier);

	for (i = !swappiness; i < ANON_AND_FILE; i++) {
		if (tier < 0)
			tier = get_tier_idx(lruvec, type);

		scanned = scan_pages(lruvec, sc, type, tier, list);
		if (scanned)
			break;

		type = !type;
		tier = -1;
	}

	*type_scanned = type;

	return scanned;
}

static int evict_pages(struct lruvec *lruvec, struct scan_control *sc, int swappiness,
		       bool *need_swapping)
{
	int type;
	int scanned;
	int reclaimed;
	LIST_HEAD(list);
	LIST_HEAD(clean);
	struct page *page;
	struct page *next;
	enum vm_event_item item;
	struct reclaim_stat stat;
	struct lru_gen_mm_walk *walk;
	bool skip_retry = false;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	spin_lock_irq(&lruvec->lru_lock);

	scanned = isolate_pages(lruvec, sc, swappiness, &type, &list);

	scanned += try_to_inc_min_seq(lruvec, swappiness);

	if (get_nr_gens(lruvec, !swappiness) == MIN_NR_GENS)
		scanned = 0;

	spin_unlock_irq(&lruvec->lru_lock);

	if (list_empty(&list))
		return scanned;
retry:
	reclaimed = shrink_page_list(&list, pgdat, sc, &stat, false);
	sc->nr_reclaimed += reclaimed;

	list_for_each_entry_safe_reverse(page, next, &list, lru) {
		if (!page_evictable(page)) {
			list_del(&page->lru);
			putback_lru_page(page);
			continue;
		}

		if (PageReclaim(page) &&
		    (PageDirty(page) || PageWriteback(page))) {
			/* restore LRU_REFS_FLAGS cleared by isolate_page() */
			if (PageWorkingset(page))
				SetPageReferenced(page);
			continue;
		}

		if (skip_retry || PageActive(page) || PageReferenced(page) ||
		    page_mapped(page) || PageLocked(page) ||
		    PageDirty(page) || PageWriteback(page)) {
			/* don't add rejected pages to the oldest generation */
			set_mask_bits(&page->flags, LRU_REFS_MASK | LRU_REFS_FLAGS,
				      BIT(PG_active));
			continue;
		}

		/* retry pages that may have missed rotate_reclaimable_page() */
		list_move(&page->lru, &clean);
		sc->nr_scanned -= thp_nr_pages(page);
	}

	spin_lock_irq(&lruvec->lru_lock);

	move_pages_to_lru(lruvec, &list);

	walk = current->reclaim_state->mm_walk;
	if (walk && walk->batched)
		reset_batch_size(lruvec, walk);

	item = current_is_kswapd() ? PGSTEAL_KSWAPD : PGSTEAL_DIRECT;
	if (!cgroup_reclaim(sc))
		__count_vm_events(item, reclaimed);
	__count_memcg_events(memcg, item, reclaimed);
	__count_vm_events(PGSTEAL_ANON + type, reclaimed);

	spin_unlock_irq(&lruvec->lru_lock);

	mem_cgroup_uncharge_list(&list);
	free_unref_page_list(&list);

	INIT_LIST_HEAD(&list);
	list_splice_init(&clean, &list);

	if (!list_empty(&list)) {
		skip_retry = true;
		goto retry;
	}

	if (need_swapping && type == LRU_GEN_ANON)
		*need_swapping = true;

	return scanned;
}

/*
 * For future optimizations:
 * 1. Defer try_to_inc_max_seq() to workqueues to reduce latency for memcg
 *    reclaim.
 */
static unsigned long get_nr_to_scan(struct lruvec *lruvec, struct scan_control *sc,
				    bool can_swap, bool *need_aging)
{
	unsigned long nr_to_scan;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	DEFINE_MAX_SEQ(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	if (mem_cgroup_below_min(memcg) ||
	    (mem_cgroup_below_low(memcg) && !sc->memcg_low_reclaim))
		return 0;

	*need_aging = should_run_aging(lruvec, max_seq, min_seq, sc, can_swap, &nr_to_scan);
	if (!*need_aging)
		return nr_to_scan;

	/* skip the aging path at the default priority */
	if (sc->priority == DEF_PRIORITY)
		goto done;

	/* leave the work to lru_gen_age_node() */
	if (current_is_kswapd())
		return 0;

	if (try_to_inc_max_seq(lruvec, max_seq, sc, can_swap, false))
		return nr_to_scan;
done:
	return min_seq[!can_swap] + MIN_NR_GENS <= max_seq ? nr_to_scan : 0;
}

static bool should_abort_scan(struct lruvec *lruvec, unsigned long seq,
			      struct scan_control *sc, bool need_swapping)
{
	int i;
	DEFINE_MAX_SEQ(lruvec);

	if (!current_is_kswapd()) {
		/* age each memcg at most once to ensure fairness */
		if (max_seq - seq > 1)
			return true;

		/* over-swapping can increase allocation latency */
		if (sc->nr_reclaimed >= sc->nr_to_reclaim && need_swapping)
			return true;

		/* give this thread a chance to exit and free its memory */
		if (fatal_signal_pending(current)) {
			sc->nr_reclaimed += MIN_LRU_BATCH;
			return true;
		}

		if (cgroup_reclaim(sc))
			return false;
	} else if (sc->nr_reclaimed - sc->last_reclaimed < sc->nr_to_reclaim)
		return false;

	/* keep scanning at low priorities to ensure fairness */
	if (sc->priority > DEF_PRIORITY - 2)
		return false;

	/*
	 * A minimum amount of work was done under global memory pressure. For
	 * kswapd, it may be overshooting. For direct reclaim, the allocation
	 * may succeed if all suitable zones are somewhat safe. In either case,
	 * it's better to stop now, and restart later if necessary.
	 */
	for (i = 0; i <= sc->reclaim_idx; i++) {
		unsigned long wmark;
		struct zone *zone = lruvec_pgdat(lruvec)->node_zones + i;

		if (!managed_zone(zone))
			continue;

		wmark = current_is_kswapd() ? high_wmark_pages(zone) : low_wmark_pages(zone);
		if (wmark > zone_page_state(zone, NR_FREE_PAGES))
			return false;
	}

	sc->nr_reclaimed += MIN_LRU_BATCH;

	return true;
}

static void lru_gen_shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
	struct blk_plug plug;
	bool need_aging = false;
	bool need_swapping = false;
	unsigned long scanned = 0;
	unsigned long reclaimed = sc->nr_reclaimed;
	DEFINE_MAX_SEQ(lruvec);

	lru_add_drain();

	blk_start_plug(&plug);

	set_mm_walk(lruvec_pgdat(lruvec));

	while (true) {
		int delta;
		int swappiness;
		unsigned long nr_to_scan;

		if (sc->may_swap)
			swappiness = get_swappiness(lruvec, sc);
		else if (!cgroup_reclaim(sc) && get_swappiness(lruvec, sc))
			swappiness = 1;
		else
			swappiness = 0;

		nr_to_scan = get_nr_to_scan(lruvec, sc, swappiness, &need_aging);
		if (!nr_to_scan)
			goto done;

		delta = evict_pages(lruvec, sc, swappiness, &need_swapping);
		if (!delta)
			goto done;

		scanned += delta;
		if (scanned >= nr_to_scan)
			break;

		if (should_abort_scan(lruvec, max_seq, sc, need_swapping))
			break;

		cond_resched();
	}

	/* see the comment in lru_gen_age_node() */
	if (sc->nr_reclaimed - reclaimed >= MIN_LRU_BATCH && !need_aging)
		sc->memcgs_need_aging = false;
done:
	clear_mm_walk();

	blk_finish_plug(&plug);
}

/******************************************************************************
 *                          state change
 ******************************************************************************/

static bool __maybe_unused state_is_valid(struct lruvec *lruvec)
{
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	if (lrugen->enabled) {
		enum lru_list lru;

		for_each_evictable_lru(lru) {
			if (!list_empty(&lruvec->lists[lru]))
				return false;
		}
	} else {
		int gen, type, zone;

		for_each_gen_type_zone(gen, type, zone) {
			if (!list_empty(&lrugen->lists[gen][type][zone]))
				return false;
		}
	}

	return true;
}

static bool fill_evictable(struct lruvec *lruvec)
{
	enum lru_list lru;
	int remaining = MAX_LRU_BATCH;

	for_each_evictable_lru(lru) {
		int type = is_file_lru(lru);
		bool active = is_active_lru(lru);
		struct list_head *head = &lruvec->lists[lru];

		while (!list_empty(head)) {
			bool success;
			struct page *page = lru_to_page(head);

			VM_WARN_ON_ONCE_PAGE(PageUnevictable(page), page);
			VM_WARN_ON_ONCE_PAGE(PageActive(page) != active, page);
			VM_WARN_ON_ONCE_PAGE(page_is_file_lru(page) != type, page);
			VM_WARN_ON_ONCE_PAGE(page_lru_gen(page) != -1, page);

			del_page_from_lru_list(page, lruvec);
			success = lru_gen_add_page(lruvec, page, false);
			VM_WARN_ON_ONCE(!success);

			if (!--remaining)
				return false;
		}
	}

	return true;
}

static bool drain_evictable(struct lruvec *lruvec)
{
	int gen, type, zone;
	int remaining = MAX_LRU_BATCH;

	for_each_gen_type_zone(gen, type, zone) {
		struct list_head *head = &lruvec->lrugen.lists[gen][type][zone];

		while (!list_empty(head)) {
			bool success;
			struct page *page = lru_to_page(head);

			VM_WARN_ON_ONCE_PAGE(PageUnevictable(page), page);
			VM_WARN_ON_ONCE_PAGE(PageActive(page), page);
			VM_WARN_ON_ONCE_PAGE(page_is_file_lru(page) != type, page);
			VM_WARN_ON_ONCE_PAGE(page_zonenum(page) != zone, page);

			success = lru_gen_del_page(lruvec, page, false);
			VM_WARN_ON_ONCE(!success);
			add_page_to_lru_list(page, lruvec);

			if (!--remaining)
				return false;
		}
	}

	return true;
}

static void lru_gen_change_state(bool enabled)
{
	static DEFINE_MUTEX(state_mutex);

	struct mem_cgroup *memcg;

	cgroup_lock();
	cpus_read_lock();
	get_online_mems();
	mutex_lock(&state_mutex);

	if (enabled == lru_gen_enabled())
		goto unlock;

	if (enabled)
		static_branch_enable_cpuslocked(&lru_gen_caps[LRU_GEN_CORE]);
	else
		static_branch_disable_cpuslocked(&lru_gen_caps[LRU_GEN_CORE]);

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		int nid;

		for_each_node(nid) {
			struct lruvec *lruvec = get_lruvec(memcg, nid);

			if (!lruvec)
				continue;

			spin_lock_irq(&lruvec->lru_lock);

			VM_WARN_ON_ONCE(!seq_is_valid(lruvec));
			VM_WARN_ON_ONCE(!state_is_valid(lruvec));

			lruvec->lrugen.enabled = enabled;

			while (!(enabled ? fill_evictable(lruvec) : drain_evictable(lruvec))) {
				spin_unlock_irq(&lruvec->lru_lock);
				cond_resched();
				spin_lock_irq(&lruvec->lru_lock);
			}

			spin_unlock_irq(&lruvec->lru_lock);
		}

		cond_resched();
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));
unlock:
	mutex_unlock(&state_mutex);
	put_online_mems();
	cpus_read_unlock();
	cgroup_unlock();
}

/******************************************************************************
 *                          sysfs interface
 ******************************************************************************/

static ssize_t show_min_ttl(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", jiffies_to_msecs(READ_ONCE(lru_gen_min_ttl)));
}

/* see Documentation/admin-guide/mm/multigen_lru.rst for details */
static ssize_t store_min_ttl(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t len)
{
	unsigned int msecs;

	if (kstrtouint(buf, 0, &msecs))
		return -EINVAL;

	WRITE_ONCE(lru_gen_min_ttl, msecs_to_jiffies(msecs));

	return len;
}

static struct kobj_attribute lru_gen_min_ttl_attr = __ATTR(
	min_ttl_ms, 0644, show_min_ttl, store_min_ttl
);

static ssize_t show_enabled(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	unsigned int caps = 0;

	if (get_cap(LRU_GEN_CORE))
		caps |= BIT(LRU_GEN_CORE);

	if (arch_has_hw_pte_young() && get_cap(LRU_GEN_MM_WALK))
		caps |= BIT(LRU_GEN_MM_WALK);

	if (IS_ENABLED(CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG) && get_cap(LRU_GEN_NONLEAF_YOUNG))
		caps |= BIT(LRU_GEN_NONLEAF_YOUNG);

	return snprintf(buf, PAGE_SIZE, "0x%04x\n", caps);
}

/* see Documentation/admin-guide/mm/multigen_lru.rst for details */
static ssize_t store_enabled(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t len)
{
	int i;
	unsigned int caps;

#ifdef CONFIG_CONT_PTE_HUGEPAGE
	return -EINVAL;
#endif

	if (tolower(*buf) == 'n')
		caps = 0;
	else if (tolower(*buf) == 'y')
		caps = -1;
	else if (kstrtouint(buf, 0, &caps))
		return -EINVAL;

	for (i = 0; i < NR_LRU_GEN_CAPS; i++) {
		bool enabled = caps & BIT(i);

		if (i == LRU_GEN_CORE)
			lru_gen_change_state(enabled);
		else if (enabled)
			static_branch_enable(&lru_gen_caps[i]);
		else
			static_branch_disable(&lru_gen_caps[i]);
	}

	return len;
}

static struct kobj_attribute lru_gen_enabled_attr = __ATTR(
	enabled, 0644, show_enabled, store_enabled
);

static struct attribute *lru_gen_attrs[] = {
	&lru_gen_min_ttl_attr.attr,
	&lru_gen_enabled_attr.attr,
	NULL
};

static struct attribute_group lru_gen_attr_group = {
	.name = "lru_gen",
	.attrs = lru_gen_attrs,
};

/******************************************************************************
 *                          debugfs interface
 ******************************************************************************/

static void *lru_gen_seq_start(struct seq_file *m, loff_t *pos)
{
	struct mem_cgroup *memcg;
	loff_t nr_to_skip = *pos;

	m->private = kvmalloc(PATH_MAX, GFP_KERNEL);
	if (!m->private)
		return ERR_PTR(-ENOMEM);

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		int nid;

		for_each_node_state(nid, N_MEMORY) {
			if (!nr_to_skip--)
				return get_lruvec(memcg, nid);
		}
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));

	return NULL;
}

static void lru_gen_seq_stop(struct seq_file *m, void *v)
{
	if (!IS_ERR_OR_NULL(v))
		mem_cgroup_iter_break(NULL, lruvec_memcg(v));

	kvfree(m->private);
	m->private = NULL;
}

static void *lru_gen_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	int nid = lruvec_pgdat(v)->node_id;
	struct mem_cgroup *memcg = lruvec_memcg(v);

	++*pos;

	nid = next_memory_node(nid);
	if (nid == MAX_NUMNODES) {
		memcg = mem_cgroup_iter(NULL, memcg, NULL);
		if (!memcg)
			return NULL;

		nid = first_memory_node;
	}

	return get_lruvec(memcg, nid);
}

static void lru_gen_seq_show_full(struct seq_file *m, struct lruvec *lruvec,
				  unsigned long max_seq, unsigned long *min_seq,
				  unsigned long seq)
{
	int i;
	int type, tier;
	int hist = lru_hist_from_seq(seq);
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	for (tier = 0; tier < MAX_NR_TIERS; tier++) {
		seq_printf(m, "            %10d", tier);
		for (type = 0; type < ANON_AND_FILE; type++) {
			const char *s = "   ";
			unsigned long n[3] = {};

			if (seq == max_seq) {
				s = "RT ";
				n[0] = READ_ONCE(lrugen->avg_refaulted[type][tier]);
				n[1] = READ_ONCE(lrugen->avg_total[type][tier]);
			} else if (seq == min_seq[type] || NR_HIST_GENS > 1) {
				s = "rep";
				n[0] = atomic_long_read(&lrugen->refaulted[hist][type][tier]);
				n[1] = atomic_long_read(&lrugen->evicted[hist][type][tier]);
				if (tier)
					n[2] = READ_ONCE(lrugen->protected[hist][type][tier - 1]);
			}

			for (i = 0; i < 3; i++)
				seq_printf(m, " %10lu%c", n[i], s[i]);
		}
		seq_putc(m, '\n');
	}

	seq_puts(m, "                      ");
	for (i = 0; i < NR_MM_STATS; i++) {
		const char *s = "      ";
		unsigned long n = 0;

		if (seq == max_seq && NR_HIST_GENS == 1) {
			s = "LOYNFA";
			n = READ_ONCE(lruvec->mm_state.stats[hist][i]);
		} else if (seq != max_seq && NR_HIST_GENS > 1) {
			s = "loynfa";
			n = READ_ONCE(lruvec->mm_state.stats[hist][i]);
		}

		seq_printf(m, " %10lu%c", n, s[i]);
	}
	seq_putc(m, '\n');
}

/* see Documentation/admin-guide/mm/multigen_lru.rst for details */
static int lru_gen_seq_show(struct seq_file *m, void *v)
{
	unsigned long seq;
	bool full = !debugfs_real_fops(m->file)->write;
	struct lruvec *lruvec = v;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;
	int nid = lruvec_pgdat(lruvec)->node_id;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);
	DEFINE_MAX_SEQ(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	if (nid == first_memory_node) {
		const char *path = memcg ? m->private : "";

#ifdef CONFIG_MEMCG
		if (memcg)
			cgroup_path(memcg->css.cgroup, m->private, PATH_MAX);
#endif
		seq_printf(m, "memcg %5hu %s\n", mem_cgroup_id(memcg), path);
	}

	seq_printf(m, " node %5d\n", nid);

	if (!full)
		seq = min_seq[LRU_GEN_ANON];
	else if (max_seq >= MAX_NR_GENS)
		seq = max_seq - MAX_NR_GENS + 1;
	else
		seq = 0;

	for (; seq <= max_seq; seq++) {
		int type, zone;
		int gen = lru_gen_from_seq(seq);
		unsigned long birth = READ_ONCE(lruvec->lrugen.timestamps[gen]);

		seq_printf(m, " %10lu %10u", seq, jiffies_to_msecs(jiffies - birth));

		for (type = 0; type < ANON_AND_FILE; type++) {
			unsigned long size = 0;
			char mark = full && seq < min_seq[type] ? 'x' : ' ';

			for (zone = 0; zone < MAX_NR_ZONES; zone++)
				size += max_t(long, READ_ONCE(lrugen->nr_pages[gen][type][zone]),
						0);

			seq_printf(m, " %10lu%c", size, mark);
		}

		seq_putc(m, '\n');

		if (full)
			lru_gen_seq_show_full(m, lruvec, max_seq, min_seq, seq);
	}

	return 0;
}

static const struct seq_operations lru_gen_seq_ops = {
	.start = lru_gen_seq_start,
	.stop = lru_gen_seq_stop,
	.next = lru_gen_seq_next,
	.show = lru_gen_seq_show,
};

static int run_aging(struct lruvec *lruvec, unsigned long seq, struct scan_control *sc,
		     bool can_swap, bool full_scan)
{
	DEFINE_MAX_SEQ(lruvec);
	DEFINE_MIN_SEQ(lruvec);

	if (seq < max_seq)
		return 0;

	if (seq > max_seq)
		return -EINVAL;

	if (!full_scan && min_seq[!can_swap] + MAX_NR_GENS - 1 <= max_seq)
		return -ERANGE;

	try_to_inc_max_seq(lruvec, max_seq, sc, can_swap, full_scan);

	return 0;
}

static int run_eviction(struct lruvec *lruvec, unsigned long seq, struct scan_control *sc,
			int swappiness, unsigned long nr_to_reclaim)
{
	DEFINE_MAX_SEQ(lruvec);

	if (seq + MIN_NR_GENS > max_seq)
		return -EINVAL;

	sc->nr_reclaimed = 0;

	while (!signal_pending(current)) {
		DEFINE_MIN_SEQ(lruvec);

		if (seq < min_seq[!swappiness])
			return 0;

		if (sc->nr_reclaimed >= nr_to_reclaim)
			return 0;

		if (!evict_pages(lruvec, sc, swappiness, NULL))
			return 0;

		cond_resched();
	}

	return -EINTR;
}

static int run_cmd(char cmd, int memcg_id, int nid, unsigned long seq,
		   struct scan_control *sc, int swappiness, unsigned long opt)
{
	struct lruvec *lruvec;
	int err = -EINVAL;
	struct mem_cgroup *memcg = NULL;

	if (nid < 0 || nid >= MAX_NUMNODES || !node_state(nid, N_MEMORY))
		return -EINVAL;

	if (!mem_cgroup_disabled()) {
		rcu_read_lock();
		memcg = mem_cgroup_from_id(memcg_id);
#ifdef CONFIG_MEMCG
		if (memcg && !css_tryget(&memcg->css))
			memcg = NULL;
#endif
		rcu_read_unlock();

		if (!memcg)
			return -EINVAL;
	}

	if (memcg_id != mem_cgroup_id(memcg))
		goto done;

	lruvec = get_lruvec(memcg, nid);

	if (swappiness < 0)
		swappiness = get_swappiness(lruvec, sc);
	else if (swappiness > 200)
		goto done;

	switch (cmd) {
	case '+':
		err = run_aging(lruvec, seq, sc, swappiness, opt);
		break;
	case '-':
		err = run_eviction(lruvec, seq, sc, swappiness, opt);
		break;
	}
done:
	mem_cgroup_put(memcg);

	return err;
}

/* see Documentation/admin-guide/mm/multigen_lru.rst for details */
static ssize_t lru_gen_seq_write(struct file *file, const char __user *src,
				 size_t len, loff_t *pos)
{
	void *buf;
	char *cur, *next;
	unsigned int flags;
	struct blk_plug plug;
	int err = -EINVAL;
	struct scan_control sc = {
		.may_writepage = true,
		.may_unmap = true,
		.may_swap = true,
		.reclaim_idx = MAX_NR_ZONES - 1,
		.gfp_mask = GFP_KERNEL,
	};

	buf = kvmalloc(len + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, src, len)) {
		kvfree(buf);
		return -EFAULT;
	}

	set_task_reclaim_state(current, &sc.reclaim_state);
	flags = memalloc_noreclaim_save();
	blk_start_plug(&plug);
	if (!set_mm_walk(NULL)) {
		err = -ENOMEM;
		goto done;
	}

	next = buf;
	next[len] = '\0';

	while ((cur = strsep(&next, ",;\n"))) {
		int n;
		int end;
		char cmd;
		unsigned int memcg_id;
		unsigned int nid;
		unsigned long seq;
		unsigned int swappiness = -1;
		unsigned long opt = -1;

		cur = skip_spaces(cur);
		if (!*cur)
			continue;

		n = sscanf(cur, "%c %u %u %lu %n %u %n %lu %n", &cmd, &memcg_id, &nid,
			   &seq, &end, &swappiness, &end, &opt, &end);
		if (n < 4 || cur[end]) {
			err = -EINVAL;
			break;
		}

		err = run_cmd(cmd, memcg_id, nid, seq, &sc, swappiness, opt);
		if (err)
			break;
	}
done:
	clear_mm_walk();
	blk_finish_plug(&plug);
	memalloc_noreclaim_restore(flags);
	set_task_reclaim_state(current, NULL);

	kvfree(buf);

	return err ? : len;
}

static int lru_gen_seq_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &lru_gen_seq_ops);
}

static const struct file_operations lru_gen_rw_fops = {
	.open = lru_gen_seq_open,
	.read = seq_read,
	.write = lru_gen_seq_write,
	.llseek = seq_lseek,
	.release = seq_release,
};

static const struct file_operations lru_gen_ro_fops = {
	.open = lru_gen_seq_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

/******************************************************************************
 *                          initialization
 ******************************************************************************/

void lru_gen_init_lruvec(struct lruvec *lruvec)
{
	int i;
	int gen, type, zone;
	struct lru_gen_struct *lrugen = &lruvec->lrugen;

	lrugen->max_seq = MIN_NR_GENS + 1;
	lrugen->enabled = lru_gen_enabled();

	for (i = 0; i <= MIN_NR_GENS + 1; i++)
		lrugen->timestamps[i] = jiffies;

	for_each_gen_type_zone(gen, type, zone)
		INIT_LIST_HEAD(&lrugen->lists[gen][type][zone]);

	lruvec->mm_state.seq = MIN_NR_GENS;
}

#ifdef CONFIG_MEMCG
void lru_gen_init_memcg(struct mem_cgroup *memcg)
{
	INIT_LIST_HEAD(&memcg->mm_list.fifo);
	spin_lock_init(&memcg->mm_list.lock);
}

void lru_gen_exit_memcg(struct mem_cgroup *memcg)
{
	int i;
	int nid;

	for_each_node(nid) {
		struct lruvec *lruvec = get_lruvec(memcg, nid);

		VM_WARN_ON_ONCE(memchr_inv(lruvec->lrugen.nr_pages, 0,
					   sizeof(lruvec->lrugen.nr_pages)));

		for (i = 0; i < NR_BLOOM_FILTERS; i++) {
			bitmap_free(lruvec->mm_state.filters[i]);
			lruvec->mm_state.filters[i] = NULL;
		}
	}
}
#endif

static int __init init_lru_gen(void)
{
	BUILD_BUG_ON(MIN_NR_GENS + 1 >= MAX_NR_GENS);
	BUILD_BUG_ON(BIT(LRU_GEN_WIDTH) <= MAX_NR_GENS);

	if (sysfs_create_group(mm_kobj, &lru_gen_attr_group))
		pr_err("lru_gen: failed to create sysfs group\n");

	debugfs_create_file("lru_gen", 0644, NULL, NULL, &lru_gen_rw_fops);
	debugfs_create_file("lru_gen_full", 0444, NULL, NULL, &lru_gen_ro_fops);

	return 0;
};
late_initcall(init_lru_gen);

#else /* !CONFIG_LRU_GEN */

static void lru_gen_age_node(struct pglist_data *pgdat, struct scan_control *sc)
{
}

static void lru_gen_shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
}

#endif /* CONFIG_LRU_GEN */


#ifdef CONFIG_CONT_PTE_HUGEPAGE
extern bool thp_swap_is_free(void);
#endif

static void shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
	unsigned long nr[NR_LRU_LISTS];
	unsigned long targets[NR_LRU_LISTS];
	unsigned long nr_to_scan;
	enum lru_list lru;
	unsigned long nr_reclaimed = 0;
	unsigned long nr_to_reclaim = sc->nr_to_reclaim;
	bool proportional_reclaim;
	struct blk_plug plug;

	if (lru_gen_enabled()) {
		lru_gen_shrink_lruvec(lruvec, sc);
		return;
	}

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	if (sc->gfp_mask & POOL_USER_ALLOC_MASK) {
		if (!thp_swap_is_free()) {
			pr_err_ratelimited("@FIXME: THP SWAP is full !!!  -> comm:%s pid:%d tgid:%d %s:%d @\n",
					current->comm, current->pid, current->tgid, __func__, __LINE__);
			return;
		}
	}
#endif

	get_scan_count(lruvec, sc, nr);

	/* Record the original scan target for proportional adjustments later */
	memcpy(targets, nr, sizeof(nr));

	/*
	 * Global reclaiming within direct reclaim at DEF_PRIORITY is a normal
	 * event that can occur when there is little memory pressure e.g.
	 * multiple streaming readers/writers. Hence, we do not abort scanning
	 * when the requested number of pages are reclaimed when scanning at
	 * DEF_PRIORITY on the assumption that the fact we are direct
	 * reclaiming implies that kswapd is not keeping up and it is best to
	 * do a batch of work at once. For memcg reclaim one check is made to
	 * abort proportional reclaim if either the file or anon lru has already
	 * dropped to zero at the first pass.
	 */
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	proportional_reclaim = (!cgroup_reclaim(sc) && !current_is_kswapd() &&
				sc->priority == DEF_PRIORITY &&
				!(sc->gfp_mask & POOL_USER_ALLOC_MASK));
#else
	proportional_reclaim = (!cgroup_reclaim(sc) && !current_is_kswapd() &&
				sc->priority == DEF_PRIORITY);
#endif

	blk_start_plug(&plug);
	while (nr[LRU_INACTIVE_ANON] || nr[LRU_ACTIVE_FILE] ||
					nr[LRU_INACTIVE_FILE]) {
		unsigned long nr_anon, nr_file, percentage;
		unsigned long nr_scanned;

		for_each_evictable_lru(lru) {
			if (nr[lru]) {
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
				if (sc->gfp_mask & POOL_USER_ALLOC_MASK)
					nr_to_scan = min(nr[lru], CHP_SWAP_CLUSTER_MAX);
				else
#endif
					nr_to_scan = min(nr[lru], SWAP_CLUSTER_MAX);
				nr[lru] -= nr_to_scan;

				nr_reclaimed += shrink_list(lru, nr_to_scan,
							    lruvec, sc);
			}
		}

		cond_resched();

		if (nr_reclaimed < nr_to_reclaim || proportional_reclaim)
			continue;

		/*
		 * For kswapd and memcg, reclaim at least the number of pages
		 * requested. Ensure that the anon and file LRUs are scanned
		 * proportionally what was requested by get_scan_count(). We
		 * stop reclaiming one LRU and reduce the amount scanning
		 * proportional to the original scan target.
		 */
		nr_file = nr[LRU_INACTIVE_FILE] + nr[LRU_ACTIVE_FILE];
		nr_anon = nr[LRU_INACTIVE_ANON] + nr[LRU_ACTIVE_ANON];

		/*
		 * It's just vindictive to attack the larger once the smaller
		 * has gone to zero.  And given the way we stop scanning the
		 * smaller below, this makes sure that we only make one nudge
		 * towards proportionality once we've got nr_to_reclaim.
		 */
		if (!nr_file || !nr_anon)
			break;

		if (nr_file > nr_anon) {
			unsigned long scan_target = targets[LRU_INACTIVE_ANON] +
						targets[LRU_ACTIVE_ANON] + 1;
			lru = LRU_BASE;
			percentage = nr_anon * 100 / scan_target;
		} else {
			unsigned long scan_target = targets[LRU_INACTIVE_FILE] +
						targets[LRU_ACTIVE_FILE] + 1;
			lru = LRU_FILE;
			percentage = nr_file * 100 / scan_target;
		}

		/* Stop scanning the smaller of the LRU */
		nr[lru] = 0;
		nr[lru + LRU_ACTIVE] = 0;

		/*
		 * Recalculate the other LRU scan count based on its original
		 * scan target and the percentage scanning already complete
		 */
		lru = (lru == LRU_FILE) ? LRU_BASE : LRU_FILE;
		nr_scanned = targets[lru] - nr[lru];
		nr[lru] = targets[lru] * (100 - percentage) / 100;
		nr[lru] -= min(nr[lru], nr_scanned);

		lru += LRU_ACTIVE;
		nr_scanned = targets[lru] - nr[lru];
		nr[lru] = targets[lru] * (100 - percentage) / 100;
		nr[lru] -= min(nr[lru], nr_scanned);
	}
	blk_finish_plug(&plug);
	sc->nr_reclaimed += nr_reclaimed;

	/*
	 * Even if we did not try to evict anon pages at all, we want to
	 * rebalance the anon lru active/inactive ratio.
	 */
	if (can_age_anon_pages(lruvec_pgdat(lruvec), sc) &&
	    inactive_is_low(lruvec, LRU_INACTIVE_ANON)) {
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
		if (sc->gfp_mask & POOL_USER_ALLOC_MASK)
			shrink_active_list(CHP_SWAP_CLUSTER_MAX, lruvec,
					sc, LRU_ACTIVE_ANON);
		else
#endif
			shrink_active_list(SWAP_CLUSTER_MAX, lruvec,
				   sc, LRU_ACTIVE_ANON);
	}
}

/* Use reclaim/compaction for costly allocs or under memory pressure */
static bool in_reclaim_compaction(struct scan_control *sc)
{
	if (IS_ENABLED(CONFIG_COMPACTION) && sc->order &&
			(sc->order > PAGE_ALLOC_COSTLY_ORDER ||
			 sc->priority < DEF_PRIORITY - 2))
		return true;

	return false;
}

/*
 * Reclaim/compaction is used for high-order allocation requests. It reclaims
 * order-0 pages before compacting the zone. should_continue_reclaim() returns
 * true if more pages should be reclaimed such that when the page allocator
 * calls try_to_compact_pages() that it will have enough free pages to succeed.
 * It will give up earlier than that if there is difficulty reclaiming pages.
 */
static inline bool should_continue_reclaim(struct pglist_data *pgdat,
					unsigned long nr_reclaimed,
					struct scan_control *sc)
{
	unsigned long pages_for_compaction;
	unsigned long inactive_lru_pages;
	int z;

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	/*
	 * cont-pte cma pool reclamation, we don't depend on compaction as we are
	 * not buddy pages. In the other words, in_reclaim_compaction() is false
	 * just simply like 0-order pages
	 */
	if (sc->gfp_mask & POOL_USER_ALLOC_MASK)
		return false;
#endif

	/* If not in reclaim/compaction mode, stop */
	if (!in_reclaim_compaction(sc))
		return false;

	/*
	 * Stop if we failed to reclaim any pages from the last SWAP_CLUSTER_MAX
	 * number of pages that were scanned. This will return to the caller
	 * with the risk reclaim/compaction and the resulting allocation attempt
	 * fails. In the past we have tried harder for __GFP_RETRY_MAYFAIL
	 * allocations through requiring that the full LRU list has been scanned
	 * first, by assuming that zero delta of sc->nr_scanned means full LRU
	 * scan, but that approximation was wrong, and there were corner cases
	 * where always a non-zero amount of pages were scanned.
	 */
	if (!nr_reclaimed)
		return false;

	/* If compaction would go ahead or the allocation would succeed, stop */
	for (z = 0; z <= sc->reclaim_idx; z++) {
		struct zone *zone = &pgdat->node_zones[z];
		if (!managed_zone(zone))
			continue;

		switch (compaction_suitable(zone, sc->order, 0, sc->reclaim_idx)) {
		case COMPACT_SUCCESS:
		case COMPACT_CONTINUE:
			return false;
		default:
			/* check next zone */
			;
		}
	}

	/*
	 * If we have not reclaimed enough pages for compaction and the
	 * inactive lists are large enough, continue reclaiming
	 */
	pages_for_compaction = compact_gap(sc->order);
	inactive_lru_pages = node_page_state(pgdat, NR_INACTIVE_FILE);
	if (can_reclaim_anon_pages(NULL, pgdat->node_id, sc))
		inactive_lru_pages += node_page_state(pgdat, NR_INACTIVE_ANON);

	return inactive_lru_pages > pages_for_compaction;
}

static void shrink_node_memcgs(pg_data_t *pgdat, struct scan_control *sc)
{
	struct mem_cgroup *target_memcg = sc->target_mem_cgroup;
	struct mem_cgroup *memcg;

	memcg = mem_cgroup_iter(target_memcg, NULL, NULL);
	do {
		struct lruvec *lruvec;
		unsigned long reclaimed;
		unsigned long scanned;
		bool skip = false;

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
		if (sc->gfp_mask & POOL_USER_ALLOC_MASK)
			lruvec = mem_cgroup_chp_lruvec(memcg, pgdat);
		else
#endif
			lruvec = mem_cgroup_lruvec(memcg, pgdat);
		/*
		 * This loop can become CPU-bound when target memcgs
		 * aren't eligible for reclaim - either because they
		 * don't have any reclaimable pages, or because their
		 * memory is explicitly protected. Avoid soft lockups.
		 */
		cond_resched();

		trace_android_vh_shrink_node_memcgs(memcg, &skip);
		if (skip)
			continue;

		mem_cgroup_calculate_protection(target_memcg, memcg);

		if (mem_cgroup_below_min(memcg)) {
			/*
			 * Hard protection.
			 * If there is no reclaimable memory, OOM.
			 */
			continue;
		} else if (mem_cgroup_below_low(memcg)) {
			/*
			 * Soft protection.
			 * Respect the protection only as long as
			 * there is an unprotected supply
			 * of reclaimable memory from other cgroups.
			 */
			if (!sc->memcg_low_reclaim) {
				sc->memcg_low_skipped = 1;
				continue;
			}
			memcg_memory_event(memcg, MEMCG_LOW);
		}

		reclaimed = sc->nr_reclaimed;
		scanned = sc->nr_scanned;

		shrink_lruvec(lruvec, sc);

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
		/* Don't reclaim slab for cont-pte huegpage */
		if (!(sc->gfp_mask & POOL_USER_ALLOC_MASK))
			shrink_slab(sc->gfp_mask, pgdat->node_id, memcg,
				sc->priority);
#else
		shrink_slab(sc->gfp_mask, pgdat->node_id, memcg,
			    sc->priority);
#endif

		/* Record the group's reclaim efficiency */
		vmpressure(sc->gfp_mask, memcg, false,
			   sc->nr_scanned - scanned,
			   sc->nr_reclaimed - reclaimed);

	} while ((memcg = mem_cgroup_iter(target_memcg, memcg, NULL)));
}

static void shrink_node(pg_data_t *pgdat, struct scan_control *sc)
{
	struct reclaim_state *reclaim_state = current->reclaim_state;
	unsigned long nr_reclaimed, nr_scanned;
	struct lruvec *target_lruvec;
	bool reclaimable = false;

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
	if (sc->gfp_mask & POOL_USER_ALLOC_MASK)
		target_lruvec = mem_cgroup_chp_lruvec(sc->target_mem_cgroup, pgdat);
	else
#endif
		target_lruvec = mem_cgroup_lruvec(sc->target_mem_cgroup, pgdat);

again:
	memset(&sc->nr, 0, sizeof(sc->nr));

	nr_reclaimed = sc->nr_reclaimed;
	nr_scanned = sc->nr_scanned;

	prepare_scan_count(pgdat, sc);

	shrink_node_memcgs(pgdat, sc);

	if (reclaim_state) {
		sc->nr_reclaimed += reclaim_state->reclaimed_slab;
		reclaim_state->reclaimed_slab = 0;
	}

	/* Record the subtree's reclaim efficiency */
	vmpressure(sc->gfp_mask, sc->target_mem_cgroup, true,
		   sc->nr_scanned - nr_scanned,
		   sc->nr_reclaimed - nr_reclaimed);

	if (sc->nr_reclaimed - nr_reclaimed)
		reclaimable = true;

	if (current_is_kswapd()) {
		/*
		 * If reclaim is isolating dirty pages under writeback,
		 * it implies that the long-lived page allocation rate
		 * is exceeding the page laundering rate. Either the
		 * global limits are not being effective at throttling
		 * processes due to the page distribution throughout
		 * zones or there is heavy usage of a slow backing
		 * device. The only option is to throttle from reclaim
		 * context which is not ideal as there is no guarantee
		 * the dirtying process is throttled in the same way
		 * balance_dirty_pages() manages.
		 *
		 * Once a node is flagged PGDAT_WRITEBACK, kswapd will
		 * count the number of pages under pages flagged for
		 * immediate reclaim and stall if any are encountered
		 * in the nr_immediate check below.
		 */
		if (sc->nr.writeback && sc->nr.writeback == sc->nr.taken)
			set_bit(PGDAT_WRITEBACK, &pgdat->flags);

		/* Allow kswapd to start writing pages during reclaim.*/
		if (sc->nr.unqueued_dirty == sc->nr.file_taken)
			set_bit(PGDAT_DIRTY, &pgdat->flags);

		/*
		 * If kswapd scans pages marked for immediate
		 * reclaim and under writeback (nr_immediate), it
		 * implies that pages are cycling through the LRU
		 * faster than they are written so also forcibly stall.
		 */
		if (sc->nr.immediate)
			congestion_wait(BLK_RW_ASYNC, HZ/10);
	}

	/*
	 * Tag a node/memcg as congested if all the dirty pages
	 * scanned were backed by a congested BDI and
	 * wait_iff_congested will stall.
	 *
	 * Legacy memcg will stall in page writeback so avoid forcibly
	 * stalling in wait_iff_congested().
	 */
	if ((current_is_kswapd() ||
	     (cgroup_reclaim(sc) && writeback_throttling_sane(sc))) &&
	    sc->nr.dirty && sc->nr.dirty == sc->nr.congested)
		set_bit(LRUVEC_CONGESTED, &target_lruvec->flags);

	/*
	 * Stall direct reclaim for IO completions if underlying BDIs
	 * and node is congested. Allow kswapd to continue until it
	 * starts encountering unqueued dirty pages or cycling through
	 * the LRU too quickly.
	 */
	if (!current_is_kswapd() && current_may_throttle() &&
	    !sc->hibernation_mode &&
	    test_bit(LRUVEC_CONGESTED, &target_lruvec->flags))
		wait_iff_congested(BLK_RW_ASYNC, HZ/10);

	if (should_continue_reclaim(pgdat, sc->nr_reclaimed - nr_reclaimed,
				    sc))
		goto again;

	/*
	 * Kswapd gives up on balancing particular nodes after too
	 * many failures to reclaim anything from them and goes to
	 * sleep. On reclaim progress, reset the failure counter. A
	 * successful direct reclaim run will revive a dormant kswapd.
	 */
	if (reclaimable)
		pgdat->kswapd_failures = 0;
}

/*
 * Returns true if compaction should go ahead for a costly-order request, or
 * the allocation would already succeed without compaction. Return false if we
 * should reclaim first.
 */
static inline bool compaction_ready(struct zone *zone, struct scan_control *sc)
{
	unsigned long watermark;
	enum compact_result suitable;

	suitable = compaction_suitable(zone, sc->order, 0, sc->reclaim_idx);
	if (suitable == COMPACT_SUCCESS)
		/* Allocation should succeed already. Don't reclaim. */
		return true;
	if (suitable == COMPACT_SKIPPED)
		/* Compaction cannot yet proceed. Do reclaim. */
		return false;

	/*
	 * Compaction is already possible, but it takes time to run and there
	 * are potentially other callers using the pages just freed. So proceed
	 * with reclaim to make a buffer of free pages available to give
	 * compaction a reasonable chance of completing and allocating the page.
	 * Note that we won't actually reclaim the whole buffer in one attempt
	 * as the target watermark in should_continue_reclaim() is lower. But if
	 * we are already above the high+gap watermark, don't reclaim at all.
	 */
	watermark = high_wmark_pages(zone) + compact_gap(sc->order);

	return zone_watermark_ok_safe(zone, 0, watermark, sc->reclaim_idx);
}

/*
 * This is the direct reclaim path, for page-allocating processes.  We only
 * try to reclaim pages from zones which will satisfy the caller's allocation
 * request.
 *
 * If a zone is deemed to be full of pinned pages then just give it a light
 * scan then give up on it.
 */
static void shrink_zones(struct zonelist *zonelist, struct scan_control *sc)
{
	struct zoneref *z;
	struct zone *zone;
	unsigned long nr_soft_reclaimed;
	unsigned long nr_soft_scanned;
	gfp_t orig_mask;
	pg_data_t *last_pgdat = NULL;

	/*
	 * If the number of buffer_heads in the machine exceeds the maximum
	 * allowed level, force direct reclaim to scan the highmem zone as
	 * highmem pages could be pinning lowmem pages storing buffer_heads
	 */
	orig_mask = sc->gfp_mask;
	if (buffer_heads_over_limit) {
		sc->gfp_mask |= __GFP_HIGHMEM;
		sc->reclaim_idx = gfp_zone(sc->gfp_mask);
	}

	for_each_zone_zonelist_nodemask(zone, z, zonelist,
					sc->reclaim_idx, sc->nodemask) {
		/*
		 * Take care memory controller reclaiming has small influence
		 * to global LRU.
		 */
		if (!cgroup_reclaim(sc)) {
			if (!cpuset_zone_allowed(zone,
						 GFP_KERNEL | __GFP_HARDWALL))
				continue;

			/*
			 * If we already have plenty of memory free for
			 * compaction in this zone, don't free any more.
			 * Even though compaction is invoked for any
			 * non-zero order, only frequent costly order
			 * reclamation is disruptive enough to become a
			 * noticeable problem, like transparent huge
			 * page allocations.
			 */
			if (IS_ENABLED(CONFIG_COMPACTION) &&
			    sc->order > PAGE_ALLOC_COSTLY_ORDER &&
			    compaction_ready(zone, sc)) {
				sc->compaction_ready = true;
				continue;
			}

			/*
			 * Shrink each node in the zonelist once. If the
			 * zonelist is ordered by zone (not the default) then a
			 * node may be shrunk multiple times but in that case
			 * the user prefers lower zones being preserved.
			 */
			if (zone->zone_pgdat == last_pgdat)
				continue;

			/*
			 * This steals pages from memory cgroups over softlimit
			 * and returns the number of reclaimed pages and
			 * scanned pages. This works for global memory pressure
			 * and balancing, not for a memcg's limit.
			 */
			nr_soft_scanned = 0;
			nr_soft_reclaimed = mem_cgroup_soft_limit_reclaim(zone->zone_pgdat,
						sc->order, sc->gfp_mask,
						&nr_soft_scanned);
			sc->nr_reclaimed += nr_soft_reclaimed;
			sc->nr_scanned += nr_soft_scanned;
			/* need some check for avoid more shrink_zone() */
		}

		/* See comment about same check for global reclaim above */
		if (zone->zone_pgdat == last_pgdat)
			continue;
		last_pgdat = zone->zone_pgdat;
		shrink_node(zone->zone_pgdat, sc);
	}

	/*
	 * Restore to original mask to avoid the impact on the caller if we
	 * promoted it to __GFP_HIGHMEM.
	 */
	sc->gfp_mask = orig_mask;
}

static void snapshot_refaults(struct mem_cgroup *target_memcg, pg_data_t *pgdat)
{
	struct lruvec *target_lruvec;
	unsigned long refaults;

	if (lru_gen_enabled())
		return;

	target_lruvec = mem_cgroup_lruvec(target_memcg, pgdat);
	refaults = lruvec_page_state(target_lruvec, WORKINGSET_ACTIVATE_ANON);
	target_lruvec->refaults[0] = refaults;
	refaults = lruvec_page_state(target_lruvec, WORKINGSET_ACTIVATE_FILE);
	target_lruvec->refaults[1] = refaults;
}

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
static void snapshot_chp_refaults(struct mem_cgroup *target_memcg, pg_data_t *pgdat)
{
	struct lruvec *target_lruvec;
	unsigned long refaults;

	target_lruvec = mem_cgroup_chp_lruvec(target_memcg, pgdat);
	refaults = lruvec_page_state(target_lruvec, WORKINGSET_ACTIVATE_ANON);
	target_lruvec->refaults[0] = refaults;
	refaults = lruvec_page_state(target_lruvec, WORKINGSET_ACTIVATE_FILE);
	target_lruvec->refaults[1] = refaults;
	//trace_android_vh_snapshot_refaults(target_lruvec); /* Fix compile error */
}
#endif

/*
 * This is the main entry point to direct page reclaim.
 *
 * If a full scan of the inactive list fails to free enough memory then we
 * are "out of memory" and something needs to be killed.
 *
 * If the caller is !__GFP_FS then the probability of a failure is reasonably
 * high - the zone may be full of dirty or under-writeback pages, which this
 * caller can't do much about.  We kick the writeback threads and take explicit
 * naps in the hope that some of these pages can be written.  But if the
 * allocating task holds filesystem locks which prevent writeout this might not
 * work, and the allocation attempt will fail.
 *
 * returns:	0, if no pages reclaimed
 * 		else, the number of pages reclaimed
 */
static unsigned long do_try_to_free_pages(struct zonelist *zonelist,
					  struct scan_control *sc)
{
	int initial_priority = sc->priority;
	pg_data_t *last_pgdat;
	struct zoneref *z;
	struct zone *zone;
retry:
	delayacct_freepages_start();

	if (!cgroup_reclaim(sc))
		__count_zid_vm_events(ALLOCSTALL, sc->reclaim_idx, 1);

	do {
		vmpressure_prio(sc->gfp_mask, sc->target_mem_cgroup,
				sc->priority);
		sc->nr_scanned = 0;
		shrink_zones(zonelist, sc);

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
		if (sc->gfp_mask & POOL_USER_ALLOC_MASK &&
		    !current_is_hybridswapd()) {
			struct huge_page_pool *pool = cont_pte_pool();
			/*
			 * When thp's swap has a free slot,
			 * we end the reclaim loop.
			 * FIXME: support file thp limit?
			 */
			if (!thp_swap_is_free() ||
			    huge_page_pool_count(pool, HPAGE_POOL_CMA) >
			    pool->wmark[POOL_WMARK_MIN])
				break;
		}
#endif
		if (sc->nr_reclaimed >= sc->nr_to_reclaim)
			break;

		if (sc->compaction_ready)
			break;

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
		if (current_is_hybridswapd() && sc->priority < 2)
			break;
#endif

		/*
		 * If we're getting trouble reclaiming, start doing
		 * writepage even in laptop mode.
		 */
		if (sc->priority < DEF_PRIORITY - 2)
			sc->may_writepage = 1;
	} while (--sc->priority >= 0);

	last_pgdat = NULL;
	for_each_zone_zonelist_nodemask(zone, z, zonelist, sc->reclaim_idx,
					sc->nodemask) {
		if (zone->zone_pgdat == last_pgdat)
			continue;
		last_pgdat = zone->zone_pgdat;

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
		if (sc->gfp_mask & POOL_USER_ALLOC_MASK)
			snapshot_chp_refaults(sc->target_mem_cgroup, zone->zone_pgdat);
		else
#endif
			snapshot_refaults(sc->target_mem_cgroup, zone->zone_pgdat);

		if (cgroup_reclaim(sc)) {
			struct lruvec *lruvec;

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
			if (sc->gfp_mask & POOL_USER_ALLOC_MASK)
				lruvec = mem_cgroup_chp_lruvec(sc->target_mem_cgroup,
						zone->zone_pgdat);
			else
#endif
				lruvec = mem_cgroup_lruvec(sc->target_mem_cgroup,
						zone->zone_pgdat);
			clear_bit(LRUVEC_CONGESTED, &lruvec->flags);
		}
	}

	delayacct_freepages_end();

	if (sc->nr_reclaimed)
		return sc->nr_reclaimed;

	/* Aborted reclaim to try compaction? don't OOM, then */
	if (sc->compaction_ready)
		return 1;

	/*
	 * We make inactive:active ratio decisions based on the node's
	 * composition of memory, but a restrictive reclaim_idx or a
	 * memory.low cgroup setting can exempt large amounts of
	 * memory from reclaim. Neither of which are very common, so
	 * instead of doing costly eligibility calculations of the
	 * entire cgroup subtree up front, we assume the estimates are
	 * good, and retry with forcible deactivation if that fails.
	 */
	if (sc->skipped_deactivate) {
		sc->priority = initial_priority;
		sc->force_deactivate = 1;
		sc->skipped_deactivate = 0;
		goto retry;
	}

	/* Untapped cgroup reserves?  Don't OOM, retry. */
	if (sc->memcg_low_skipped) {
		sc->priority = initial_priority;
		sc->force_deactivate = 0;
		sc->memcg_low_reclaim = 1;
		sc->memcg_low_skipped = 0;
		goto retry;
	}

	return 0;
}

static bool allow_direct_reclaim(pg_data_t *pgdat)
{
	struct zone *zone;
	unsigned long pfmemalloc_reserve = 0;
	unsigned long free_pages = 0;
	int i;
	bool wmark_ok;

	if (pgdat->kswapd_failures >= MAX_RECLAIM_RETRIES)
		return true;

	for (i = 0; i <= ZONE_NORMAL; i++) {
		zone = &pgdat->node_zones[i];
		if (!managed_zone(zone))
			continue;

		if (!zone_reclaimable_pages(zone))
			continue;

		pfmemalloc_reserve += min_wmark_pages(zone);
		free_pages += zone_page_state(zone, NR_FREE_PAGES);
	}

	/* If there are no reserves (unexpected config) then do not throttle */
	if (!pfmemalloc_reserve)
		return true;

	wmark_ok = free_pages > pfmemalloc_reserve / 2;

	/* kswapd must be awake if processes are being throttled */
	if (!wmark_ok && waitqueue_active(&pgdat->kswapd_wait)) {
		if (READ_ONCE(pgdat->kswapd_highest_zoneidx) > ZONE_NORMAL)
			WRITE_ONCE(pgdat->kswapd_highest_zoneidx, ZONE_NORMAL);

		wake_up_interruptible(&pgdat->kswapd_wait);
	}

	return wmark_ok;
}

/*
 * Throttle direct reclaimers if backing storage is backed by the network
 * and the PFMEMALLOC reserve for the preferred node is getting dangerously
 * depleted. kswapd will continue to make progress and wake the processes
 * when the low watermark is reached.
 *
 * Returns true if a fatal signal was delivered during throttling. If this
 * happens, the page allocator should not consider triggering the OOM killer.
 */
static bool throttle_direct_reclaim(gfp_t gfp_mask, struct zonelist *zonelist,
					nodemask_t *nodemask)
{
	struct zoneref *z;
	struct zone *zone;
	pg_data_t *pgdat = NULL;

	/*
	 * Kernel threads should not be throttled as they may be indirectly
	 * responsible for cleaning pages necessary for reclaim to make forward
	 * progress. kjournald for example may enter direct reclaim while
	 * committing a transaction where throttling it could forcing other
	 * processes to block on log_wait_commit().
	 */
	if (current->flags & PF_KTHREAD)
		goto out;

	/*
	 * If a fatal signal is pending, this process should not throttle.
	 * It should return quickly so it can exit and free its memory
	 */
	if (fatal_signal_pending(current))
		goto out;

	/*
	 * Check if the pfmemalloc reserves are ok by finding the first node
	 * with a usable ZONE_NORMAL or lower zone. The expectation is that
	 * GFP_KERNEL will be required for allocating network buffers when
	 * swapping over the network so ZONE_HIGHMEM is unusable.
	 *
	 * Throttling is based on the first usable node and throttled processes
	 * wait on a queue until kswapd makes progress and wakes them. There
	 * is an affinity then between processes waking up and where reclaim
	 * progress has been made assuming the process wakes on the same node.
	 * More importantly, processes running on remote nodes will not compete
	 * for remote pfmemalloc reserves and processes on different nodes
	 * should make reasonable progress.
	 */
	for_each_zone_zonelist_nodemask(zone, z, zonelist,
					gfp_zone(gfp_mask), nodemask) {
		if (zone_idx(zone) > ZONE_NORMAL)
			continue;

		/* Throttle based on the first usable node */
		pgdat = zone->zone_pgdat;
		if (allow_direct_reclaim(pgdat))
			goto out;
		break;
	}

	/* If no zone was usable by the allocation flags then do not throttle */
	if (!pgdat)
		goto out;

	/* Account for the throttling */
	count_vm_event(PGSCAN_DIRECT_THROTTLE);

	/*
	 * If the caller cannot enter the filesystem, it's possible that it
	 * is due to the caller holding an FS lock or performing a journal
	 * transaction in the case of a filesystem like ext[3|4]. In this case,
	 * it is not safe to block on pfmemalloc_wait as kswapd could be
	 * blocked waiting on the same lock. Instead, throttle for up to a
	 * second before continuing.
	 */
	if (!(gfp_mask & __GFP_FS))
		wait_event_interruptible_timeout(pgdat->pfmemalloc_wait,
			allow_direct_reclaim(pgdat), HZ);
	else
		/* Throttle until kswapd wakes the process */
		wait_event_killable(zone->zone_pgdat->pfmemalloc_wait,
			allow_direct_reclaim(pgdat));

	if (fatal_signal_pending(current))
		return true;

out:
	return false;
}

unsigned long try_to_free_pages(struct zonelist *zonelist, int order,
				gfp_t gfp_mask, nodemask_t *nodemask)
{
	unsigned long nr_reclaimed;
	struct scan_control sc = {
		.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.gfp_mask = current_gfp_context(gfp_mask),
		.reclaim_idx = gfp_zone(gfp_mask),
		.order = order,
		.nodemask = nodemask,
		.priority = DEF_PRIORITY,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = 1,
	};

	/*
	 * scan_control uses s8 fields for order, priority, and reclaim_idx.
	 * Confirm they are large enough for max values.
	 */
	BUILD_BUG_ON(MAX_ORDER > S8_MAX);
	BUILD_BUG_ON(DEF_PRIORITY > S8_MAX);
	BUILD_BUG_ON(MAX_NR_ZONES > S8_MAX);

	/*
	 * Do not enter reclaim if fatal signal was delivered while throttled.
	 * 1 is returned so that the page allocator does not OOM kill at this
	 * point.
	 */
	if (throttle_direct_reclaim(sc.gfp_mask, zonelist, nodemask))
		return 1;

	set_task_reclaim_state(current, &sc.reclaim_state);
	trace_mm_vmscan_direct_reclaim_begin(order, sc.gfp_mask);

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);

	trace_mm_vmscan_direct_reclaim_end(nr_reclaimed);
	set_task_reclaim_state(current, NULL);

	return nr_reclaimed;
}

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
static bool allow_pool_direct_reclaim(pg_data_t *pgdat)
{
	struct huge_page_pool *pool = cont_pte_pool();
	bool wmark_ok;

	if (pgdat->kswapd_failures >= MAX_RECLAIM_RETRIES) {
		pr_err_ratelimited("@%s:%d comm:%s pid:%d kswapd_failures >= MAX_RECLAIM_RETRIES @\n",
				   current->comm, current->pid, __func__, __LINE__);
		return true;
	}

	wmark_ok = huge_page_pool_count(pool, HPAGE_POOL_CMA) > pool->wmark[POOL_WMARK_MIN] / 4;

	/* kswapd must be awake if processes are being throttled */
	if (!wmark_ok && waitqueue_active(&pgdat->kswapd_wait)) {
		if (READ_ONCE(pgdat->kswapd_highest_zoneidx) > ZONE_NORMAL)
			WRITE_ONCE(pgdat->kswapd_highest_zoneidx, ZONE_NORMAL);
		pr_err_ratelimited("@%s:%d comm:%s pid:%d wakeup pgdat->kswapd_wait@\n",
				   current->comm, current->pid, __func__, __LINE__);
		wake_up_interruptible(&pgdat->kswapd_wait);
	}

	return wmark_ok;
}

static bool throttle_pool_direct_reclaim(gfp_t gfp_mask, struct zonelist *zonelist,
		nodemask_t *nodemask)
{
	pg_data_t *pgdat = NULL;
	struct zoneref *z;
	struct zone *zone;

	if (current->flags & PF_KTHREAD) {
		pr_err_ratelimited("@%s:%d comm:%s pid:%d @\n",
				    current->comm, current->pid, __func__, __LINE__);
		goto out;
	}

	if (fatal_signal_pending(current)) {
		pr_err_ratelimited("@%s:%d comm:%s pid:%d @\n",
				   current->comm, current->pid, __func__, __LINE__);
		goto out;
	}

	for_each_zone_zonelist_nodemask(zone, z, zonelist,
			gfp_zone(gfp_mask), nodemask) {
		if (zone_idx(zone) > ZONE_NORMAL)
			continue;

		/* Throttle based on the first usable node */
		pgdat = zone->zone_pgdat;
		if (allow_pool_direct_reclaim(pgdat))
			goto out;
		break;
	}

	if (!(gfp_mask & __GFP_FS)) {
		wait_event_interruptible_timeout(pool_direct_reclaim_wait[pgdat->node_id],
				allow_pool_direct_reclaim(pgdat), HZ);

		goto check_pending;
	}

	/* Throttle until kswapd wakes the process */
	wait_event_killable(pool_direct_reclaim_wait[pgdat->node_id],
			allow_pool_direct_reclaim(pgdat));
check_pending:
	if (fatal_signal_pending(current))
		return true;

out:
	return false;
}


/* It's similar to try_to_free_pages */
unsigned long try_to_free_cont_pte_hugepages(struct zonelist *zonelist,
				gfp_t gfp_mask, nodemask_t *nodemask,
				unsigned long nr_reclaim)
{
	unsigned long nr_reclaimed;
	unsigned int noreclaim_flag;
	struct scan_control sc = {
		.nr_to_reclaim = nr_reclaim,
		.gfp_mask = current_gfp_context(gfp_mask) | POOL_USER_ALLOC,
		.reclaim_idx = gfp_zone(gfp_mask),
		.order = HPAGE_CONT_PTE_ORDER,
		.nodemask = nodemask,
		.priority = POOL_DIRECT_RECLAIM_PRIORITY,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = 1,
	};

	/*
	 * scan_control uses s8 fields for order, priority, and reclaim_idx.
	 * Confirm they are large enough for max values.
	 */
	BUILD_BUG_ON(MAX_ORDER > S8_MAX);
	BUILD_BUG_ON(DEF_PRIORITY > S8_MAX);
	BUILD_BUG_ON(MAX_NR_ZONES > S8_MAX);

	cond_resched();

	fs_reclaim_acquire(gfp_mask);
	noreclaim_flag = memalloc_noreclaim_save();

	/* FIXME: Whether throttle direct reclaim is needed? */
	if (throttle_pool_direct_reclaim(sc.gfp_mask, zonelist, nodemask))
		return 1;

	/* FIXME: no thp swap? Whether to consider file thp? */
	if (!thp_swap_is_free())
		return 0;

	atomic64_add(1, &perf_stat.direct_reclaim_stat[POOL_DIRECT_RECLAIM_ENTER]);
	set_task_reclaim_state(current, &sc.reclaim_state);
	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);
	set_task_reclaim_state(current, NULL);

	memalloc_noreclaim_restore(noreclaim_flag);
	fs_reclaim_release(gfp_mask);

	cond_resched();

	return nr_reclaimed;
}
#endif /* defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM */

#ifdef CONFIG_MEMCG

/* Only used by soft limit reclaim. Do not reuse for anything else. */
unsigned long mem_cgroup_shrink_node(struct mem_cgroup *memcg,
						gfp_t gfp_mask, bool noswap,
						pg_data_t *pgdat,
						unsigned long *nr_scanned)
{
	/* FIXME: chp lruvec */
	struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
	struct scan_control sc = {
		.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.target_mem_cgroup = memcg,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.reclaim_idx = MAX_NR_ZONES - 1,
		.may_swap = !noswap,
	};

	WARN_ON_ONCE(!current->reclaim_state);

	sc.gfp_mask = (gfp_mask & GFP_RECLAIM_MASK) |
			(GFP_HIGHUSER_MOVABLE & ~GFP_RECLAIM_MASK);

	trace_mm_vmscan_memcg_softlimit_reclaim_begin(sc.order,
						      sc.gfp_mask);

	/*
	 * NOTE: Although we can get the priority field, using it
	 * here is not a good idea, since it limits the pages we can scan.
	 * if we don't reclaim here, the shrink_node from balance_pgdat
	 * will pick up pages from other mem cgroup's as well. We hack
	 * the priority and make it zero.
	 */
	shrink_lruvec(lruvec, &sc);

	trace_mm_vmscan_memcg_softlimit_reclaim_end(sc.nr_reclaimed);

	*nr_scanned = sc.nr_scanned;

	return sc.nr_reclaimed;
}

unsigned long try_to_free_mem_cgroup_pages(struct mem_cgroup *memcg,
					   unsigned long nr_pages,
					   gfp_t gfp_mask,
					   bool may_swap)
{
	unsigned long nr_reclaimed;
	unsigned int noreclaim_flag;
	struct scan_control sc = {
		.nr_to_reclaim = max(nr_pages, SWAP_CLUSTER_MAX),
		.gfp_mask = (current_gfp_context(gfp_mask) & GFP_RECLAIM_MASK) |
				(GFP_HIGHUSER_MOVABLE & ~GFP_RECLAIM_MASK),
		.reclaim_idx = MAX_NR_ZONES - 1,
		.target_mem_cgroup = memcg,
		.priority = DEF_PRIORITY,
		.may_writepage = !laptop_mode,
		.may_unmap = 1,
		.may_swap = may_swap,
	};
	/*
	 * Traverse the ZONELIST_FALLBACK zonelist of the current node to put
	 * equal pressure on all the nodes. This is based on the assumption that
	 * the reclaim does not bail out early.
	 */
	struct zonelist *zonelist = node_zonelist(numa_node_id(), sc.gfp_mask);

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	if (gfp_mask & POOL_USER_ALLOC_MASK) {
		sc.gfp_mask |= POOL_USER_ALLOC_MASK;
		sc.order = HPAGE_CONT_PTE_ORDER;
	}
#endif

	set_task_reclaim_state(current, &sc.reclaim_state);
	trace_mm_vmscan_memcg_reclaim_begin(0, sc.gfp_mask);
	noreclaim_flag = memalloc_noreclaim_save();

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);

	memalloc_noreclaim_restore(noreclaim_flag);
	trace_mm_vmscan_memcg_reclaim_end(nr_reclaimed);
	set_task_reclaim_state(current, NULL);

	return nr_reclaimed;
}
EXPORT_SYMBOL_GPL(try_to_free_mem_cgroup_pages);
#endif

static void kswapd_age_node(struct pglist_data *pgdat, struct scan_control *sc)
{
	struct mem_cgroup *memcg;
	struct lruvec *lruvec;
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
	bool chp_reclaim = !!(sc->gfp_mask & POOL_USER_ALLOC_MASK);
#endif

	if (lru_gen_enabled()) {
		lru_gen_age_node(pgdat, sc);
		return;
	}

	if (!can_age_anon_pages(pgdat, sc))
		return;

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
	if (chp_reclaim)
		lruvec = mem_cgroup_chp_lruvec(NULL, pgdat);
	else
#endif
		lruvec = mem_cgroup_lruvec(NULL, pgdat);
	if (!inactive_is_low(lruvec, LRU_INACTIVE_ANON))
		return;

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
		if (chp_reclaim)
			lruvec = mem_cgroup_chp_lruvec(memcg, pgdat);
		else
#endif
			lruvec = mem_cgroup_lruvec(memcg, pgdat);
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
		if (chp_reclaim)
			shrink_active_list(CHP_SWAP_CLUSTER_MAX, lruvec,
					sc, LRU_ACTIVE_ANON);
		else
#endif
			shrink_active_list(SWAP_CLUSTER_MAX, lruvec,
					sc, LRU_ACTIVE_ANON);
		memcg = mem_cgroup_iter(NULL, memcg, NULL);
	} while (memcg);
}

static bool pgdat_watermark_boosted(pg_data_t *pgdat, int highest_zoneidx)
{
	int i;
	struct zone *zone;

	/*
	 * Check for watermark boosts top-down as the higher zones
	 * are more likely to be boosted. Both watermarks and boosts
	 * should not be checked at the same time as reclaim would
	 * start prematurely when there is no boosting and a lower
	 * zone is balanced.
	 */
	for (i = highest_zoneidx; i >= 0; i--) {
		zone = pgdat->node_zones + i;
		if (!managed_zone(zone))
			continue;

		if (zone->watermark_boost)
			return true;
	}

	return false;
}

/*
 * Returns true if there is an eligible zone balanced for the request order
 * and highest_zoneidx
 */
static bool pgdat_balanced(pg_data_t *pgdat, int order, int highest_zoneidx)
{
	int i;
	unsigned long mark = -1;
	struct zone *zone;

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	if (order == HPAGE_CONT_PTE_ORDER &&
	    test_bit(PGDAT_POOL_USER_ALLOC, &pgdat->flags)) {
		struct huge_page_pool *pool = cont_pte_pool();

		if (huge_page_pool_count(pool, HPAGE_POOL_CMA) < pool->wmark[POOL_WMARK_HIGH] && thp_swap_is_free())
			return false;
		else
			return true;
	}
#endif
	/*
	 * Check watermarks bottom-up as lower zones are more likely to
	 * meet watermarks.
	 */
	for (i = 0; i <= highest_zoneidx; i++) {
		zone = pgdat->node_zones + i;

		if (!managed_zone(zone))
			continue;

		mark = high_wmark_pages(zone);
		if (zone_watermark_ok_safe(zone, order, mark, highest_zoneidx))
			return true;
	}

	/*
	 * If a node has no populated zone within highest_zoneidx, it does not
	 * need balancing by definition. This can happen if a zone-restricted
	 * allocation tries to wake a remote kswapd.
	 */
	if (mark == -1)
		return true;

	return false;
}

/* Clear pgdat state for congested, dirty or under writeback. */
static void clear_pgdat_congested(pg_data_t *pgdat)
{
	struct lruvec *lruvec = mem_cgroup_lruvec(NULL, pgdat);

	clear_bit(LRUVEC_CONGESTED, &lruvec->flags);
	clear_bit(PGDAT_DIRTY, &pgdat->flags);
	clear_bit(PGDAT_WRITEBACK, &pgdat->flags);
}

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
static void clear_chp_pgdat_congested(pg_data_t *pgdat)
{
	struct lruvec *lruvec = mem_cgroup_chp_lruvec(NULL, pgdat);

	clear_bit(LRUVEC_CONGESTED, &lruvec->flags);
	clear_bit(PGDAT_DIRTY, &pgdat->flags);
	clear_bit(PGDAT_WRITEBACK, &pgdat->flags);
}
#endif

/*
 * Prepare kswapd for sleeping. This verifies that there are no processes
 * waiting in throttle_direct_reclaim() and that watermarks have been met.
 *
 * Returns true if kswapd is ready to sleep
 */
static bool prepare_kswapd_sleep(pg_data_t *pgdat, int order,
				int highest_zoneidx)
{
	/*
	 * The throttled processes are normally woken up in balance_pgdat() as
	 * soon as allow_direct_reclaim() is true. But there is a potential
	 * race between when kswapd checks the watermarks and a process gets
	 * throttled. There is also a potential race if processes get
	 * throttled, kswapd wakes, a large process exits thereby balancing the
	 * zones, which causes kswapd to exit balance_pgdat() before reaching
	 * the wake up checks. If kswapd is going to sleep, no process should
	 * be sleeping on pfmemalloc_wait, so wake them now if necessary. If
	 * the wake up is premature, processes will wake kswapd and get
	 * throttled again. The difference from wake ups in balance_pgdat() is
	 * that here we are under prepare_to_wait().
	 */
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	if (order == HPAGE_CONT_PTE_ORDER &&
			test_bit(PGDAT_POOL_USER_ALLOC, &pgdat->flags)) {
		if (waitqueue_active(&pool_direct_reclaim_wait[pgdat->node_id])) {
			struct huge_page_pool *pool = cont_pte_pool();

			pr_debug_ratelimited("@%s:%d -> wake_up pool_direct_reclaim_wait count:%d wmark_min=%d @\n",
					__func__, __LINE__, huge_page_pool_count(pool, HPAGE_POOL_CMA), pool->wmark[POOL_WMARK_MIN]);
			wake_up_all(&pool_direct_reclaim_wait[pgdat->node_id]);
		}
	} else
#endif
	if (waitqueue_active(&pgdat->pfmemalloc_wait))
		wake_up_all(&pgdat->pfmemalloc_wait);

	/* Hopeless node, leave it to direct reclaim */
	if (pgdat->kswapd_failures >= MAX_RECLAIM_RETRIES)
		return true;

	if (pgdat_balanced(pgdat, order, highest_zoneidx)) {
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
		if (order == HPAGE_CONT_PTE_ORDER &&
		    test_bit(PGDAT_POOL_USER_ALLOC, &pgdat->flags))
			clear_chp_pgdat_congested(pgdat);
		else
#endif
			clear_pgdat_congested(pgdat);
		return true;
	}

	return false;
}

/*
 * kswapd shrinks a node of pages that are at or below the highest usable
 * zone that is currently unbalanced.
 *
 * Returns true if kswapd scanned at least the requested number of pages to
 * reclaim or if the lack of progress was due to pages under writeback.
 * This is used to determine if the scanning priority needs to be raised.
 */
static bool kswapd_shrink_node(pg_data_t *pgdat,
			       struct scan_control *sc)
{
	struct zone *zone;
	int z;

	/* Reclaim a number of pages proportional to the number of zones */
	sc->nr_to_reclaim = 0;
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	/* FIXME: need kswapd reclaim to wmark_high of pool?*/
	if (sc->gfp_mask & POOL_USER_ALLOC_MASK) {
		struct huge_page_pool *pool = cont_pte_pool();

		/* FIXME: We only recclaim half of the wmark_high! */
		sc->nr_to_reclaim = (pool->wmark[POOL_WMARK_HIGH] * HPAGE_CONT_PTE_NR) / 2;
	} else
#endif
		for (z = 0; z <= sc->reclaim_idx; z++) {
			zone = pgdat->node_zones + z;
			if (!managed_zone(zone))
				continue;

			sc->nr_to_reclaim += max(high_wmark_pages(zone), SWAP_CLUSTER_MAX);
		}

	/*
	 * Historically care was taken to put equal pressure on all zones but
	 * now pressure is applied based on node LRU order.
	 */
	shrink_node(pgdat, sc);

	/*
	 * Fragmentation may mean that the system cannot be rebalanced for
	 * high-order allocations. If twice the allocation size has been
	 * reclaimed then recheck watermarks only at order-0 to prevent
	 * excessive reclaim. Assume that a process requested a high-order
	 * can direct reclaim/compact.
	 */
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	if (!(sc->gfp_mask & POOL_USER_ALLOC_MASK))
#endif
	if (sc->order && sc->nr_reclaimed >= compact_gap(sc->order))
		sc->order = 0;

	return sc->nr_scanned >= sc->nr_to_reclaim;
}

/* Page allocator PCP high watermark is lowered if reclaim is active. */
static inline void
update_reclaim_active(pg_data_t *pgdat, int highest_zoneidx, bool active)
{
	int i;
	struct zone *zone;

	for (i = 0; i <= highest_zoneidx; i++) {
		zone = pgdat->node_zones + i;

		if (!managed_zone(zone))
			continue;

		if (active)
			set_bit(ZONE_RECLAIM_ACTIVE, &zone->flags);
		else
			clear_bit(ZONE_RECLAIM_ACTIVE, &zone->flags);
	}
}

static inline void
set_reclaim_active(pg_data_t *pgdat, int highest_zoneidx)
{
	update_reclaim_active(pgdat, highest_zoneidx, true);
}

static inline void
clear_reclaim_active(pg_data_t *pgdat, int highest_zoneidx)
{
	update_reclaim_active(pgdat, highest_zoneidx, false);
}

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
/* perf: a chp stub for splitting backtraces of  basepage and hugepages */
static noinline bool kswapd_chp_shrink_node(pg_data_t *pgdat,
			       struct scan_control *sc)
{
	return kswapd_shrink_node(pgdat, sc);
}
#endif

/*
 * For kswapd, balance_pgdat() will reclaim pages across a node from zones
 * that are eligible for use by the caller until at least one zone is
 * balanced.
 *
 * Returns the order kswapd finished reclaiming at.
 *
 * kswapd scans the zones in the highmem->normal->dma direction.  It skips
 * zones which have free_pages > high_wmark_pages(zone), but once a zone is
 * found to have free_pages <= high_wmark_pages(zone), any page in that zone
 * or lower is eligible for reclaim until at least one usable zone is
 * balanced.
 */
static int balance_pgdat(pg_data_t *pgdat, int order, int highest_zoneidx)
{
	int i;
	unsigned long nr_soft_reclaimed;
	unsigned long nr_soft_scanned;
	unsigned long pflags;
	unsigned long nr_boost_reclaim;
	unsigned long zone_boosts[MAX_NR_ZONES] = { 0, };
	bool boosted;
	struct zone *zone;
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.order = order,
		.may_unmap = 1,
	};
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	s64 time;
	s64 reclaim_seq;
#endif

	set_task_reclaim_state(current, &sc.reclaim_state);
	psi_memstall_enter(&pflags);
	__fs_reclaim_acquire(_THIS_IP_);

	count_vm_event(PAGEOUTRUN);

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	if (order == HPAGE_CONT_PTE_ORDER &&
	    test_bit(PGDAT_POOL_USER_ALLOC, &pgdat->flags)) {
		sc.gfp_mask |= POOL_USER_ALLOC;

		/* FIXME: no thp swap? Whether to consider file thp? */
		if (!thp_swap_is_free()) {
			__fs_reclaim_release(_THIS_IP_);
			psi_memstall_leave(&pflags);
			set_task_reclaim_state(current, NULL);

			return sc.order;
		}

		time = ktime_to_ms(ktime_get());
		atomic64_add(1, &perf_stat.reclaim_seq[POOL_KSWAPD_RECLAIM]);
		reclaim_seq = atomic64_read(&perf_stat.reclaim_seq[POOL_KSWAPD_RECLAIM]);
	}
#endif

	/*
	 * Account for the reclaim boost. Note that the zone boost is left in
	 * place so that parallel allocations that are near the watermark will
	 * stall or direct reclaim until kswapd is finished.
	 */
	nr_boost_reclaim = 0;
	for (i = 0; i <= highest_zoneidx; i++) {
		zone = pgdat->node_zones + i;
		if (!managed_zone(zone))
			continue;

		nr_boost_reclaim += zone->watermark_boost;
		zone_boosts[i] = zone->watermark_boost;
	}
	boosted = nr_boost_reclaim;

restart:
	set_reclaim_active(pgdat, highest_zoneidx);
	sc.priority = DEF_PRIORITY;
	do {
		unsigned long nr_reclaimed = sc.nr_reclaimed;
		bool raise_priority = true;
		bool balanced;
		bool ret;

		sc.reclaim_idx = highest_zoneidx;

		/*
		 * If the number of buffer_heads exceeds the maximum allowed
		 * then consider reclaiming from all zones. This has a dual
		 * purpose -- on 64-bit systems it is expected that
		 * buffer_heads are stripped during active rotation. On 32-bit
		 * systems, highmem pages can pin lowmem memory and shrinking
		 * buffers can relieve lowmem pressure. Reclaim may still not
		 * go ahead if all eligible zones for the original allocation
		 * request are balanced to avoid excessive reclaim from kswapd.
		 */
		if (buffer_heads_over_limit) {
			for (i = MAX_NR_ZONES - 1; i >= 0; i--) {
				zone = pgdat->node_zones + i;
				if (!managed_zone(zone))
					continue;

				sc.reclaim_idx = i;
				break;
			}
		}

		/*
		 * If the pgdat is imbalanced then ignore boosting and preserve
		 * the watermarks for a later time and restart. Note that the
		 * zone watermarks will be still reset at the end of balancing
		 * on the grounds that the normal reclaim should be enough to
		 * re-evaluate if boosting is required when kswapd next wakes.
		 */
		balanced = pgdat_balanced(pgdat, sc.order, highest_zoneidx);
		if (!balanced && nr_boost_reclaim) {
			nr_boost_reclaim = 0;
			goto restart;
		}

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
		if (sc.gfp_mask & POOL_USER_ALLOC_MASK) {
			/*
			 * When thp's swap has no free slot,
			 * we end the reclaim loop.
			 * FIXME: support file thp limit?
			 */
			if (balanced || !thp_swap_is_free())
				goto out;
		}
#endif
		/*
		 * If boosting is not active then only reclaim if there are no
		 * eligible zones. Note that sc.reclaim_idx is not used as
		 * buffer_heads_over_limit may have adjusted it.
		 */
		if (!nr_boost_reclaim && balanced)
			goto out;

		/* Limit the priority of boosting to avoid reclaim writeback */
		if (nr_boost_reclaim && sc.priority == DEF_PRIORITY - 2)
			raise_priority = false;

		/*
		 * Do not writeback or swap pages for boosted reclaim. The
		 * intent is to relieve pressure not issue sub-optimal IO
		 * from reclaim context. If no pages are reclaimed, the
		 * reclaim will be aborted.
		 */
		sc.may_writepage = !laptop_mode && !nr_boost_reclaim;
		sc.may_swap = !nr_boost_reclaim;

		/*
		 * Do some background aging, to give pages a chance to be
		 * referenced before reclaiming. All pages are rotated
		 * regardless of classzone as this is about consistent aging.
		 */
		kswapd_age_node(pgdat, &sc);

		/*
		 * If we're getting trouble reclaiming, start doing writepage
		 * even in laptop mode.
		 */
		if (sc.priority < DEF_PRIORITY - 2)
			sc.may_writepage = 1;

		/* Call soft limit reclaim before calling shrink_node. */
		sc.nr_scanned = 0;
		nr_soft_scanned = 0;
		nr_soft_reclaimed = mem_cgroup_soft_limit_reclaim(pgdat, sc.order,
						sc.gfp_mask, &nr_soft_scanned);
		sc.nr_reclaimed += nr_soft_reclaimed;

		/*
		 * There should be no need to raise the scanning priority if
		 * enough pages are already being scanned that that high
		 * watermark would be met at 100% efficiency.
		 */
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
		if (sc.gfp_mask & POOL_USER_ALLOC_MASK) {
			if (kswapd_chp_shrink_node(pgdat, &sc))
				raise_priority = false;
		} else
#endif
		if (kswapd_shrink_node(pgdat, &sc))
			raise_priority = false;

		/*
		 * If the low watermark is met there is no need for processes
		 * to be throttled on pfmemalloc_wait as they should not be
		 * able to safely make forward progress. Wake them
		 */
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
		if (sc.gfp_mask & POOL_USER_ALLOC_MASK) {
			if (waitqueue_active(&pool_direct_reclaim_wait[pgdat->node_id]) &&
				allow_pool_direct_reclaim(pgdat)) {
				wake_up_all(&pool_direct_reclaim_wait[pgdat->node_id]);
			}
		} else
#endif
		if (waitqueue_active(&pgdat->pfmemalloc_wait) &&
				allow_direct_reclaim(pgdat))
			wake_up_all(&pgdat->pfmemalloc_wait);

		/* Check if kswapd should be suspending */
		__fs_reclaim_release(_THIS_IP_);
		ret = try_to_freeze();
		__fs_reclaim_acquire(_THIS_IP_);
		if (ret || kthread_should_stop())
			break;

		/*
		 * Raise priority if scanning rate is too low or there was no
		 * progress in reclaiming pages
		 */
		nr_reclaimed = sc.nr_reclaimed - nr_reclaimed;
		nr_boost_reclaim -= min(nr_boost_reclaim, nr_reclaimed);

		/*
		 * If reclaim made no progress for a boost, stop reclaim as
		 * IO cannot be queued and it could be an infinite loop in
		 * extreme circumstances.
		 */
		if (nr_boost_reclaim && !nr_reclaimed)
			break;

		if (raise_priority || !nr_reclaimed)
			sc.priority--;
	} while (sc.priority >= 1);

	if (!sc.nr_reclaimed)
		pgdat->kswapd_failures++;

out:
	clear_reclaim_active(pgdat, highest_zoneidx);
#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	if (sc.gfp_mask & POOL_USER_ALLOC_MASK) {
		atomic64_set(&perf_stat.reclaim_count[POOL_KSWAPD_RECLAIM][reclaim_seq % POOL_RECLAIM_SEQ_ITEM], sc.nr_reclaimed);
		perf_stat.reclaim_time[POOL_KSWAPD_RECLAIM][reclaim_seq % POOL_RECLAIM_SEQ_ITEM] = ktime_to_ms(ktime_get()) - time;
	}
#endif

	/* If reclaim was boosted, account for the reclaim done in this pass */
	if (boosted) {
		unsigned long flags;

		for (i = 0; i <= highest_zoneidx; i++) {
			if (!zone_boosts[i])
				continue;

			/* Increments are under the zone lock */
			zone = pgdat->node_zones + i;
			spin_lock_irqsave(&zone->lock, flags);
			zone->watermark_boost -= min(zone->watermark_boost, zone_boosts[i]);
			spin_unlock_irqrestore(&zone->lock, flags);
		}

		/*
		 * As there is now likely space, wakeup kcompact to defragment
		 * pageblocks.
		 */
		wakeup_kcompactd(pgdat, pageblock_order, highest_zoneidx);
	}

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_CONT_PTE_HUGEPAGE_LRU
	if (sc.gfp_mask & POOL_USER_ALLOC_MASK)
		snapshot_chp_refaults(NULL, pgdat);
	else
#endif
		snapshot_refaults(NULL, pgdat);
	__fs_reclaim_release(_THIS_IP_);
	psi_memstall_leave(&pflags);
	set_task_reclaim_state(current, NULL);

	/*
	 * Return the order kswapd stopped reclaiming at as
	 * prepare_kswapd_sleep() takes it into account. If another caller
	 * entered the allocator slow path while kswapd was awake, order will
	 * remain at the higher level.
	 */
	return sc.order;
}

/*
 * The pgdat->kswapd_highest_zoneidx is used to pass the highest zone index to
 * be reclaimed by kswapd from the waker. If the value is MAX_NR_ZONES which is
 * not a valid index then either kswapd runs for first time or kswapd couldn't
 * sleep after previous reclaim attempt (node is still unbalanced). In that
 * case return the zone index of the previous kswapd reclaim cycle.
 */
static enum zone_type kswapd_highest_zoneidx(pg_data_t *pgdat,
					   enum zone_type prev_highest_zoneidx)
{
	enum zone_type curr_idx = READ_ONCE(pgdat->kswapd_highest_zoneidx);

	return curr_idx == MAX_NR_ZONES ? prev_highest_zoneidx : curr_idx;
}

static void kswapd_try_to_sleep(pg_data_t *pgdat, int alloc_order, int reclaim_order,
				unsigned int highest_zoneidx)
{
	long remaining = 0;
	DEFINE_WAIT(wait);

	if (freezing(current) || kthread_should_stop())
		return;

	prepare_to_wait(&pgdat->kswapd_wait, &wait, TASK_INTERRUPTIBLE);

	/*
	 * Try to sleep for a short interval. Note that kcompactd will only be
	 * woken if it is possible to sleep for a short interval. This is
	 * deliberate on the assumption that if reclaim cannot keep an
	 * eligible zone balanced that it's also unlikely that compaction will
	 * succeed.
	 */
	if (prepare_kswapd_sleep(pgdat, reclaim_order, highest_zoneidx)) {
		/*
		 * Compaction records what page blocks it recently failed to
		 * isolate pages from and skips them in the future scanning.
		 * When kswapd is going to sleep, it is reasonable to assume
		 * that pages and compaction may succeed so reset the cache.
		 */
		reset_isolation_suitable(pgdat);

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
		/*
		 * We have freed the memory, now we should clear PGDAT_POOL_USER_ALLOC.
		 */
		if (alloc_order == HPAGE_CONT_PTE_ORDER &&
		    test_bit(PGDAT_POOL_USER_ALLOC, &pgdat->flags))
			clear_bit(PGDAT_POOL_USER_ALLOC, &pgdat->flags);
		else
#endif
		/*
		 * We have freed the memory, now we should compact it to make
		 * allocation of the requested order possible.
		 */
		wakeup_kcompactd(pgdat, alloc_order, highest_zoneidx);

		remaining = schedule_timeout(HZ/10);

		/*
		 * If woken prematurely then reset kswapd_highest_zoneidx and
		 * order. The values will either be from a wakeup request or
		 * the previous request that slept prematurely.
		 */
		if (remaining) {
			WRITE_ONCE(pgdat->kswapd_highest_zoneidx,
					kswapd_highest_zoneidx(pgdat,
							highest_zoneidx));

			if (READ_ONCE(pgdat->kswapd_order) < reclaim_order)
				WRITE_ONCE(pgdat->kswapd_order, reclaim_order);
		}

		finish_wait(&pgdat->kswapd_wait, &wait);
		prepare_to_wait(&pgdat->kswapd_wait, &wait, TASK_INTERRUPTIBLE);
	}

	/*
	 * After a short sleep, check if it was a premature sleep. If not, then
	 * go fully to sleep until explicitly woken up.
	 */
	if (!remaining &&
	    prepare_kswapd_sleep(pgdat, reclaim_order, highest_zoneidx)) {
		trace_mm_vmscan_kswapd_sleep(pgdat->node_id);

		/*
		 * vmstat counters are not perfectly accurate and the estimated
		 * value for counters such as NR_FREE_PAGES can deviate from the
		 * true value by nr_online_cpus * threshold. To avoid the zone
		 * watermarks being breached while under pressure, we reduce the
		 * per-cpu vmstat threshold while kswapd is awake and restore
		 * them before going back to sleep.
		 */
		set_pgdat_percpu_threshold(pgdat, calculate_normal_threshold);

		if (!kthread_should_stop())
			schedule();

		set_pgdat_percpu_threshold(pgdat, calculate_pressure_threshold);
	} else {
		if (remaining)
			count_vm_event(KSWAPD_LOW_WMARK_HIT_QUICKLY);
		else
			count_vm_event(KSWAPD_HIGH_WMARK_HIT_QUICKLY);
	}
	finish_wait(&pgdat->kswapd_wait, &wait);
}

/*
 * The background pageout daemon, started as a kernel thread
 * from the init process.
 *
 * This basically trickles out pages so that we have _some_
 * free memory available even if there is no other activity
 * that frees anything up. This is needed for things like routing
 * etc, where we otherwise might have all activity going on in
 * asynchronous contexts that cannot page things out.
 *
 * If there are applications that are active memory-allocators
 * (most normal use), this basically shouldn't matter.
 */
int kswapd(void *p)
{
	unsigned int alloc_order, reclaim_order;
	unsigned int highest_zoneidx = MAX_NR_ZONES - 1;
	pg_data_t *pgdat = (pg_data_t *)p;
	struct task_struct *tsk = current;
	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(tsk, cpumask);

	/*
	 * Tell the memory management that we're a "memory allocator",
	 * and that if we need more memory we should get access to it
	 * regardless (see "__alloc_pages()"). "kswapd" should
	 * never get caught in the normal page freeing logic.
	 *
	 * (Kswapd normally doesn't need memory anyway, but sometimes
	 * you need a small amount of memory in order to be able to
	 * page out something else, and this flag essentially protects
	 * us from recursively trying to free more memory as we're
	 * trying to free the first piece of memory in the first place).
	 */
	tsk->flags |= PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD;
	set_freezable();

	WRITE_ONCE(pgdat->kswapd_order, 0);
	WRITE_ONCE(pgdat->kswapd_highest_zoneidx, MAX_NR_ZONES);
	for ( ; ; ) {
		bool ret;

		alloc_order = reclaim_order = READ_ONCE(pgdat->kswapd_order);
		highest_zoneidx = kswapd_highest_zoneidx(pgdat,
							highest_zoneidx);

kswapd_try_sleep:
		kswapd_try_to_sleep(pgdat, alloc_order, reclaim_order,
					highest_zoneidx);

		/* Read the new order and highest_zoneidx */
		alloc_order = READ_ONCE(pgdat->kswapd_order);
		highest_zoneidx = kswapd_highest_zoneidx(pgdat,
							highest_zoneidx);
		WRITE_ONCE(pgdat->kswapd_order, 0);
		WRITE_ONCE(pgdat->kswapd_highest_zoneidx, MAX_NR_ZONES);

		ret = try_to_freeze();
		if (kthread_should_stop())
			break;

		/*
		 * We can speed up thawing tasks if we don't call balance_pgdat
		 * after returning from the refrigerator
		 */
		if (ret)
			continue;

		/*
		 * Reclaim begins at the requested order but if a high-order
		 * reclaim fails then kswapd falls back to reclaiming for
		 * order-0. If that happens, kswapd will consider sleeping
		 * for the order it finished reclaiming at (reclaim_order)
		 * but kcompactd is woken to compact for the original
		 * request (alloc_order).
		 */
		trace_mm_vmscan_kswapd_wake(pgdat->node_id, highest_zoneidx,
						alloc_order);
		reclaim_order = balance_pgdat(pgdat, alloc_order,
						highest_zoneidx);
		trace_android_vh_vmscan_kswapd_done(pgdat->node_id, highest_zoneidx,
						alloc_order, reclaim_order);
		if (reclaim_order < alloc_order)
			goto kswapd_try_sleep;
	}

	tsk->flags &= ~(PF_MEMALLOC | PF_SWAPWRITE | PF_KSWAPD);

	return 0;
}
EXPORT_SYMBOL_GPL(kswapd);

/*
 * A zone is low on free memory or too fragmented for high-order memory.  If
 * kswapd should reclaim (direct reclaim is deferred), wake it up for the zone's
 * pgdat.  It will wake up kcompactd after reclaiming memory.  If kswapd reclaim
 * has failed or is not needed, still wake up kcompactd if only compaction is
 * needed.
 */
void wakeup_kswapd(struct zone *zone, gfp_t gfp_flags, int order,
		   enum zone_type highest_zoneidx)
{
	pg_data_t *pgdat;
	enum zone_type curr_idx;

	if (!managed_zone(zone))
		return;

	if (!cpuset_zone_allowed(zone, gfp_flags))
		return;

	pgdat = zone->zone_pgdat;
	curr_idx = READ_ONCE(pgdat->kswapd_highest_zoneidx);

	if (curr_idx == MAX_NR_ZONES || curr_idx < highest_zoneidx)
		WRITE_ONCE(pgdat->kswapd_highest_zoneidx, highest_zoneidx);

	if (READ_ONCE(pgdat->kswapd_order) < order)
		WRITE_ONCE(pgdat->kswapd_order, order);

	if (!waitqueue_active(&pgdat->kswapd_wait))
		return;

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	/* It starts with real sleep and ends with sleep */
	if (gfp_flags & POOL_USER_ALLOC_MASK) {
		set_bit(PGDAT_POOL_USER_ALLOC, &pgdat->flags);
		gfp_flags &= ~POOL_USER_ALLOC_MASK;
	}
#endif

	/* Hopeless node, leave it to direct reclaim if possible */
	if (pgdat->kswapd_failures >= MAX_RECLAIM_RETRIES ||
	    (pgdat_balanced(pgdat, order, highest_zoneidx) &&
	     !pgdat_watermark_boosted(pgdat, highest_zoneidx))) {
		/*
		 * There may be plenty of free memory available, but it's too
		 * fragmented for high-order allocations.  Wake up kcompactd
		 * and rely on compaction_suitable() to determine if it's
		 * needed.  If it fails, it will defer subsequent attempts to
		 * ratelimit its work.
		 */
		if (!(gfp_flags & __GFP_DIRECT_RECLAIM))
			wakeup_kcompactd(pgdat, order, highest_zoneidx);

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
		test_and_clear_bit(PGDAT_POOL_USER_ALLOC, &pgdat->flags);
#endif

		return;
	}

	trace_mm_vmscan_wakeup_kswapd(pgdat->node_id, highest_zoneidx, order,
				      gfp_flags);
	wake_up_interruptible(&pgdat->kswapd_wait);

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	atomic64_add(1, &perf_stat.kswapd_wakeup_count);
#endif
}

#ifdef CONFIG_HIBERNATION
/*
 * Try to free `nr_to_reclaim' of memory, system-wide, and return the number of
 * freed pages.
 *
 * Rather than trying to age LRUs the aim is to preserve the overall
 * LRU order by reclaiming preferentially
 * inactive > active > active referenced > active mapped
 */
unsigned long shrink_all_memory(unsigned long nr_to_reclaim)
{
	struct scan_control sc = {
		.nr_to_reclaim = nr_to_reclaim,
		.gfp_mask = GFP_HIGHUSER_MOVABLE,
		.reclaim_idx = MAX_NR_ZONES - 1,
		.priority = DEF_PRIORITY,
		.may_writepage = 1,
		.may_unmap = 1,
		.may_swap = 1,
		.hibernation_mode = 1,
	};
	struct zonelist *zonelist = node_zonelist(numa_node_id(), sc.gfp_mask);
	unsigned long nr_reclaimed;
	unsigned int noreclaim_flag;

	fs_reclaim_acquire(sc.gfp_mask);
	noreclaim_flag = memalloc_noreclaim_save();
	set_task_reclaim_state(current, &sc.reclaim_state);

	nr_reclaimed = do_try_to_free_pages(zonelist, &sc);

	set_task_reclaim_state(current, NULL);
	memalloc_noreclaim_restore(noreclaim_flag);
	fs_reclaim_release(sc.gfp_mask);

	return nr_reclaimed;
}
#endif /* CONFIG_HIBERNATION */

/*
 * This kswapd start function will be called by init and node-hot-add.
 * On node-hot-add, kswapd will moved to proper cpus if cpus are hot-added.
 */
void kswapd_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	bool skip = false;

	if (pgdat->kswapd)
		return;

	trace_android_vh_kswapd_per_node(nid, &skip, true);
	if (skip)
		return;
	pgdat->kswapd = kthread_run(kswapd, pgdat, "kswapd%d", nid);
	if (IS_ERR(pgdat->kswapd)) {
		/* failure at boot is fatal */
		BUG_ON(system_state < SYSTEM_RUNNING);
		pr_err("Failed to start kswapd on node %d\n", nid);
		pgdat->kswapd = NULL;
	}
}

/*
 * Called by memory hotplug when all memory in a node is offlined.  Caller must
 * hold mem_hotplug_begin/end().
 */
void kswapd_stop(int nid)
{
	struct task_struct *kswapd = NODE_DATA(nid)->kswapd;
	bool skip = false;

	trace_android_vh_kswapd_per_node(nid, &skip, false);
	if (skip)
		return;
	if (kswapd) {
		kthread_stop(kswapd);
		NODE_DATA(nid)->kswapd = NULL;
	}
}

static int __init kswapd_init(void)
{
	int nid;

	swap_setup();
	for_each_node_state(nid, N_MEMORY)
 		kswapd_run(nid);
	return 0;
}

module_init(kswapd_init)

#ifdef CONFIG_NUMA
/*
 * Node reclaim mode
 *
 * If non-zero call node_reclaim when the number of free pages falls below
 * the watermarks.
 */
int node_reclaim_mode __read_mostly;

/*
 * Priority for NODE_RECLAIM. This determines the fraction of pages
 * of a node considered for each zone_reclaim. 4 scans 1/16th of
 * a zone.
 */
#define NODE_RECLAIM_PRIORITY 4

/*
 * Percentage of pages in a zone that must be unmapped for node_reclaim to
 * occur.
 */
int sysctl_min_unmapped_ratio = 1;

/*
 * If the number of slab pages in a zone grows beyond this percentage then
 * slab reclaim needs to occur.
 */
int sysctl_min_slab_ratio = 5;

static inline unsigned long node_unmapped_file_pages(struct pglist_data *pgdat)
{
	unsigned long file_mapped = node_page_state(pgdat, NR_FILE_MAPPED);
	unsigned long file_lru = node_page_state(pgdat, NR_INACTIVE_FILE) +
		node_page_state(pgdat, NR_ACTIVE_FILE);

	/*
	 * It's possible for there to be more file mapped pages than
	 * accounted for by the pages on the file LRU lists because
	 * tmpfs pages accounted for as ANON can also be FILE_MAPPED
	 */
	return (file_lru > file_mapped) ? (file_lru - file_mapped) : 0;
}

/* Work out how many page cache pages we can reclaim in this reclaim_mode */
static unsigned long node_pagecache_reclaimable(struct pglist_data *pgdat)
{
	unsigned long nr_pagecache_reclaimable;
	unsigned long delta = 0;

	/*
	 * If RECLAIM_UNMAP is set, then all file pages are considered
	 * potentially reclaimable. Otherwise, we have to worry about
	 * pages like swapcache and node_unmapped_file_pages() provides
	 * a better estimate
	 */
	if (node_reclaim_mode & RECLAIM_UNMAP)
		nr_pagecache_reclaimable = node_page_state(pgdat, NR_FILE_PAGES);
	else
		nr_pagecache_reclaimable = node_unmapped_file_pages(pgdat);

	/* If we can't clean pages, remove dirty pages from consideration */
	if (!(node_reclaim_mode & RECLAIM_WRITE))
		delta += node_page_state(pgdat, NR_FILE_DIRTY);

	/* Watch for any possible underflows due to delta */
	if (unlikely(delta > nr_pagecache_reclaimable))
		delta = nr_pagecache_reclaimable;

	return nr_pagecache_reclaimable - delta;
}

/*
 * Try to free up some pages from this node through reclaim.
 */
static int __node_reclaim(struct pglist_data *pgdat, gfp_t gfp_mask, unsigned int order)
{
	/* Minimum pages needed in order to stay on node */
	const unsigned long nr_pages = 1 << order;
	struct task_struct *p = current;
	unsigned int noreclaim_flag;
	struct scan_control sc = {
		.nr_to_reclaim = max(nr_pages, SWAP_CLUSTER_MAX),
		.gfp_mask = current_gfp_context(gfp_mask),
		.order = order,
		.priority = NODE_RECLAIM_PRIORITY,
		.may_writepage = !!(node_reclaim_mode & RECLAIM_WRITE),
		.may_unmap = !!(node_reclaim_mode & RECLAIM_UNMAP),
		.may_swap = 1,
		.reclaim_idx = gfp_zone(gfp_mask),
	};
	unsigned long pflags;

#if defined(CONFIG_CONT_PTE_HUGEPAGE) && CONFIG_POOL_ASYNC_RECLAIM
	if (sc.gfp_mask & POOL_USER_ALLOC_MASK)
		sc.nr_to_reclaim = max(nr_pages, CHP_SWAP_CLUSTER_MAX);
#endif
	trace_mm_vmscan_node_reclaim_begin(pgdat->node_id, order,
					   sc.gfp_mask);

	cond_resched();
	psi_memstall_enter(&pflags);
	fs_reclaim_acquire(sc.gfp_mask);
	/*
	 * We need to be able to allocate from the reserves for RECLAIM_UNMAP
	 * and we also need to be able to write out pages for RECLAIM_WRITE
	 * and RECLAIM_UNMAP.
	 */
	noreclaim_flag = memalloc_noreclaim_save();
	p->flags |= PF_SWAPWRITE;
	set_task_reclaim_state(p, &sc.reclaim_state);

	if (node_pagecache_reclaimable(pgdat) > pgdat->min_unmapped_pages) {
		/*
		 * Free memory by calling shrink node with increasing
		 * priorities until we have enough memory freed.
		 */
		do {
			shrink_node(pgdat, &sc);
		} while (sc.nr_reclaimed < nr_pages && --sc.priority >= 0);
	}

	set_task_reclaim_state(p, NULL);
	current->flags &= ~PF_SWAPWRITE;
	memalloc_noreclaim_restore(noreclaim_flag);
	fs_reclaim_release(sc.gfp_mask);
	psi_memstall_leave(&pflags);

	trace_mm_vmscan_node_reclaim_end(sc.nr_reclaimed);

	return sc.nr_reclaimed >= nr_pages;
}

int node_reclaim(struct pglist_data *pgdat, gfp_t gfp_mask, unsigned int order)
{
	int ret;

	/*
	 * Node reclaim reclaims unmapped file backed pages and
	 * slab pages if we are over the defined limits.
	 *
	 * A small portion of unmapped file backed pages is needed for
	 * file I/O otherwise pages read by file I/O will be immediately
	 * thrown out if the node is overallocated. So we do not reclaim
	 * if less than a specified percentage of the node is used by
	 * unmapped file backed pages.
	 */
	if (node_pagecache_reclaimable(pgdat) <= pgdat->min_unmapped_pages &&
	    node_page_state_pages(pgdat, NR_SLAB_RECLAIMABLE_B) <=
	    pgdat->min_slab_pages)
		return NODE_RECLAIM_FULL;

	/*
	 * Do not scan if the allocation should not be delayed.
	 */
	if (!gfpflags_allow_blocking(gfp_mask) || (current->flags & PF_MEMALLOC))
		return NODE_RECLAIM_NOSCAN;

	/*
	 * Only run node reclaim on the local node or on nodes that do not
	 * have associated processors. This will favor the local processor
	 * over remote processors and spread off node memory allocations
	 * as wide as possible.
	 */
	if (node_state(pgdat->node_id, N_CPU) && pgdat->node_id != numa_node_id())
		return NODE_RECLAIM_NOSCAN;

	if (test_and_set_bit(PGDAT_RECLAIM_LOCKED, &pgdat->flags))
		return NODE_RECLAIM_NOSCAN;

	ret = __node_reclaim(pgdat, gfp_mask, order);
	clear_bit(PGDAT_RECLAIM_LOCKED, &pgdat->flags);

	if (!ret)
		count_vm_event(PGSCAN_ZONE_RECLAIM_FAILED);

	return ret;
}
#endif

/**
 * check_move_unevictable_pages - check pages for evictability and move to
 * appropriate zone lru list
 * @pvec: pagevec with lru pages to check
 *
 * Checks pages for evictability, if an evictable page is in the unevictable
 * lru list, moves it to the appropriate evictable lru list. This function
 * should be only used for lru pages.
 */
void check_move_unevictable_pages(struct pagevec *pvec)
{
	struct lruvec *lruvec = NULL;
	int pgscanned = 0;
	int pgrescued = 0;
	int i;

	for (i = 0; i < pvec->nr; i++) {
		struct page *page = pvec->pages[i];
		int nr_pages;

		if (PageTransTail(page))
			continue;

		nr_pages = thp_nr_pages(page);
		pgscanned += nr_pages;

		/* block memcg migration during page moving between lru */
		if (!TestClearPageLRU(page))
			continue;

		lruvec = relock_page_lruvec_irq(page, lruvec);
		if (page_evictable(page) && PageUnevictable(page)) {
			del_page_from_lru_list(page, lruvec);
			ClearPageUnevictable(page);
			add_page_to_lru_list(page, lruvec);
			pgrescued += nr_pages;
		}
		SetPageLRU(page);
	}

	if (lruvec) {
		__count_vm_events(UNEVICTABLE_PGRESCUED, pgrescued);
		__count_vm_events(UNEVICTABLE_PGSCANNED, pgscanned);
		unlock_page_lruvec_irq(lruvec);
	} else if (pgscanned) {
		count_vm_events(UNEVICTABLE_PGSCANNED, pgscanned);
	}
}
EXPORT_SYMBOL_GPL(check_move_unevictable_pages);
