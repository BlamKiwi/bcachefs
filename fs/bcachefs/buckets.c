// SPDX-License-Identifier: GPL-2.0
/*
 * Code for manipulating bucket marks for garbage collection.
 *
 * Copyright 2014 Datera, Inc.
 *
 * Bucket states:
 * - free bucket: mark == 0
 *   The bucket contains no data and will not be read
 *
 * - allocator bucket: owned_by_allocator == 1
 *   The bucket is on a free list, or it is an open bucket
 *
 * - cached bucket: owned_by_allocator == 0 &&
 *                  dirty_sectors == 0 &&
 *                  cached_sectors > 0
 *   The bucket contains data but may be safely discarded as there are
 *   enough replicas of the data on other cache devices, or it has been
 *   written back to the backing device
 *
 * - dirty bucket: owned_by_allocator == 0 &&
 *                 dirty_sectors > 0
 *   The bucket contains data that we must not discard (either only copy,
 *   or one of the 'main copies' for data requiring multiple replicas)
 *
 * - metadata bucket: owned_by_allocator == 0 && is_metadata == 1
 *   This is a btree node, journal or gen/prio bucket
 *
 * Lifecycle:
 *
 * bucket invalidated => bucket on freelist => open bucket =>
 *     [dirty bucket =>] cached bucket => bucket invalidated => ...
 *
 * Note that cache promotion can skip the dirty bucket step, as data
 * is copied from a deeper tier to a shallower tier, onto a cached
 * bucket.
 * Note also that a cached bucket can spontaneously become dirty --
 * see below.
 *
 * Only a traversal of the key space can determine whether a bucket is
 * truly dirty or cached.
 *
 * Transitions:
 *
 * - free => allocator: bucket was invalidated
 * - cached => allocator: bucket was invalidated
 *
 * - allocator => dirty: open bucket was filled up
 * - allocator => cached: open bucket was filled up
 * - allocator => metadata: metadata was allocated
 *
 * - dirty => cached: dirty sectors were copied to a deeper tier
 * - dirty => free: dirty sectors were overwritten or moved (copy gc)
 * - cached => free: cached sectors were overwritten
 *
 * - metadata => free: metadata was freed
 *
 * Oddities:
 * - cached => dirty: a device was removed so formerly replicated data
 *                    is no longer sufficiently replicated
 * - free => cached: cannot happen
 * - free => dirty: cannot happen
 * - free => metadata: cannot happen
 */

#include "bcachefs.h"
#include "alloc_background.h"
#include "bset.h"
#include "btree_gc.h"
#include "btree_update.h"
#include "buckets.h"
#include "ec.h"
#include "error.h"
#include "movinggc.h"
#include "replicas.h"

#include <linux/preempt.h>
#include <trace/events/bcachefs.h>

static inline void fs_usage_data_type_to_base(struct bch_fs_usage *fs_usage,
					      enum bch_data_type data_type,
					      s64 sectors)
{
	switch (data_type) {
	case BCH_DATA_btree:
		fs_usage->btree		+= sectors;
		break;
	case BCH_DATA_user:
	case BCH_DATA_parity:
		fs_usage->data		+= sectors;
		break;
	case BCH_DATA_cached:
		fs_usage->cached	+= sectors;
		break;
	default:
		break;
	}
}

/*
 * Clear journal_seq_valid for buckets for which it's not needed, to prevent
 * wraparound:
 */
void bch2_bucket_seq_cleanup(struct bch_fs *c)
{
	u64 journal_seq = atomic64_read(&c->journal.seq);
	u16 last_seq_ondisk = c->journal.last_seq_ondisk;
	struct bch_dev *ca;
	struct bucket_array *buckets;
	struct bucket *g;
	struct bucket_mark m;
	unsigned i;

	if (journal_seq - c->last_bucket_seq_cleanup <
	    (1U << (BUCKET_JOURNAL_SEQ_BITS - 2)))
		return;

	c->last_bucket_seq_cleanup = journal_seq;

	for_each_member_device(ca, c, i) {
		down_read(&ca->bucket_lock);
		buckets = bucket_array(ca);

		for_each_bucket(g, buckets) {
			bucket_cmpxchg(g, m, ({
				if (!m.journal_seq_valid ||
				    bucket_needs_journal_commit(m, last_seq_ondisk))
					break;

				m.journal_seq_valid = 0;
			}));
		}
		up_read(&ca->bucket_lock);
	}
}

void bch2_fs_usage_initialize(struct bch_fs *c)
{
	struct bch_fs_usage *usage;
	struct bch_dev *ca;
	unsigned i;

	percpu_down_write(&c->mark_lock);
	usage = c->usage_base;

	for (i = 0; i < ARRAY_SIZE(c->usage); i++)
		bch2_fs_usage_acc_to_base(c, i);

	for (i = 0; i < BCH_REPLICAS_MAX; i++)
		usage->reserved += usage->persistent_reserved[i];

	for (i = 0; i < c->replicas.nr; i++) {
		struct bch_replicas_entry *e =
			cpu_replicas_entry(&c->replicas, i);

		fs_usage_data_type_to_base(usage, e->data_type, usage->replicas[i]);
	}

	for_each_member_device(ca, c, i) {
		struct bch_dev_usage dev = bch2_dev_usage_read(ca);

		usage->hidden += (dev.d[BCH_DATA_sb].buckets +
				  dev.d[BCH_DATA_journal].buckets) *
			ca->mi.bucket_size;
	}

	percpu_up_write(&c->mark_lock);
}

void bch2_fs_usage_scratch_put(struct bch_fs *c, struct bch_fs_usage *fs_usage)
{
	if (fs_usage == c->usage_scratch)
		mutex_unlock(&c->usage_scratch_lock);
	else
		kfree(fs_usage);
}

struct bch_fs_usage *bch2_fs_usage_scratch_get(struct bch_fs *c)
{
	struct bch_fs_usage *ret;
	unsigned bytes = fs_usage_u64s(c) * sizeof(u64);

	ret = kzalloc(bytes, GFP_NOWAIT|__GFP_NOWARN);
	if (ret)
		return ret;

	if (mutex_trylock(&c->usage_scratch_lock))
		goto out_pool;

	ret = kzalloc(bytes, GFP_NOFS);
	if (ret)
		return ret;

	mutex_lock(&c->usage_scratch_lock);
out_pool:
	ret = c->usage_scratch;
	memset(ret, 0, bytes);
	return ret;
}

static inline struct bch_dev_usage *dev_usage_ptr(struct bch_dev *ca,
						  unsigned journal_seq,
						  bool gc)
{
	return this_cpu_ptr(gc
			    ? ca->usage_gc
			    : ca->usage[journal_seq & JOURNAL_BUF_MASK]);
}

struct bch_dev_usage bch2_dev_usage_read(struct bch_dev *ca)
{
	struct bch_fs *c = ca->fs;
	struct bch_dev_usage ret;
	unsigned seq, i, u64s = dev_usage_u64s();

	do {
		seq = read_seqcount_begin(&c->usage_lock);
		memcpy(&ret, ca->usage_base, u64s * sizeof(u64));
		for (i = 0; i < ARRAY_SIZE(ca->usage); i++)
			acc_u64s_percpu((u64 *) &ret, (u64 __percpu *) ca->usage[i], u64s);
	} while (read_seqcount_retry(&c->usage_lock, seq));

	return ret;
}

static inline struct bch_fs_usage *fs_usage_ptr(struct bch_fs *c,
						unsigned journal_seq,
						bool gc)
{
	return this_cpu_ptr(gc
			    ? c->usage_gc
			    : c->usage[journal_seq & JOURNAL_BUF_MASK]);
}

u64 bch2_fs_usage_read_one(struct bch_fs *c, u64 *v)
{
	ssize_t offset = v - (u64 *) c->usage_base;
	unsigned i, seq;
	u64 ret;

	BUG_ON(offset < 0 || offset >= fs_usage_u64s(c));
	percpu_rwsem_assert_held(&c->mark_lock);

	do {
		seq = read_seqcount_begin(&c->usage_lock);
		ret = *v;

		for (i = 0; i < ARRAY_SIZE(c->usage); i++)
			ret += percpu_u64_get((u64 __percpu *) c->usage[i] + offset);
	} while (read_seqcount_retry(&c->usage_lock, seq));

	return ret;
}

struct bch_fs_usage *bch2_fs_usage_read(struct bch_fs *c)
{
	struct bch_fs_usage *ret;
	unsigned seq, i, v, u64s = fs_usage_u64s(c);
retry:
	ret = kmalloc(u64s * sizeof(u64), GFP_NOFS);
	if (unlikely(!ret))
		return NULL;

	percpu_down_read(&c->mark_lock);

	v = fs_usage_u64s(c);
	if (unlikely(u64s != v)) {
		u64s = v;
		percpu_up_read(&c->mark_lock);
		kfree(ret);
		goto retry;
	}

	do {
		seq = read_seqcount_begin(&c->usage_lock);
		memcpy(ret, c->usage_base, u64s * sizeof(u64));
		for (i = 0; i < ARRAY_SIZE(c->usage); i++)
			acc_u64s_percpu((u64 *) ret, (u64 __percpu *) c->usage[i], u64s);
	} while (read_seqcount_retry(&c->usage_lock, seq));

	return ret;
}

void bch2_fs_usage_acc_to_base(struct bch_fs *c, unsigned idx)
{
	struct bch_dev *ca;
	unsigned i, u64s = fs_usage_u64s(c);

	BUG_ON(idx >= ARRAY_SIZE(c->usage));

	preempt_disable();
	write_seqcount_begin(&c->usage_lock);

	acc_u64s_percpu((u64 *) c->usage_base,
			(u64 __percpu *) c->usage[idx], u64s);
	percpu_memset(c->usage[idx], 0, u64s * sizeof(u64));

	rcu_read_lock();
	for_each_member_device_rcu(ca, c, i, NULL) {
		u64s = dev_usage_u64s();

		acc_u64s_percpu((u64 *) ca->usage_base,
				(u64 __percpu *) ca->usage[idx], u64s);
		percpu_memset(ca->usage[idx], 0, u64s * sizeof(u64));
	}
	rcu_read_unlock();

	write_seqcount_end(&c->usage_lock);
	preempt_enable();
}

void bch2_fs_usage_to_text(struct printbuf *out,
			   struct bch_fs *c,
			   struct bch_fs_usage *fs_usage)
{
	unsigned i;

	pr_buf(out, "capacity:\t\t\t%llu\n", c->capacity);

	pr_buf(out, "hidden:\t\t\t\t%llu\n",
	       fs_usage->hidden);
	pr_buf(out, "data:\t\t\t\t%llu\n",
	       fs_usage->data);
	pr_buf(out, "cached:\t\t\t\t%llu\n",
	       fs_usage->cached);
	pr_buf(out, "reserved:\t\t\t%llu\n",
	       fs_usage->reserved);
	pr_buf(out, "nr_inodes:\t\t\t%llu\n",
	       fs_usage->nr_inodes);
	pr_buf(out, "online reserved:\t\t%llu\n",
	       fs_usage->online_reserved);

	for (i = 0;
	     i < ARRAY_SIZE(fs_usage->persistent_reserved);
	     i++) {
		pr_buf(out, "%u replicas:\n", i + 1);
		pr_buf(out, "\treserved:\t\t%llu\n",
		       fs_usage->persistent_reserved[i]);
	}

	for (i = 0; i < c->replicas.nr; i++) {
		struct bch_replicas_entry *e =
			cpu_replicas_entry(&c->replicas, i);

		pr_buf(out, "\t");
		bch2_replicas_entry_to_text(out, e);
		pr_buf(out, ":\t%llu\n", fs_usage->replicas[i]);
	}
}

#define RESERVE_FACTOR	6

static u64 reserve_factor(u64 r)
{
	return r + (round_up(r, (1 << RESERVE_FACTOR)) >> RESERVE_FACTOR);
}

static u64 avail_factor(u64 r)
{
	return div_u64(r << RESERVE_FACTOR, (1 << RESERVE_FACTOR) + 1);
}

u64 bch2_fs_sectors_used(struct bch_fs *c, struct bch_fs_usage *fs_usage)
{
	return min(fs_usage->hidden +
		   fs_usage->btree +
		   fs_usage->data +
		   reserve_factor(fs_usage->reserved +
				  fs_usage->online_reserved),
		   c->capacity);
}

static struct bch_fs_usage_short
__bch2_fs_usage_read_short(struct bch_fs *c)
{
	struct bch_fs_usage_short ret;
	u64 data, reserved;

	ret.capacity = c->capacity -
		bch2_fs_usage_read_one(c, &c->usage_base->hidden);

	data		= bch2_fs_usage_read_one(c, &c->usage_base->data) +
		bch2_fs_usage_read_one(c, &c->usage_base->btree);
	reserved	= bch2_fs_usage_read_one(c, &c->usage_base->reserved) +
		bch2_fs_usage_read_one(c, &c->usage_base->online_reserved);

	ret.used	= min(ret.capacity, data + reserve_factor(reserved));
	ret.free	= ret.capacity - ret.used;

	ret.nr_inodes	= bch2_fs_usage_read_one(c, &c->usage_base->nr_inodes);

	return ret;
}

struct bch_fs_usage_short
bch2_fs_usage_read_short(struct bch_fs *c)
{
	struct bch_fs_usage_short ret;

	percpu_down_read(&c->mark_lock);
	ret = __bch2_fs_usage_read_short(c);
	percpu_up_read(&c->mark_lock);

	return ret;
}

static inline int is_unavailable_bucket(struct bucket_mark m)
{
	return !is_available_bucket(m);
}

static inline int bucket_sectors_fragmented(struct bch_dev *ca,
					    struct bucket_mark m)
{
	return bucket_sectors_used(m)
		? max(0, (int) ca->mi.bucket_size - (int) bucket_sectors_used(m))
		: 0;
}

static inline int is_stripe_data_bucket(struct bucket_mark m)
{
	return m.stripe && m.data_type != BCH_DATA_parity;
}

static inline enum bch_data_type bucket_type(struct bucket_mark m)
{
	return m.cached_sectors && !m.dirty_sectors
		? BCH_DATA_cached
		: m.data_type;
}

static bool bucket_became_unavailable(struct bucket_mark old,
				      struct bucket_mark new)
{
	return is_available_bucket(old) &&
	       !is_available_bucket(new);
}

int bch2_fs_usage_apply(struct bch_fs *c,
			struct bch_fs_usage *fs_usage,
			struct disk_reservation *disk_res,
			unsigned journal_seq)
{
	s64 added = fs_usage->data + fs_usage->reserved;
	s64 should_not_have_added;
	int ret = 0;

	percpu_rwsem_assert_held(&c->mark_lock);

	/*
	 * Not allowed to reduce sectors_available except by getting a
	 * reservation:
	 */
	should_not_have_added = added - (s64) (disk_res ? disk_res->sectors : 0);
	if (WARN_ONCE(should_not_have_added > 0,
		      "disk usage increased by %lli more than reservation of %llu",
		      added, disk_res ? disk_res->sectors : 0)) {
		atomic64_sub(should_not_have_added, &c->sectors_available);
		added -= should_not_have_added;
		ret = -1;
	}

	if (added > 0) {
		disk_res->sectors		-= added;
		fs_usage->online_reserved	-= added;
	}

	preempt_disable();
	acc_u64s((u64 *) fs_usage_ptr(c, journal_seq, false),
		 (u64 *) fs_usage, fs_usage_u64s(c));
	preempt_enable();

	return ret;
}

static inline void account_bucket(struct bch_fs_usage *fs_usage,
				  struct bch_dev_usage *dev_usage,
				  enum bch_data_type type,
				  int nr, s64 size)
{
	if (type == BCH_DATA_sb || type == BCH_DATA_journal)
		fs_usage->hidden	+= size;

	dev_usage->d[type].buckets	+= nr;
}

static void bch2_dev_usage_update(struct bch_fs *c, struct bch_dev *ca,
				  struct bch_fs_usage *fs_usage,
				  struct bucket_mark old, struct bucket_mark new,
				  u64 journal_seq, bool gc)
{
	struct bch_dev_usage *u;

	percpu_rwsem_assert_held(&c->mark_lock);

	preempt_disable();
	u = dev_usage_ptr(ca, journal_seq, gc);

	if (bucket_type(old))
		account_bucket(fs_usage, u, bucket_type(old),
			       -1, -ca->mi.bucket_size);

	if (bucket_type(new))
		account_bucket(fs_usage, u, bucket_type(new),
			       1, ca->mi.bucket_size);

	u->buckets_alloc +=
		(int) new.owned_by_allocator - (int) old.owned_by_allocator;
	u->buckets_ec += (int) new.stripe - (int) old.stripe;
	u->buckets_unavailable +=
		is_unavailable_bucket(new) - is_unavailable_bucket(old);

	u->d[old.data_type].sectors -= old.dirty_sectors;
	u->d[new.data_type].sectors += new.dirty_sectors;
	u->d[BCH_DATA_cached].sectors +=
		(int) new.cached_sectors - (int) old.cached_sectors;

	u->d[old.data_type].fragmented -= bucket_sectors_fragmented(ca, old);
	u->d[new.data_type].fragmented += bucket_sectors_fragmented(ca, new);

	preempt_enable();

	if (!is_available_bucket(old) && is_available_bucket(new))
		bch2_wake_allocator(ca);
}

static inline int update_replicas(struct bch_fs *c,
				  struct bch_fs_usage *fs_usage,
				  struct bch_replicas_entry *r,
				  s64 sectors)
{
	int idx = bch2_replicas_entry_idx(c, r);

	if (idx < 0)
		return -1;

	if (!fs_usage)
		return 0;

	fs_usage_data_type_to_base(fs_usage, r->data_type, sectors);
	fs_usage->replicas[idx]		+= sectors;
	return 0;
}

static inline void update_cached_sectors(struct bch_fs *c,
					 struct bch_fs_usage *fs_usage,
					 unsigned dev, s64 sectors)
{
	struct bch_replicas_padded r;

	bch2_replicas_entry_cached(&r.e, dev);

	update_replicas(c, fs_usage, &r.e, sectors);
}

static struct replicas_delta_list *
replicas_deltas_realloc(struct btree_trans *trans, unsigned more)
{
	struct replicas_delta_list *d = trans->fs_usage_deltas;
	unsigned new_size = d ? (d->size + more) * 2 : 128;

	if (!d || d->used + more > d->size) {
		d = krealloc(d, sizeof(*d) + new_size, GFP_NOIO|__GFP_ZERO);
		BUG_ON(!d);

		d->size = new_size;
		trans->fs_usage_deltas = d;
	}
	return d;
}

static inline void update_replicas_list(struct btree_trans *trans,
					struct bch_replicas_entry *r,
					s64 sectors)
{
	struct replicas_delta_list *d;
	struct replicas_delta *n;
	unsigned b;

	if (!sectors)
		return;

	b = replicas_entry_bytes(r) + 8;
	d = replicas_deltas_realloc(trans, b);

	n = (void *) d->d + d->used;
	n->delta = sectors;
	memcpy(&n->r, r, replicas_entry_bytes(r));
	d->used += b;
}

static inline void update_cached_sectors_list(struct btree_trans *trans,
					      unsigned dev, s64 sectors)
{
	struct bch_replicas_padded r;

	bch2_replicas_entry_cached(&r.e, dev);

	update_replicas_list(trans, &r.e, sectors);
}

static inline struct replicas_delta *
replicas_delta_next(struct replicas_delta *d)
{
	return (void *) d + replicas_entry_bytes(&d->r) + 8;
}

int bch2_replicas_delta_list_apply(struct bch_fs *c,
				   struct bch_fs_usage *fs_usage,
				   struct replicas_delta_list *r)
{
	struct replicas_delta *d = r->d;
	struct replicas_delta *top = (void *) r->d + r->used;
	unsigned i;

	for (d = r->d; d != top; d = replicas_delta_next(d))
		if (update_replicas(c, fs_usage, &d->r, d->delta)) {
			top = d;
			goto unwind;
		}

	if (!fs_usage)
		return 0;

	fs_usage->nr_inodes += r->nr_inodes;

	for (i = 0; i < BCH_REPLICAS_MAX; i++) {
		fs_usage->reserved += r->persistent_reserved[i];
		fs_usage->persistent_reserved[i] += r->persistent_reserved[i];
	}

	return 0;
unwind:
	for (d = r->d; d != top; d = replicas_delta_next(d))
		update_replicas(c, fs_usage, &d->r, -d->delta);
	return -1;
}

#define do_mark_fn(fn, c, pos, flags, ...)				\
({									\
	int gc, ret = 0;						\
									\
	percpu_rwsem_assert_held(&c->mark_lock);			\
									\
	for (gc = 0; gc < 2 && !ret; gc++)				\
		if (!gc == !(flags & BTREE_TRIGGER_GC) ||		\
		    (gc && gc_visited(c, pos)))				\
			ret = fn(c, __VA_ARGS__, gc);			\
	ret;								\
})

static int __bch2_mark_alloc_bucket(struct bch_fs *c, struct bch_dev *ca,
				    size_t b, bool owned_by_allocator,
				    bool gc)
{
	struct bch_fs_usage *fs_usage = fs_usage_ptr(c, 0, gc);
	struct bucket *g = __bucket(ca, b, gc);
	struct bucket_mark old, new;

	old = bucket_cmpxchg(g, new, ({
		new.owned_by_allocator	= owned_by_allocator;
	}));

	/*
	 * XXX: this is wrong, this means we'll be doing updates to the percpu
	 * buckets_alloc counter that don't have an open journal buffer and
	 * we'll race with the machinery that accumulates that to ca->usage_base
	 */
	bch2_dev_usage_update(c, ca, fs_usage, old, new, 0, gc);

	BUG_ON(!gc &&
	       !owned_by_allocator && !old.owned_by_allocator);

	return 0;
}

void bch2_mark_alloc_bucket(struct bch_fs *c, struct bch_dev *ca,
			    size_t b, bool owned_by_allocator,
			    struct gc_pos pos, unsigned flags)
{
	preempt_disable();

	do_mark_fn(__bch2_mark_alloc_bucket, c, pos, flags,
		   ca, b, owned_by_allocator);

	preempt_enable();
}

static int bch2_mark_alloc(struct bch_fs *c,
			   struct bkey_s_c old, struct bkey_s_c new,
			   struct bch_fs_usage *fs_usage,
			   u64 journal_seq, unsigned flags)
{
	bool gc = flags & BTREE_TRIGGER_GC;
	struct bkey_alloc_unpacked u;
	struct bch_dev *ca;
	struct bucket *g;
	struct bucket_mark old_m, m;

	/* We don't do anything for deletions - do we?: */
	if (new.k->type != KEY_TYPE_alloc &&
	    new.k->type != KEY_TYPE_alloc_v2)
		return 0;

	/*
	 * alloc btree is read in by bch2_alloc_read, not gc:
	 */
	if ((flags & BTREE_TRIGGER_GC) &&
	    !(flags & BTREE_TRIGGER_BUCKET_INVALIDATE))
		return 0;

	ca = bch_dev_bkey_exists(c, new.k->p.inode);

	if (new.k->p.offset >= ca->mi.nbuckets)
		return 0;

	g = __bucket(ca, new.k->p.offset, gc);
	u = bch2_alloc_unpack(new);

	old_m = bucket_cmpxchg(g, m, ({
		m.gen			= u.gen;
		m.data_type		= u.data_type;
		m.dirty_sectors		= u.dirty_sectors;
		m.cached_sectors	= u.cached_sectors;
		m.stripe		= u.stripe != 0;

		if (journal_seq) {
			m.journal_seq_valid	= 1;
			m.journal_seq		= journal_seq;
		}
	}));

	bch2_dev_usage_update(c, ca, fs_usage, old_m, m, journal_seq, gc);

	g->io_time[READ]	= u.read_time;
	g->io_time[WRITE]	= u.write_time;
	g->oldest_gen		= u.oldest_gen;
	g->gen_valid		= 1;
	g->stripe		= u.stripe;
	g->stripe_redundancy	= u.stripe_redundancy;

	/*
	 * need to know if we're getting called from the invalidate path or
	 * not:
	 */

	if ((flags & BTREE_TRIGGER_BUCKET_INVALIDATE) &&
	    old_m.cached_sectors) {
		update_cached_sectors(c, fs_usage, ca->dev_idx,
				      -old_m.cached_sectors);
		trace_invalidate(ca, bucket_to_sector(ca, new.k->p.offset),
				 old_m.cached_sectors);
	}

	return 0;
}

#define checked_add(a, b)					\
({								\
	unsigned _res = (unsigned) (a) + (b);			\
	bool overflow = _res > U16_MAX;				\
	if (overflow)						\
		_res = U16_MAX;					\
	(a) = _res;						\
	overflow;						\
})

static int __bch2_mark_metadata_bucket(struct bch_fs *c, struct bch_dev *ca,
				       size_t b, enum bch_data_type data_type,
				       unsigned sectors, bool gc)
{
	struct bucket *g = __bucket(ca, b, gc);
	struct bucket_mark old, new;
	bool overflow;

	BUG_ON(data_type != BCH_DATA_sb &&
	       data_type != BCH_DATA_journal);

	old = bucket_cmpxchg(g, new, ({
		new.data_type	= data_type;
		overflow = checked_add(new.dirty_sectors, sectors);
	}));

	bch2_fs_inconsistent_on(old.data_type &&
				old.data_type != data_type, c,
		"different types of data in same bucket: %s, %s",
		bch2_data_types[old.data_type],
		bch2_data_types[data_type]);

	bch2_fs_inconsistent_on(overflow, c,
		"bucket %u:%zu gen %u data type %s sector count overflow: %u + %u > U16_MAX",
		ca->dev_idx, b, new.gen,
		bch2_data_types[old.data_type ?: data_type],
		old.dirty_sectors, sectors);

	if (c)
		bch2_dev_usage_update(c, ca, fs_usage_ptr(c, 0, gc),
				      old, new, 0, gc);

	return 0;
}

void bch2_mark_metadata_bucket(struct bch_fs *c, struct bch_dev *ca,
			       size_t b, enum bch_data_type type,
			       unsigned sectors, struct gc_pos pos,
			       unsigned flags)
{
	BUG_ON(type != BCH_DATA_sb &&
	       type != BCH_DATA_journal);

	preempt_disable();

	if (likely(c)) {
		do_mark_fn(__bch2_mark_metadata_bucket, c, pos, flags,
			   ca, b, type, sectors);
	} else {
		__bch2_mark_metadata_bucket(c, ca, b, type, sectors, 0);
	}

	preempt_enable();
}

static s64 disk_sectors_scaled(unsigned n, unsigned d, unsigned sectors)
{
	return DIV_ROUND_UP(sectors * n, d);
}

static s64 __ptr_disk_sectors_delta(unsigned old_size,
				    unsigned offset, s64 delta,
				    unsigned flags,
				    unsigned n, unsigned d)
{
	BUG_ON(!n || !d);

	if (flags & BTREE_TRIGGER_OVERWRITE_SPLIT) {
		BUG_ON(offset + -delta > old_size);

		return -disk_sectors_scaled(n, d, old_size) +
			disk_sectors_scaled(n, d, offset) +
			disk_sectors_scaled(n, d, old_size - offset + delta);
	} else if (flags & BTREE_TRIGGER_OVERWRITE) {
		BUG_ON(offset + -delta > old_size);

		return -disk_sectors_scaled(n, d, old_size) +
			disk_sectors_scaled(n, d, old_size + delta);
	} else {
		return  disk_sectors_scaled(n, d, delta);
	}
}

static s64 ptr_disk_sectors_delta(struct extent_ptr_decoded p,
				  unsigned offset, s64 delta,
				  unsigned flags)
{
	return __ptr_disk_sectors_delta(p.crc.live_size,
					offset, delta, flags,
					p.crc.compressed_size,
					p.crc.uncompressed_size);
}

static int check_bucket_ref(struct bch_fs *c, struct bkey_s_c k,
			    const struct bch_extent_ptr *ptr,
			    s64 sectors, enum bch_data_type ptr_data_type,
			    u8 bucket_gen, u8 bucket_data_type,
			    u16 dirty_sectors, u16 cached_sectors)
{
	size_t bucket_nr = PTR_BUCKET_NR(bch_dev_bkey_exists(c, ptr->dev), ptr);
	u16 bucket_sectors = !ptr->cached
		? dirty_sectors
		: cached_sectors;
	char buf[200];

	if (gen_after(ptr->gen, bucket_gen)) {
		bch2_fsck_err(c, FSCK_CAN_IGNORE|FSCK_NEED_FSCK,
			"bucket %u:%zu gen %u data type %s: ptr gen %u newer than bucket gen\n"
			"while marking %s",
			ptr->dev, bucket_nr, bucket_gen,
			bch2_data_types[bucket_data_type ?: ptr_data_type],
			ptr->gen,
			(bch2_bkey_val_to_text(&PBUF(buf), c, k), buf));
		return -EIO;
	}

	if (gen_cmp(bucket_gen, ptr->gen) > BUCKET_GC_GEN_MAX) {
		bch2_fsck_err(c, FSCK_CAN_IGNORE|FSCK_NEED_FSCK,
			"bucket %u:%zu gen %u data type %s: ptr gen %u too stale\n"
			"while marking %s",
			ptr->dev, bucket_nr, bucket_gen,
			bch2_data_types[bucket_data_type ?: ptr_data_type],
			ptr->gen,
			(bch2_bkey_val_to_text(&PBUF(buf), c, k), buf));
		return -EIO;
	}

	if (bucket_gen != ptr->gen && !ptr->cached) {
		bch2_fsck_err(c, FSCK_CAN_IGNORE|FSCK_NEED_FSCK,
			"bucket %u:%zu gen %u data type %s: stale dirty ptr (gen %u)\n"
			"while marking %s",
			ptr->dev, bucket_nr, bucket_gen,
			bch2_data_types[bucket_data_type ?: ptr_data_type],
			ptr->gen,
			(bch2_bkey_val_to_text(&PBUF(buf), c, k), buf));
		return -EIO;
	}

	if (bucket_gen != ptr->gen)
		return 1;

	if (bucket_data_type && ptr_data_type &&
	    bucket_data_type != ptr_data_type) {
		bch2_fsck_err(c, FSCK_CAN_IGNORE|FSCK_NEED_FSCK,
			"bucket %u:%zu gen %u different types of data in same bucket: %s, %s\n"
			"while marking %s",
			ptr->dev, bucket_nr, bucket_gen,
			bch2_data_types[bucket_data_type],
			bch2_data_types[ptr_data_type],
			(bch2_bkey_val_to_text(&PBUF(buf), c, k), buf));
		return -EIO;
	}

	if ((unsigned) (bucket_sectors + sectors) > U16_MAX) {
		bch2_fsck_err(c, FSCK_CAN_IGNORE|FSCK_NEED_FSCK,
			"bucket %u:%zu gen %u data type %s sector count overflow: %u + %lli > U16_MAX\n"
			"while marking %s",
			ptr->dev, bucket_nr, bucket_gen,
			bch2_data_types[bucket_data_type ?: ptr_data_type],
			bucket_sectors, sectors,
			(bch2_bkey_val_to_text(&PBUF(buf), c, k), buf));
		return -EIO;
	}

	return 0;
}

static int mark_stripe_bucket(struct bch_fs *c, struct bkey_s_c k,
			     unsigned ptr_idx,
			     struct bch_fs_usage *fs_usage,
			     u64 journal_seq, unsigned flags)
{
	const struct bch_stripe *s = bkey_s_c_to_stripe(k).v;
	unsigned nr_data = s->nr_blocks - s->nr_redundant;
	bool parity = ptr_idx >= nr_data;
	const struct bch_extent_ptr *ptr = s->ptrs + ptr_idx;
	bool gc = flags & BTREE_TRIGGER_GC;
	struct bch_dev *ca = bch_dev_bkey_exists(c, ptr->dev);
	struct bucket *g = PTR_BUCKET(ca, ptr, gc);
	struct bucket_mark new, old;
	char buf[200];
	int ret;

	if (g->stripe && g->stripe != k.k->p.offset) {
		bch2_fs_inconsistent(c,
			      "bucket %u:%zu gen %u: multiple stripes using same bucket\n%s",
			      ptr->dev, PTR_BUCKET_NR(ca, ptr), new.gen,
			      (bch2_bkey_val_to_text(&PBUF(buf), c, k), buf));
		return -EINVAL;
	}

	old = bucket_cmpxchg(g, new, ({
		ret = check_bucket_ref(c, k, ptr, 0, 0, new.gen, new.data_type,
				       new.dirty_sectors, new.cached_sectors);
		if (ret)
			return ret;

		if (parity) {
			new.data_type		= BCH_DATA_parity;
			new.dirty_sectors	= le16_to_cpu(s->sectors);
		}

		if (journal_seq) {
			new.journal_seq_valid	= 1;
			new.journal_seq		= journal_seq;
		}
	}));

	g->stripe		= k.k->p.offset;
	g->stripe_redundancy	= s->nr_redundant;

	bch2_dev_usage_update(c, ca, fs_usage, old, new, journal_seq, gc);
	return 0;
}

static int __mark_pointer(struct bch_fs *c, struct bkey_s_c k,
			  const struct bch_extent_ptr *ptr,
			  s64 sectors, enum bch_data_type ptr_data_type,
			  u8 bucket_gen, u8 *bucket_data_type,
			  u16 *dirty_sectors, u16 *cached_sectors)
{
	u16 *dst_sectors = !ptr->cached
		? dirty_sectors
		: cached_sectors;
	int ret = check_bucket_ref(c, k, ptr, sectors, ptr_data_type,
				   bucket_gen, *bucket_data_type,
				   *dirty_sectors, *cached_sectors);

	if (ret)
		return ret;

	*dst_sectors += sectors;
	*bucket_data_type = *dirty_sectors || *cached_sectors
		? ptr_data_type : 0;
	return 0;
}

static int bch2_mark_pointer(struct bch_fs *c, struct bkey_s_c k,
			     struct extent_ptr_decoded p,
			     s64 sectors, enum bch_data_type data_type,
			     struct bch_fs_usage *fs_usage,
			     u64 journal_seq, unsigned flags)
{
	bool gc = flags & BTREE_TRIGGER_GC;
	struct bucket_mark old, new;
	struct bch_dev *ca = bch_dev_bkey_exists(c, p.ptr.dev);
	struct bucket *g = PTR_BUCKET(ca, &p.ptr, gc);
	u8 bucket_data_type;
	u64 v;
	int ret;

	v = atomic64_read(&g->_mark.v);
	do {
		new.v.counter = old.v.counter = v;
		bucket_data_type = new.data_type;

		ret = __mark_pointer(c, k, &p.ptr, sectors, data_type, new.gen,
				     &bucket_data_type,
				     &new.dirty_sectors,
				     &new.cached_sectors);
		if (ret)
			return ret;

		new.data_type = bucket_data_type;

		if (journal_seq) {
			new.journal_seq_valid = 1;
			new.journal_seq = journal_seq;
		}

		if (flags & BTREE_TRIGGER_NOATOMIC) {
			g->_mark = new;
			break;
		}
	} while ((v = atomic64_cmpxchg(&g->_mark.v,
			      old.v.counter,
			      new.v.counter)) != old.v.counter);

	bch2_dev_usage_update(c, ca, fs_usage, old, new, journal_seq, gc);

	BUG_ON(!gc && bucket_became_unavailable(old, new));

	return 0;
}

static int bch2_mark_stripe_ptr(struct bch_fs *c,
				struct bch_extent_stripe_ptr p,
				enum bch_data_type data_type,
				struct bch_fs_usage *fs_usage,
				s64 sectors, unsigned flags)
{
	bool gc = flags & BTREE_TRIGGER_GC;
	struct bch_replicas_padded r;
	struct stripe *m;
	unsigned i, blocks_nonempty = 0;

	m = genradix_ptr(&c->stripes[gc], p.idx);

	spin_lock(&c->ec_stripes_heap_lock);

	if (!m || !m->alive) {
		spin_unlock(&c->ec_stripes_heap_lock);
		bch_err_ratelimited(c, "pointer to nonexistent stripe %llu",
				    (u64) p.idx);
		return -EIO;
	}

	m->block_sectors[p.block] += sectors;

	r = m->r;

	for (i = 0; i < m->nr_blocks; i++)
		blocks_nonempty += m->block_sectors[i] != 0;

	if (m->blocks_nonempty != blocks_nonempty) {
		m->blocks_nonempty = blocks_nonempty;
		if (!gc)
			bch2_stripes_heap_update(c, m, p.idx);
	}

	spin_unlock(&c->ec_stripes_heap_lock);

	r.e.data_type = data_type;
	update_replicas(c, fs_usage, &r.e, sectors);

	return 0;
}

static int bch2_mark_extent(struct bch_fs *c,
			    struct bkey_s_c old, struct bkey_s_c new,
			    unsigned offset, s64 sectors,
			    enum bch_data_type data_type,
			    struct bch_fs_usage *fs_usage,
			    unsigned journal_seq, unsigned flags)
{
	struct bkey_s_c k = flags & BTREE_TRIGGER_INSERT ? new : old;
	struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(k);
	const union bch_extent_entry *entry;
	struct extent_ptr_decoded p;
	struct bch_replicas_padded r;
	s64 dirty_sectors = 0;
	bool stale;
	int ret;

	r.e.data_type	= data_type;
	r.e.nr_devs	= 0;
	r.e.nr_required	= 1;

	BUG_ON(!sectors);

	bkey_for_each_ptr_decode(k.k, ptrs, p, entry) {
		s64 disk_sectors = data_type == BCH_DATA_btree
			? sectors
			: ptr_disk_sectors_delta(p, offset, sectors, flags);

		ret = bch2_mark_pointer(c, k, p, disk_sectors, data_type,
					fs_usage, journal_seq, flags);
		if (ret < 0)
			return ret;

		stale = ret > 0;

		if (p.ptr.cached) {
			if (!stale)
				update_cached_sectors(c, fs_usage, p.ptr.dev,
						      disk_sectors);
		} else if (!p.has_ec) {
			dirty_sectors	       += disk_sectors;
			r.e.devs[r.e.nr_devs++]	= p.ptr.dev;
		} else {
			ret = bch2_mark_stripe_ptr(c, p.ec, data_type,
					fs_usage, disk_sectors, flags);
			if (ret)
				return ret;

			/*
			 * There may be other dirty pointers in this extent, but
			 * if so they're not required for mounting if we have an
			 * erasure coded pointer in this extent:
			 */
			r.e.nr_required = 0;
		}
	}

	if (r.e.nr_devs)
		update_replicas(c, fs_usage, &r.e, dirty_sectors);

	return 0;
}

static int bch2_mark_stripe(struct bch_fs *c,
			    struct bkey_s_c old, struct bkey_s_c new,
			    struct bch_fs_usage *fs_usage,
			    u64 journal_seq, unsigned flags)
{
	bool gc = flags & BTREE_TRIGGER_GC;
	size_t idx = new.k->p.offset;
	const struct bch_stripe *old_s = old.k->type == KEY_TYPE_stripe
		? bkey_s_c_to_stripe(old).v : NULL;
	const struct bch_stripe *new_s = new.k->type == KEY_TYPE_stripe
		? bkey_s_c_to_stripe(new).v : NULL;
	struct stripe *m = genradix_ptr(&c->stripes[gc], idx);
	unsigned i;
	int ret;

	BUG_ON(gc && old_s);

	if (!m || (old_s && !m->alive)) {
		bch_err_ratelimited(c, "error marking nonexistent stripe %zu",
				    idx);
		return -1;
	}

	if (!new_s) {
		spin_lock(&c->ec_stripes_heap_lock);
		bch2_stripes_heap_del(c, m, idx);
		spin_unlock(&c->ec_stripes_heap_lock);

		memset(m, 0, sizeof(*m));
	} else {
		m->alive	= true;
		m->sectors	= le16_to_cpu(new_s->sectors);
		m->algorithm	= new_s->algorithm;
		m->nr_blocks	= new_s->nr_blocks;
		m->nr_redundant	= new_s->nr_redundant;
		m->blocks_nonempty = 0;

		for (i = 0; i < new_s->nr_blocks; i++) {
			m->block_sectors[i] =
				stripe_blockcount_get(new_s, i);
			m->blocks_nonempty += !!m->block_sectors[i];
		}

		bch2_bkey_to_replicas(&m->r.e, new);

		if (!gc) {
			spin_lock(&c->ec_stripes_heap_lock);
			bch2_stripes_heap_update(c, m, idx);
			spin_unlock(&c->ec_stripes_heap_lock);
		}
	}

	if (gc) {
		/*
		 * gc recalculates this field from stripe ptr
		 * references:
		 */
		memset(m->block_sectors, 0, sizeof(m->block_sectors));
		m->blocks_nonempty = 0;

		for (i = 0; i < new_s->nr_blocks; i++) {
			ret = mark_stripe_bucket(c, new, i, fs_usage,
						 journal_seq, flags);
			if (ret)
				return ret;
		}

		update_replicas(c, fs_usage, &m->r.e,
				((s64) m->sectors * m->nr_redundant));
	}

	return 0;
}

static int bch2_mark_key_locked(struct bch_fs *c,
		   struct bkey_s_c old,
		   struct bkey_s_c new,
		   unsigned offset, s64 sectors,
		   struct bch_fs_usage *fs_usage,
		   u64 journal_seq, unsigned flags)
{
	struct bkey_s_c k = flags & BTREE_TRIGGER_INSERT ? new : old;
	int ret = 0;

	BUG_ON(!(flags & (BTREE_TRIGGER_INSERT|BTREE_TRIGGER_OVERWRITE)));

	preempt_disable();

	if (!fs_usage || (flags & BTREE_TRIGGER_GC))
		fs_usage = fs_usage_ptr(c, journal_seq,
					flags & BTREE_TRIGGER_GC);

	switch (k.k->type) {
	case KEY_TYPE_alloc:
	case KEY_TYPE_alloc_v2:
		ret = bch2_mark_alloc(c, old, new, fs_usage, journal_seq, flags);
		break;
	case KEY_TYPE_btree_ptr:
	case KEY_TYPE_btree_ptr_v2:
		sectors = !(flags & BTREE_TRIGGER_OVERWRITE)
			?  c->opts.btree_node_size
			: -c->opts.btree_node_size;

		ret = bch2_mark_extent(c, old, new, offset, sectors,
				BCH_DATA_btree, fs_usage, journal_seq, flags);
		break;
	case KEY_TYPE_extent:
	case KEY_TYPE_reflink_v:
		ret = bch2_mark_extent(c, old, new, offset, sectors,
				BCH_DATA_user, fs_usage, journal_seq, flags);
		break;
	case KEY_TYPE_stripe:
		ret = bch2_mark_stripe(c, old, new, fs_usage, journal_seq, flags);
		break;
	case KEY_TYPE_inode:
		fs_usage->nr_inodes += new.k->type == KEY_TYPE_inode;
		fs_usage->nr_inodes -= old.k->type == KEY_TYPE_inode;
		break;
	case KEY_TYPE_reservation: {
		unsigned replicas = bkey_s_c_to_reservation(k).v->nr_replicas;

		sectors *= replicas;
		replicas = clamp_t(unsigned, replicas, 1,
				   ARRAY_SIZE(fs_usage->persistent_reserved));

		fs_usage->reserved				+= sectors;
		fs_usage->persistent_reserved[replicas - 1]	+= sectors;
		break;
	}
	}

	preempt_enable();

	return ret;
}

int bch2_mark_key(struct bch_fs *c, struct bkey_s_c new,
		  unsigned offset, s64 sectors,
		  struct bch_fs_usage *fs_usage,
		  u64 journal_seq, unsigned flags)
{
	struct bkey deleted;
	struct bkey_s_c old = (struct bkey_s_c) { &deleted, NULL };
	int ret;

	bkey_init(&deleted);

	percpu_down_read(&c->mark_lock);
	ret = bch2_mark_key_locked(c, old, new, offset, sectors,
				   fs_usage, journal_seq,
				   BTREE_TRIGGER_INSERT|flags);
	percpu_up_read(&c->mark_lock);

	return ret;
}

int bch2_mark_update(struct btree_trans *trans,
		     struct btree_iter *iter,
		     struct bkey_i *new,
		     struct bch_fs_usage *fs_usage,
		     unsigned flags)
{
	struct bch_fs		*c = trans->c;
	struct btree		*b = iter_l(iter)->b;
	struct btree_node_iter	node_iter = iter_l(iter)->iter;
	struct bkey_packed	*_old;
	struct bkey_s_c		old;
	struct bkey		unpacked;
	int ret = 0;

	if (unlikely(flags & BTREE_TRIGGER_NORUN))
		return 0;

	if (!btree_node_type_needs_gc(iter->btree_id))
		return 0;

	bkey_init(&unpacked);
	old = (struct bkey_s_c) { &unpacked, NULL };

	if (!btree_node_type_is_extents(iter->btree_id)) {
		/* iterators should be uptodate, shouldn't get errors here: */
		if (btree_iter_type(iter) != BTREE_ITER_CACHED) {
			old = bch2_btree_iter_peek_slot(iter);
			BUG_ON(bkey_err(old));
		} else {
			struct bkey_cached *ck = (void *) iter->l[0].b;

			if (ck->valid)
				old = bkey_i_to_s_c(ck->k);
		}

		if (old.k->type == new->k.type) {
			bch2_mark_key_locked(c, old, bkey_i_to_s_c(new), 0, 0,
				fs_usage, trans->journal_res.seq,
				BTREE_TRIGGER_INSERT|BTREE_TRIGGER_OVERWRITE|flags);

		} else {
			bch2_mark_key_locked(c, old, bkey_i_to_s_c(new), 0, 0,
				fs_usage, trans->journal_res.seq,
				BTREE_TRIGGER_INSERT|flags);
			bch2_mark_key_locked(c, old, bkey_i_to_s_c(new), 0, 0,
				fs_usage, trans->journal_res.seq,
				BTREE_TRIGGER_OVERWRITE|flags);
		}
	} else {
		BUG_ON(btree_iter_type(iter) == BTREE_ITER_CACHED);
		bch2_mark_key_locked(c, old, bkey_i_to_s_c(new),
			0, new->k.size,
			fs_usage, trans->journal_res.seq,
			BTREE_TRIGGER_INSERT|flags);

		while ((_old = bch2_btree_node_iter_peek(&node_iter, b))) {
			unsigned offset = 0;
			s64 sectors;

			old = bkey_disassemble(b, _old, &unpacked);
			sectors = -((s64) old.k->size);

			flags |= BTREE_TRIGGER_OVERWRITE;

			if (bkey_cmp(new->k.p, bkey_start_pos(old.k)) <= 0)
				return 0;

			switch (bch2_extent_overlap(&new->k, old.k)) {
			case BCH_EXTENT_OVERLAP_ALL:
				offset = 0;
				sectors = -((s64) old.k->size);
				break;
			case BCH_EXTENT_OVERLAP_BACK:
				offset = bkey_start_offset(&new->k) -
					bkey_start_offset(old.k);
				sectors = bkey_start_offset(&new->k) -
					old.k->p.offset;
				break;
			case BCH_EXTENT_OVERLAP_FRONT:
				offset = 0;
				sectors = bkey_start_offset(old.k) -
					new->k.p.offset;
				break;
			case BCH_EXTENT_OVERLAP_MIDDLE:
				offset = bkey_start_offset(&new->k) -
					bkey_start_offset(old.k);
				sectors = -((s64) new->k.size);
				flags |= BTREE_TRIGGER_OVERWRITE_SPLIT;
				break;
			}

			BUG_ON(sectors >= 0);

			ret = bch2_mark_key_locked(c, old, bkey_i_to_s_c(new),
					offset, sectors, fs_usage,
					trans->journal_res.seq, flags) ?: 1;
			if (ret <= 0)
				break;

			bch2_btree_node_iter_advance(&node_iter, b);
		}
	}

	return ret;
}

void bch2_trans_fs_usage_apply(struct btree_trans *trans,
			       struct bch_fs_usage *fs_usage)
{
	struct bch_fs *c = trans->c;
	struct btree_insert_entry *i;
	static int warned_disk_usage = 0;
	u64 disk_res_sectors = trans->disk_res ? trans->disk_res->sectors : 0;
	char buf[200];

	if (!bch2_fs_usage_apply(c, fs_usage, trans->disk_res,
				 trans->journal_res.seq) ||
	    warned_disk_usage ||
	    xchg(&warned_disk_usage, 1))
		return;

	bch_err(c, "disk usage increased more than %llu sectors reserved",
		disk_res_sectors);

	trans_for_each_update(trans, i) {
		pr_err("while inserting");
		bch2_bkey_val_to_text(&PBUF(buf), c, bkey_i_to_s_c(i->k));
		pr_err("%s", buf);
		pr_err("overlapping with");

		if (btree_iter_type(i->iter) != BTREE_ITER_CACHED) {
			struct btree		*b = iter_l(i->iter)->b;
			struct btree_node_iter	node_iter = iter_l(i->iter)->iter;
			struct bkey_packed	*_k;

			while ((_k = bch2_btree_node_iter_peek(&node_iter, b))) {
				struct bkey		unpacked;
				struct bkey_s_c		k;

				pr_info("_k %px format %u", _k, _k->format);
				k = bkey_disassemble(b, _k, &unpacked);

				if (btree_node_is_extents(b)
				    ? bkey_cmp(i->k->k.p, bkey_start_pos(k.k)) <= 0
				    : bkey_cmp(i->k->k.p, k.k->p))
					break;

				bch2_bkey_val_to_text(&PBUF(buf), c, k);
				pr_err("%s", buf);

				bch2_btree_node_iter_advance(&node_iter, b);
			}
		} else {
			struct bkey_cached *ck = (void *) i->iter->l[0].b;

			if (ck->valid) {
				bch2_bkey_val_to_text(&PBUF(buf), c, bkey_i_to_s_c(ck->k));
				pr_err("%s", buf);
			}
		}
	}
}

/* trans_mark: */

static struct btree_iter *trans_get_update(struct btree_trans *trans,
			    enum btree_id btree_id, struct bpos pos,
			    struct bkey_s_c *k)
{
	struct btree_insert_entry *i;

	trans_for_each_update(trans, i)
		if (i->iter->btree_id == btree_id &&
		    (btree_node_type_is_extents(btree_id)
		     ? bkey_cmp(pos, bkey_start_pos(&i->k->k)) >= 0 &&
		       bkey_cmp(pos, i->k->k.p) < 0
		     : !bkey_cmp(pos, i->iter->pos))) {
			*k = bkey_i_to_s_c(i->k);
			return i->iter;
		}

	return NULL;
}

static int trans_get_key(struct btree_trans *trans,
			 enum btree_id btree_id, struct bpos pos,
			 struct btree_iter **iter,
			 struct bkey_s_c *k)
{
	unsigned flags = btree_id != BTREE_ID_ALLOC
		? BTREE_ITER_SLOTS
		: BTREE_ITER_CACHED;
	int ret;

	*iter = trans_get_update(trans, btree_id, pos, k);
	if (*iter)
		return 1;

	*iter = bch2_trans_get_iter(trans, btree_id, pos,
				    flags|BTREE_ITER_INTENT);
	*k = __bch2_btree_iter_peek(*iter, flags);
	ret = bkey_err(*k);
	if (ret)
		bch2_trans_iter_put(trans, *iter);
	return ret;
}

static struct bkey_alloc_buf *
bch2_trans_start_alloc_update(struct btree_trans *trans, struct btree_iter **_iter,
			      const struct bch_extent_ptr *ptr,
			      struct bkey_alloc_unpacked *u)
{
	struct bch_fs *c = trans->c;
	struct bch_dev *ca = bch_dev_bkey_exists(c, ptr->dev);
	struct bpos pos = POS(ptr->dev, PTR_BUCKET_NR(ca, ptr));
	struct bucket *g;
	struct btree_iter *iter;
	struct bkey_s_c k;
	struct bkey_alloc_buf *a;
	int ret;

	a = bch2_trans_kmalloc(trans, sizeof(struct bkey_alloc_buf));
	if (IS_ERR(a))
		return a;

	iter = trans_get_update(trans, BTREE_ID_ALLOC, pos, &k);
	if (iter) {
		*u = bch2_alloc_unpack(k);
	} else {
		iter = bch2_trans_get_iter(trans, BTREE_ID_ALLOC, pos,
					   BTREE_ITER_CACHED|
					   BTREE_ITER_CACHED_NOFILL|
					   BTREE_ITER_INTENT);
		ret = bch2_btree_iter_traverse(iter);
		if (ret) {
			bch2_trans_iter_put(trans, iter);
			return ERR_PTR(ret);
		}

		percpu_down_read(&c->mark_lock);
		g = bucket(ca, pos.offset);
		*u = alloc_mem_to_key(iter, g, READ_ONCE(g->mark));
		percpu_up_read(&c->mark_lock);
	}

	*_iter = iter;
	return a;
}

static int bch2_trans_mark_pointer(struct btree_trans *trans,
			struct bkey_s_c k, struct extent_ptr_decoded p,
			s64 sectors, enum bch_data_type data_type)
{
	struct bch_fs *c = trans->c;
	struct btree_iter *iter;
	struct bkey_alloc_unpacked u;
	struct bkey_alloc_buf *a;
	int ret;

	a = bch2_trans_start_alloc_update(trans, &iter, &p.ptr, &u);
	if (IS_ERR(a))
		return PTR_ERR(a);

	ret = __mark_pointer(c, k, &p.ptr, sectors, data_type, u.gen, &u.data_type,
			     &u.dirty_sectors, &u.cached_sectors);
	if (ret)
		goto out;

	bch2_alloc_pack(c, a, u);
	bch2_trans_update(trans, iter, &a->k, 0);
out:
	bch2_trans_iter_put(trans, iter);
	return ret;
}

static int bch2_trans_mark_stripe_ptr(struct btree_trans *trans,
			struct extent_ptr_decoded p,
			s64 sectors, enum bch_data_type data_type)
{
	struct bch_fs *c = trans->c;
	struct btree_iter *iter;
	struct bkey_s_c k;
	struct bkey_i_stripe *s;
	struct bch_replicas_padded r;
	int ret = 0;

	ret = trans_get_key(trans, BTREE_ID_EC, POS(0, p.ec.idx), &iter, &k);
	if (ret < 0)
		return ret;

	if (k.k->type != KEY_TYPE_stripe) {
		bch2_fs_inconsistent(c,
			"pointer to nonexistent stripe %llu",
			(u64) p.ec.idx);
		ret = -EIO;
		goto out;
	}

	if (!bch2_ptr_matches_stripe(bkey_s_c_to_stripe(k).v, p)) {
		bch2_fs_inconsistent(c,
			"stripe pointer doesn't match stripe %llu",
			(u64) p.ec.idx);
		ret = -EIO;
		goto out;
	}

	s = bch2_trans_kmalloc(trans, bkey_bytes(k.k));
	ret = PTR_ERR_OR_ZERO(s);
	if (ret)
		goto out;

	bkey_reassemble(&s->k_i, k);
	stripe_blockcount_set(&s->v, p.ec.block,
		stripe_blockcount_get(&s->v, p.ec.block) +
		sectors);
	bch2_trans_update(trans, iter, &s->k_i, 0);

	bch2_bkey_to_replicas(&r.e, bkey_i_to_s_c(&s->k_i));
	r.e.data_type = data_type;
	update_replicas_list(trans, &r.e, sectors);
out:
	bch2_trans_iter_put(trans, iter);
	return ret;
}

static int bch2_trans_mark_extent(struct btree_trans *trans,
			struct bkey_s_c k, unsigned offset,
			s64 sectors, unsigned flags,
			enum bch_data_type data_type)
{
	struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(k);
	const union bch_extent_entry *entry;
	struct extent_ptr_decoded p;
	struct bch_replicas_padded r;
	s64 dirty_sectors = 0;
	bool stale;
	int ret;

	r.e.data_type	= data_type;
	r.e.nr_devs	= 0;
	r.e.nr_required	= 1;

	BUG_ON(!sectors);

	bkey_for_each_ptr_decode(k.k, ptrs, p, entry) {
		s64 disk_sectors = data_type == BCH_DATA_btree
			? sectors
			: ptr_disk_sectors_delta(p, offset, sectors, flags);

		ret = bch2_trans_mark_pointer(trans, k, p, disk_sectors,
					      data_type);
		if (ret < 0)
			return ret;

		stale = ret > 0;

		if (p.ptr.cached) {
			if (!stale)
				update_cached_sectors_list(trans, p.ptr.dev,
							   disk_sectors);
		} else if (!p.has_ec) {
			dirty_sectors	       += disk_sectors;
			r.e.devs[r.e.nr_devs++]	= p.ptr.dev;
		} else {
			ret = bch2_trans_mark_stripe_ptr(trans, p,
					disk_sectors, data_type);
			if (ret)
				return ret;

			r.e.nr_required = 0;
		}
	}

	if (r.e.nr_devs)
		update_replicas_list(trans, &r.e, dirty_sectors);

	return 0;
}

static int bch2_trans_mark_stripe_alloc_ref(struct btree_trans *trans,
					    struct bkey_s_c_stripe s,
					    unsigned idx, bool deleting)
{
	struct bch_fs *c = trans->c;
	const struct bch_extent_ptr *ptr = &s.v->ptrs[idx];
	struct bkey_alloc_buf *a;
	struct btree_iter *iter;
	struct bkey_alloc_unpacked u;
	bool parity = idx >= s.v->nr_blocks - s.v->nr_redundant;
	int ret = 0;

	a = bch2_trans_start_alloc_update(trans, &iter, ptr, &u);
	if (IS_ERR(a))
		return PTR_ERR(a);

	if (parity) {
		s64 sectors = le16_to_cpu(s.v->sectors);

		if (deleting)
			sectors = -sectors;

		u.dirty_sectors += sectors;
		u.data_type = u.dirty_sectors
			? BCH_DATA_parity
			: 0;
	}

	if (!deleting) {
		if (bch2_fs_inconsistent_on(u.stripe && u.stripe != s.k->p.offset, c,
				"bucket %llu:%llu gen %u: multiple stripes using same bucket (%u, %llu)",
				iter->pos.inode, iter->pos.offset, u.gen,
				u.stripe, s.k->p.offset)) {
			ret = -EIO;
			goto err;
		}

		u.stripe		= s.k->p.offset;
		u.stripe_redundancy	= s.v->nr_redundant;
	} else {
		u.stripe		= 0;
		u.stripe_redundancy	= 0;
	}

	bch2_alloc_pack(c, a, u);
	bch2_trans_update(trans, iter, &a->k, 0);
err:
	bch2_trans_iter_put(trans, iter);
	return ret;
}

static int bch2_trans_mark_stripe(struct btree_trans *trans,
				  struct bkey_s_c old, struct bkey_s_c new,
				  unsigned flags)
{
	struct bkey_s_c_stripe old_s = { NULL };
	struct bkey_s_c_stripe new_s = { NULL };
	struct bch_replicas_padded r;
	unsigned i;
	int ret = 0;

	if (old.k->type == KEY_TYPE_stripe)
		old_s = bkey_s_c_to_stripe(old);
	if (new.k->type == KEY_TYPE_stripe)
		new_s = bkey_s_c_to_stripe(new);

	/*
	 * If the pointers aren't changing, we don't need to do anything:
	 */
	if (new_s.k && old_s.k &&
	    new_s.v->nr_blocks		== old_s.v->nr_blocks &&
	    new_s.v->nr_redundant	== old_s.v->nr_redundant &&
	    !memcmp(old_s.v->ptrs, new_s.v->ptrs,
		    new_s.v->nr_blocks * sizeof(struct bch_extent_ptr)))
		return 0;

	if (new_s.k) {
		s64 sectors = le16_to_cpu(new_s.v->sectors);

		bch2_bkey_to_replicas(&r.e, new);
		update_replicas_list(trans, &r.e, sectors * new_s.v->nr_redundant);

		for (i = 0; i < new_s.v->nr_blocks; i++) {
			ret = bch2_trans_mark_stripe_alloc_ref(trans, new_s,
							       i, false);
			if (ret)
				return ret;
		}
	}

	if (old_s.k) {
		s64 sectors = -((s64) le16_to_cpu(old_s.v->sectors));

		bch2_bkey_to_replicas(&r.e, old);
		update_replicas_list(trans, &r.e, sectors * old_s.v->nr_redundant);

		for (i = 0; i < old_s.v->nr_blocks; i++) {
			ret = bch2_trans_mark_stripe_alloc_ref(trans, old_s,
							       i, true);
			if (ret)
				return ret;
		}
	}

	return ret;
}

static __le64 *bkey_refcount(struct bkey_i *k)
{
	switch (k->k.type) {
	case KEY_TYPE_reflink_v:
		return &bkey_i_to_reflink_v(k)->v.refcount;
	case KEY_TYPE_indirect_inline_data:
		return &bkey_i_to_indirect_inline_data(k)->v.refcount;
	default:
		return NULL;
	}
}

static int __bch2_trans_mark_reflink_p(struct btree_trans *trans,
			struct bkey_s_c_reflink_p p,
			u64 idx, unsigned sectors,
			unsigned flags)
{
	struct bch_fs *c = trans->c;
	struct btree_iter *iter;
	struct bkey_s_c k;
	struct bkey_i *n;
	__le64 *refcount;
	s64 ret;

	ret = trans_get_key(trans, BTREE_ID_REFLINK,
			    POS(0, idx), &iter, &k);
	if (ret < 0)
		return ret;

	if ((flags & BTREE_TRIGGER_OVERWRITE) &&
	    (bkey_start_offset(k.k) < idx ||
	     k.k->p.offset > idx + sectors))
		goto out;

	sectors = k.k->p.offset - idx;

	n = bch2_trans_kmalloc(trans, bkey_bytes(k.k));
	ret = PTR_ERR_OR_ZERO(n);
	if (ret)
		goto err;

	bkey_reassemble(n, k);

	refcount = bkey_refcount(n);
	if (!refcount) {
		bch2_fs_inconsistent(c,
			"%llu:%llu len %u points to nonexistent indirect extent %llu",
			p.k->p.inode, p.k->p.offset, p.k->size, idx);
		ret = -EIO;
		goto err;
	}

	le64_add_cpu(refcount, !(flags & BTREE_TRIGGER_OVERWRITE) ? 1 : -1);

	if (!*refcount) {
		n->k.type = KEY_TYPE_deleted;
		set_bkey_val_u64s(&n->k, 0);
	}

	bch2_btree_iter_set_pos(iter, bkey_start_pos(k.k));
	BUG_ON(iter->uptodate > BTREE_ITER_NEED_PEEK);

	bch2_trans_update(trans, iter, n, 0);
out:
	ret = sectors;
err:
	bch2_trans_iter_put(trans, iter);
	return ret;
}

static int bch2_trans_mark_reflink_p(struct btree_trans *trans,
			struct bkey_s_c_reflink_p p, unsigned offset,
			s64 sectors, unsigned flags)
{
	u64 idx = le64_to_cpu(p.v->idx) + offset;
	s64 ret = 0;

	sectors = abs(sectors);
	BUG_ON(offset + sectors > p.k->size);

	while (sectors) {
		ret = __bch2_trans_mark_reflink_p(trans, p, idx, sectors, flags);
		if (ret < 0)
			break;

		idx += ret;
		sectors = max_t(s64, 0LL, sectors - ret);
		ret = 0;
	}

	return ret;
}

int bch2_trans_mark_key(struct btree_trans *trans,
			struct bkey_s_c old,
			struct bkey_s_c new,
			unsigned offset, s64 sectors, unsigned flags)
{
	struct bch_fs *c = trans->c;
	struct bkey_s_c k = flags & BTREE_TRIGGER_INSERT ? new : old;
	struct replicas_delta_list *d;

	BUG_ON(!(flags & (BTREE_TRIGGER_INSERT|BTREE_TRIGGER_OVERWRITE)));

	switch (k.k->type) {
	case KEY_TYPE_btree_ptr:
	case KEY_TYPE_btree_ptr_v2:
		sectors = !(flags & BTREE_TRIGGER_OVERWRITE)
			?  c->opts.btree_node_size
			: -c->opts.btree_node_size;

		return bch2_trans_mark_extent(trans, k, offset, sectors,
					      flags, BCH_DATA_btree);
	case KEY_TYPE_extent:
	case KEY_TYPE_reflink_v:
		return bch2_trans_mark_extent(trans, k, offset, sectors,
					      flags, BCH_DATA_user);
	case KEY_TYPE_stripe:
		return bch2_trans_mark_stripe(trans, old, new, flags);
	case KEY_TYPE_inode: {
		int nr = (new.k->type == KEY_TYPE_inode) -
			 (old.k->type == KEY_TYPE_inode);

		if (nr) {
			d = replicas_deltas_realloc(trans, 0);
			d->nr_inodes += nr;
		}

		return 0;
	}
	case KEY_TYPE_reservation: {
		unsigned replicas = bkey_s_c_to_reservation(k).v->nr_replicas;

		d = replicas_deltas_realloc(trans, 0);

		sectors *= replicas;
		replicas = clamp_t(unsigned, replicas, 1,
				   ARRAY_SIZE(d->persistent_reserved));

		d->persistent_reserved[replicas - 1] += sectors;
		return 0;
	}
	case KEY_TYPE_reflink_p:
		return bch2_trans_mark_reflink_p(trans,
					bkey_s_c_to_reflink_p(k),
					offset, sectors, flags);
	default:
		return 0;
	}
}

int bch2_trans_mark_update(struct btree_trans *trans,
			   struct btree_iter *iter,
			   struct bkey_i *new,
			   unsigned flags)
{
	struct bkey_s_c	old;
	int ret;

	if (unlikely(flags & BTREE_TRIGGER_NORUN))
		return 0;

	if (!btree_node_type_needs_gc(iter->btree_id))
		return 0;

	if (!btree_node_type_is_extents(iter->btree_id)) {
		/* iterators should be uptodate, shouldn't get errors here: */
		if (btree_iter_type(iter) != BTREE_ITER_CACHED) {
			old = bch2_btree_iter_peek_slot(iter);
			BUG_ON(bkey_err(old));
		} else {
			struct bkey_cached *ck = (void *) iter->l[0].b;

			BUG_ON(!ck->valid);
			old = bkey_i_to_s_c(ck->k);
		}

		if (old.k->type == new->k.type) {
			ret   = bch2_trans_mark_key(trans, old, bkey_i_to_s_c(new), 0, 0,
					BTREE_TRIGGER_INSERT|BTREE_TRIGGER_OVERWRITE|flags);
		} else {
			ret   = bch2_trans_mark_key(trans, old, bkey_i_to_s_c(new), 0, 0,
					BTREE_TRIGGER_INSERT|flags) ?:
				bch2_trans_mark_key(trans, old, bkey_i_to_s_c(new), 0, 0,
					BTREE_TRIGGER_OVERWRITE|flags);
		}
	} else {
		struct btree		*b = iter_l(iter)->b;
		struct btree_node_iter	node_iter = iter_l(iter)->iter;
		struct bkey_packed	*_old;
		struct bkey		unpacked;

		EBUG_ON(btree_iter_type(iter) == BTREE_ITER_CACHED);

		bkey_init(&unpacked);
		old = (struct bkey_s_c) { &unpacked, NULL };

		ret = bch2_trans_mark_key(trans, old, bkey_i_to_s_c(new),
					  0, new->k.size,
					  BTREE_TRIGGER_INSERT);
		if (ret)
			return ret;

		while ((_old = bch2_btree_node_iter_peek(&node_iter, b))) {
			unsigned flags = BTREE_TRIGGER_OVERWRITE;
			unsigned offset = 0;
			s64 sectors;

			old = bkey_disassemble(b, _old, &unpacked);
			sectors = -((s64) old.k->size);

			flags |= BTREE_TRIGGER_OVERWRITE;

			if (bkey_cmp(new->k.p, bkey_start_pos(old.k)) <= 0)
				return 0;

			switch (bch2_extent_overlap(&new->k, old.k)) {
			case BCH_EXTENT_OVERLAP_ALL:
				offset = 0;
				sectors = -((s64) old.k->size);
				break;
			case BCH_EXTENT_OVERLAP_BACK:
				offset = bkey_start_offset(&new->k) -
					bkey_start_offset(old.k);
				sectors = bkey_start_offset(&new->k) -
					old.k->p.offset;
				break;
			case BCH_EXTENT_OVERLAP_FRONT:
				offset = 0;
				sectors = bkey_start_offset(old.k) -
					new->k.p.offset;
				break;
			case BCH_EXTENT_OVERLAP_MIDDLE:
				offset = bkey_start_offset(&new->k) -
					bkey_start_offset(old.k);
				sectors = -((s64) new->k.size);
				flags |= BTREE_TRIGGER_OVERWRITE_SPLIT;
				break;
			}

			BUG_ON(sectors >= 0);

			ret = bch2_trans_mark_key(trans, old, bkey_i_to_s_c(new),
					offset, sectors, flags);
			if (ret)
				return ret;

			bch2_btree_node_iter_advance(&node_iter, b);
		}
	}

	return ret;
}

static int __bch2_trans_mark_metadata_bucket(struct btree_trans *trans,
				    struct bch_dev *ca, size_t b,
				    enum bch_data_type type,
				    unsigned sectors)
{
	struct bch_fs *c = trans->c;
	struct btree_iter *iter;
	struct bkey_alloc_unpacked u;
	struct bkey_alloc_buf *a;
	struct bch_extent_ptr ptr = {
		.dev = ca->dev_idx,
		.offset = bucket_to_sector(ca, b),
	};
	int ret = 0;

	a = bch2_trans_start_alloc_update(trans, &iter, &ptr, &u);
	if (IS_ERR(a))
		return PTR_ERR(a);

	if (u.data_type && u.data_type != type) {
		bch2_fsck_err(c, FSCK_CAN_IGNORE|FSCK_NEED_FSCK,
			"bucket %llu:%llu gen %u different types of data in same bucket: %s, %s\n"
			"while marking %s",
			iter->pos.inode, iter->pos.offset, u.gen,
			bch2_data_types[u.data_type],
			bch2_data_types[type],
			bch2_data_types[type]);
		ret = -EIO;
		goto out;
	}

	if ((unsigned) (u.dirty_sectors + sectors) > ca->mi.bucket_size) {
		bch2_fsck_err(c, FSCK_CAN_IGNORE|FSCK_NEED_FSCK,
			"bucket %llu:%llu gen %u data type %s sector count overflow: %u + %u > %u\n"
			"while marking %s",
			iter->pos.inode, iter->pos.offset, u.gen,
			bch2_data_types[u.data_type ?: type],
			u.dirty_sectors, sectors, ca->mi.bucket_size,
			bch2_data_types[type]);
		ret = -EIO;
		goto out;
	}

	if (u.data_type		== type &&
	    u.dirty_sectors	== sectors)
		goto out;

	u.data_type	= type;
	u.dirty_sectors	= sectors;

	bch2_alloc_pack(c, a, u);
	bch2_trans_update(trans, iter, &a->k, 0);
out:
	bch2_trans_iter_put(trans, iter);
	return ret;
}

int bch2_trans_mark_metadata_bucket(struct btree_trans *trans,
				    struct disk_reservation *res,
				    struct bch_dev *ca, size_t b,
				    enum bch_data_type type,
				    unsigned sectors)
{
	return __bch2_trans_do(trans, res, NULL, 0,
			__bch2_trans_mark_metadata_bucket(trans, ca, b, BCH_DATA_journal,
							ca->mi.bucket_size));

}

static int bch2_trans_mark_metadata_sectors(struct btree_trans *trans,
					    struct disk_reservation *res,
					    struct bch_dev *ca,
					    u64 start, u64 end,
					    enum bch_data_type type,
					    u64 *bucket, unsigned *bucket_sectors)
{
	int ret;

	do {
		u64 b = sector_to_bucket(ca, start);
		unsigned sectors =
			min_t(u64, bucket_to_sector(ca, b + 1), end) - start;

		if (b != *bucket) {
			if (*bucket_sectors) {
				ret = bch2_trans_mark_metadata_bucket(trans, res, ca,
						*bucket, type, *bucket_sectors);
				if (ret)
					return ret;
			}

			*bucket		= b;
			*bucket_sectors	= 0;
		}

		*bucket_sectors	+= sectors;
		start += sectors;
	} while (!ret && start < end);

	return 0;
}

static int __bch2_trans_mark_dev_sb(struct btree_trans *trans,
			     struct disk_reservation *res,
			     struct bch_dev *ca)
{
	struct bch_sb_layout *layout = &ca->disk_sb.sb->layout;
	u64 bucket = 0;
	unsigned i, bucket_sectors = 0;
	int ret;

	for (i = 0; i < layout->nr_superblocks; i++) {
		u64 offset = le64_to_cpu(layout->sb_offset[i]);

		if (offset == BCH_SB_SECTOR) {
			ret = bch2_trans_mark_metadata_sectors(trans, res, ca,
						0, BCH_SB_SECTOR,
						BCH_DATA_sb, &bucket, &bucket_sectors);
			if (ret)
				return ret;
		}

		ret = bch2_trans_mark_metadata_sectors(trans, res, ca, offset,
				      offset + (1 << layout->sb_max_size_bits),
				      BCH_DATA_sb, &bucket, &bucket_sectors);
		if (ret)
			return ret;
	}

	if (bucket_sectors) {
		ret = bch2_trans_mark_metadata_bucket(trans, res, ca,
				bucket, BCH_DATA_sb, bucket_sectors);
		if (ret)
			return ret;
	}

	for (i = 0; i < ca->journal.nr; i++) {
		ret = bch2_trans_mark_metadata_bucket(trans, res, ca,
				ca->journal.buckets[i],
				BCH_DATA_journal, ca->mi.bucket_size);
		if (ret)
			return ret;
	}

	return 0;
}

int bch2_trans_mark_dev_sb(struct bch_fs *c,
			   struct disk_reservation *res,
			   struct bch_dev *ca)
{
	return bch2_trans_do(c, res, NULL, 0,
			__bch2_trans_mark_dev_sb(&trans, res, ca));
}

/* Disk reservations: */

void __bch2_disk_reservation_put(struct bch_fs *c, struct disk_reservation *res)
{
	percpu_down_read(&c->mark_lock);
	this_cpu_sub(c->usage[0]->online_reserved,
		     res->sectors);
	percpu_up_read(&c->mark_lock);

	res->sectors = 0;
}

#define SECTORS_CACHE	1024

int bch2_disk_reservation_add(struct bch_fs *c, struct disk_reservation *res,
			      u64 sectors, int flags)
{
	struct bch_fs_pcpu *pcpu;
	u64 old, v, get;
	s64 sectors_available;
	int ret;

	percpu_down_read(&c->mark_lock);
	preempt_disable();
	pcpu = this_cpu_ptr(c->pcpu);

	if (sectors <= pcpu->sectors_available)
		goto out;

	v = atomic64_read(&c->sectors_available);
	do {
		old = v;
		get = min((u64) sectors + SECTORS_CACHE, old);

		if (get < sectors) {
			preempt_enable();
			goto recalculate;
		}
	} while ((v = atomic64_cmpxchg(&c->sectors_available,
				       old, old - get)) != old);

	pcpu->sectors_available		+= get;

out:
	pcpu->sectors_available		-= sectors;
	this_cpu_add(c->usage[0]->online_reserved, sectors);
	res->sectors			+= sectors;

	preempt_enable();
	percpu_up_read(&c->mark_lock);
	return 0;

recalculate:
	mutex_lock(&c->sectors_available_lock);

	percpu_u64_set(&c->pcpu->sectors_available, 0);
	sectors_available = avail_factor(__bch2_fs_usage_read_short(c).free);

	if (sectors <= sectors_available ||
	    (flags & BCH_DISK_RESERVATION_NOFAIL)) {
		atomic64_set(&c->sectors_available,
			     max_t(s64, 0, sectors_available - sectors));
		this_cpu_add(c->usage[0]->online_reserved, sectors);
		res->sectors			+= sectors;
		ret = 0;
	} else {
		atomic64_set(&c->sectors_available, sectors_available);
		ret = -ENOSPC;
	}

	mutex_unlock(&c->sectors_available_lock);
	percpu_up_read(&c->mark_lock);

	return ret;
}

/* Startup/shutdown: */

static void buckets_free_rcu(struct rcu_head *rcu)
{
	struct bucket_array *buckets =
		container_of(rcu, struct bucket_array, rcu);

	kvpfree(buckets,
		sizeof(struct bucket_array) +
		buckets->nbuckets * sizeof(struct bucket));
}

int bch2_dev_buckets_resize(struct bch_fs *c, struct bch_dev *ca, u64 nbuckets)
{
	struct bucket_array *buckets = NULL, *old_buckets = NULL;
	unsigned long *buckets_nouse = NULL;
	alloc_fifo	free[RESERVE_NR];
	alloc_fifo	free_inc;
	alloc_heap	alloc_heap;

	size_t btree_reserve	= DIV_ROUND_UP(BTREE_NODE_RESERVE,
			     ca->mi.bucket_size / c->opts.btree_node_size);
	/* XXX: these should be tunable */
	size_t reserve_none	= max_t(size_t, 1, nbuckets >> 9);
	size_t copygc_reserve	= max_t(size_t, 2, nbuckets >> 6);
	size_t free_inc_nr	= max(max_t(size_t, 1, nbuckets >> 12),
				      btree_reserve * 2);
	bool resize = ca->buckets[0] != NULL;
	int ret = -ENOMEM;
	unsigned i;

	memset(&free,		0, sizeof(free));
	memset(&free_inc,	0, sizeof(free_inc));
	memset(&alloc_heap,	0, sizeof(alloc_heap));

	if (!(buckets		= kvpmalloc(sizeof(struct bucket_array) +
					    nbuckets * sizeof(struct bucket),
					    GFP_KERNEL|__GFP_ZERO)) ||
	    !(buckets_nouse	= kvpmalloc(BITS_TO_LONGS(nbuckets) *
					    sizeof(unsigned long),
					    GFP_KERNEL|__GFP_ZERO)) ||
	    !init_fifo(&free[RESERVE_MOVINGGC],
		       copygc_reserve, GFP_KERNEL) ||
	    !init_fifo(&free[RESERVE_NONE], reserve_none, GFP_KERNEL) ||
	    !init_fifo(&free_inc,	free_inc_nr, GFP_KERNEL) ||
	    !init_heap(&alloc_heap,	ALLOC_SCAN_BATCH(ca) << 1, GFP_KERNEL))
		goto err;

	buckets->first_bucket	= ca->mi.first_bucket;
	buckets->nbuckets	= nbuckets;

	bch2_copygc_stop(c);

	if (resize) {
		down_write(&c->gc_lock);
		down_write(&ca->bucket_lock);
		percpu_down_write(&c->mark_lock);
	}

	old_buckets = bucket_array(ca);

	if (resize) {
		size_t n = min(buckets->nbuckets, old_buckets->nbuckets);

		memcpy(buckets->b,
		       old_buckets->b,
		       n * sizeof(struct bucket));
		memcpy(buckets_nouse,
		       ca->buckets_nouse,
		       BITS_TO_LONGS(n) * sizeof(unsigned long));
	}

	rcu_assign_pointer(ca->buckets[0], buckets);
	buckets = old_buckets;

	swap(ca->buckets_nouse, buckets_nouse);

	if (resize) {
		percpu_up_write(&c->mark_lock);
		up_write(&c->gc_lock);
	}

	spin_lock(&c->freelist_lock);
	for (i = 0; i < RESERVE_NR; i++) {
		fifo_move(&free[i], &ca->free[i]);
		swap(ca->free[i], free[i]);
	}
	fifo_move(&free_inc, &ca->free_inc);
	swap(ca->free_inc, free_inc);
	spin_unlock(&c->freelist_lock);

	/* with gc lock held, alloc_heap can't be in use: */
	swap(ca->alloc_heap, alloc_heap);

	nbuckets = ca->mi.nbuckets;

	if (resize)
		up_write(&ca->bucket_lock);

	ret = 0;
err:
	free_heap(&alloc_heap);
	free_fifo(&free_inc);
	for (i = 0; i < RESERVE_NR; i++)
		free_fifo(&free[i]);
	kvpfree(buckets_nouse,
		BITS_TO_LONGS(nbuckets) * sizeof(unsigned long));
	if (buckets)
		call_rcu(&old_buckets->rcu, buckets_free_rcu);

	return ret;
}

void bch2_dev_buckets_free(struct bch_dev *ca)
{
	unsigned i;

	free_heap(&ca->alloc_heap);
	free_fifo(&ca->free_inc);
	for (i = 0; i < RESERVE_NR; i++)
		free_fifo(&ca->free[i]);
	kvpfree(ca->buckets_nouse,
		BITS_TO_LONGS(ca->mi.nbuckets) * sizeof(unsigned long));
	kvpfree(rcu_dereference_protected(ca->buckets[0], 1),
		sizeof(struct bucket_array) +
		ca->mi.nbuckets * sizeof(struct bucket));

	for (i = 0; i < ARRAY_SIZE(ca->usage); i++)
		free_percpu(ca->usage[i]);
	kfree(ca->usage_base);
}

int bch2_dev_buckets_alloc(struct bch_fs *c, struct bch_dev *ca)
{
	unsigned i;

	ca->usage_base = kzalloc(sizeof(struct bch_dev_usage), GFP_KERNEL);
	if (!ca->usage_base)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(ca->usage); i++) {
		ca->usage[i] = alloc_percpu(struct bch_dev_usage);
		if (!ca->usage[i])
			return -ENOMEM;
	}

	return bch2_dev_buckets_resize(c, ca, ca->mi.nbuckets);;
}
