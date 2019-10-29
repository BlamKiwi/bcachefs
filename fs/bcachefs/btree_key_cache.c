
#include "bcachefs.h"
#include "btree_iter.h"
#include "btree_key_cache.h"
#include "btree_locking.h"
#include "btree_update.h"
#include "error.h"
#include "journal.h"
#include "journal_reclaim.h"

static const struct htable_params bch_btree_key_cache_params = {
	.head_offset	= offsetof(struct bkey_cached, hash),
	.key_offset	= offsetof(struct bkey_cached, key),
	.key_len	= sizeof(struct bkey_cached_key),
};

__flatten
static inline struct bkey_cached *
btree_key_cache_find(struct bch_fs *c, enum btree_id btree_id, struct bpos pos)
{
	struct bkey_cached_key key = {
		.btree_id	= btree_id,
		.pos		= pos,
	};

	return htable_lookup(&c->btree_key_cache.table, &key,
			     bch_btree_key_cache_params);
}

static bool bkey_cached_lock_for_evict(struct bkey_cached *ck)
{
	if (!six_trylock_intent(&ck->lock))
		return false;

	if (!six_trylock_write(&ck->lock)) {
		six_unlock_intent(&ck->lock);
		return false;
	}

	if (test_bit(BKEY_CACHED_DIRTY, &ck->flags)) {
		six_unlock_write(&ck->lock);
		six_unlock_intent(&ck->lock);
		return false;
	}

	return true;
}

static void bkey_cached_evict(struct btree_key_cache *c,
			      struct bkey_cached *ck)
{
	htable_remove(&c->table, ck, bch_btree_key_cache_params);
	memset(&ck->key, ~0, sizeof(ck->key));
}

static void bkey_cached_free(struct btree_key_cache *c,
			     struct bkey_cached *ck)
{
	list_move(&ck->list, &c->freed);

	kfree(ck->k);
	ck->k		= NULL;
	ck->u64s	= 0;

	six_unlock_write(&ck->lock);
	six_unlock_intent(&ck->lock);
}

static struct bkey_cached *
bkey_cached_alloc(struct btree_key_cache *c, unsigned u64s)
{
	struct bkey_cached *ck;

	u64s = roundup_pow_of_two(u64s);

	list_for_each_entry(ck, &c->freed, list)
		if (bkey_cached_lock_for_evict(ck))
			goto found;

	list_for_each_entry(ck, &c->clean, list)
		if (bkey_cached_lock_for_evict(ck)) {
			bkey_cached_evict(c, ck);
			goto found;
		}

	ck = kzalloc(sizeof(*ck), GFP_NOFS);
	if (!ck)
		return NULL;

	INIT_LIST_HEAD(&ck->list);
	six_lock_init(&ck->lock);
	BUG_ON(!six_trylock_intent(&ck->lock));
	BUG_ON(!six_trylock_write(&ck->lock));
found:
	if (ck->u64s < u64s) {
		kfree(ck->k);
		ck->k = kmalloc(u64s * sizeof(u64), GFP_NOFS);
		ck->u64s = u64s;
	}

	if (!ck->k) {
		list_move(&ck->list, &c->freed);
		six_unlock_write(&ck->lock);
		six_unlock_intent(&ck->lock);
		return NULL;
	}

	return ck;
}

static struct bkey_cached *
__btree_key_cache_fill(struct btree_key_cache *c,
		       enum btree_id btree_id,
		       struct bkey_s_c k,
		       enum six_lock_type lock_type)
{
	struct bkey_cached *ck;

	ck = bkey_cached_alloc(c, k.k->u64s);
	if (!ck)
		return ERR_PTR(-ENOMEM);

	ck->key.btree_id	= btree_id;
	ck->key.pos		= k.k->p;
	bkey_reassemble(ck->k, k);

	if (htable_insert(&c->table, ck, bch_btree_key_cache_params)) {
		/* We raced with another fill: */
		bkey_cached_free(c, ck);
		return NULL;
	}

	list_move(&ck->list, &c->clean);
	six_unlock_write(&ck->lock);
	if (lock_type == SIX_LOCK_read)
		six_lock_downgrade(&ck->lock);

	return ck;
}

static struct bkey_cached *
btree_key_cache_fill(struct btree_trans *trans,
		     enum btree_id btree_id,
		     struct bpos pos,
		     enum six_lock_type lock_type)
{
	struct bch_fs *c = trans->c;
	struct bkey_cached *ck;
	struct btree_iter *iter;
	struct bkey_s_c k;
	int ret;

	iter = bch2_trans_get_iter(trans, btree_id, pos, BTREE_ITER_SLOTS);
	if (IS_ERR(iter))
		return ERR_CAST(iter);

	k = bch2_btree_iter_peek_slot(iter);
	ret = bkey_err(k);
	if (ret) {
		bch2_trans_iter_put(trans, iter);
		return ERR_PTR(ret);
	}

	mutex_lock(&c->btree_key_cache.lock);
	ck = __btree_key_cache_fill(&c->btree_key_cache,
				    btree_id, k, lock_type);
	mutex_unlock(&c->btree_key_cache.lock);

	/* We're not likely to need this iterator again: */
	bch2_trans_iter_free(trans, iter);

	return ck;
}

struct bkey_cached *
bch2_btree_key_cache_get(struct btree_iter *iter,
			 enum six_lock_type lock_type,
			 bool fill)
{
	struct bch_fs *c = iter->trans->c;
	struct bkey_cached *ck;
retry:
	ck = btree_key_cache_find(c, iter->btree_id, iter->pos);
	if (!ck) {
		if (!fill)
			return NULL;

		ck = btree_key_cache_fill(iter->trans,
					  iter->btree_id,
					  iter->pos, lock_type);
		if (!ck)
			goto retry;

		if (IS_ERR(ck))
			return ck;
	} else {
		if (!btree_node_lock((void *) ck, iter->pos, 0,
				     iter, lock_type))
			return ERR_PTR(-EINTR);

		if (ck->key.btree_id != iter->btree_id ||
		    bkey_cmp(ck->key.pos, iter->pos)) {
			six_unlock_type(&ck->lock, lock_type);
			goto retry;
		}
	}

	return ck;
}

static int btree_key_cache_flush_trans(struct btree_trans *trans,
				       struct bkey_cached *ck)
{
	struct bch_fs *c = trans->c;
	struct journal *j = &c->journal;
	struct btree_iter *iter;
	int ret;

	if (!test_bit(BKEY_CACHED_DIRTY, &ck->flags))
		return 0;

	BUG_ON(jset_u64s(ck->k->k.u64s) > ck->res.u64s);

	iter = bch2_trans_get_iter(trans, ck->key.btree_id, ck->key.pos,
				   BTREE_ITER_SLOTS|BTREE_ITER_INTENT);
	if (IS_ERR(iter))
		return PTR_ERR(iter);

	bch2_trans_update(trans, iter, ck->k);

	ret = bch2_trans_commit(trans, NULL, NULL,
				BTREE_INSERT_ATOMIC|
				BTREE_INSERT_NOUNLOCK|
				BTREE_INSERT_NOCHECK_RW|
				BTREE_INSERT_NOMARK|
				BTREE_INSERT_NOFAIL|
				BTREE_INSERT_USE_RESERVE|
				BTREE_INSERT_JOURNAL_RESERVED);
	if (ret == -EINTR) {
		bch2_trans_iter_put(trans, iter);
		return ret;
	}

	bch2_fs_fatal_err_on(ret && !bch2_journal_error(j), c,
			     "error flushing cached btree update: %i", ret);

	mutex_lock(&c->btree_key_cache.lock);
	clear_bit(BKEY_CACHED_DIRTY, &ck->flags);
	list_move_tail(&ck->list, &c->btree_key_cache.clean);
	mutex_unlock(&c->btree_key_cache.lock);

	bch2_journal_pin_drop(j, &ck->journal);
	bch2_journal_preres_put(j, &ck->res);

	bch2_trans_iter_free(trans, iter);
	return 0;
}

static void __btree_key_cache_flush(struct bch_fs *c,
				    struct bkey_cached *ck)
{
	struct journal *j = &c->journal;
	int ret;

	if (!test_and_clear_bit(BKEY_CACHED_DIRTY, &ck->flags))
		return;

	BUG_ON(jset_u64s(ck->k->k.u64s) > ck->res.u64s);

	ret = bch2_btree_insert(c, ck->key.btree_id, ck->k, NULL, NULL,
				BTREE_INSERT_NOCHECK_RW|
				BTREE_INSERT_NOMARK|
				BTREE_INSERT_NOFAIL|
				BTREE_INSERT_USE_RESERVE|
				BTREE_INSERT_JOURNAL_RESERVED);

	bch2_fs_fatal_err_on(ret && !bch2_journal_error(j), c,
			     "error flushing cached btree update: %i", ret);

	mutex_lock(&c->btree_key_cache.lock);
	list_move_tail(&ck->list, &c->btree_key_cache.clean);
	mutex_unlock(&c->btree_key_cache.lock);

	bch2_journal_pin_drop(j, &ck->journal);
	bch2_journal_preres_put(j, &ck->res);
}

static void btree_key_cache_journal_flush(struct journal *j,
					  struct journal_entry_pin *pin,
					  u64 seq)
{
	struct bch_fs *c = container_of(j, struct bch_fs, journal);
	struct bkey_cached *ck =
		container_of(pin, struct bkey_cached, journal);

	six_lock_intent(&ck->lock);
	if (ck->journal.seq == seq)
		__btree_key_cache_flush(c, ck);
	six_unlock_intent(&ck->lock);
}

void bch2_btree_insert_key_cached(struct btree_trans *trans,
				  struct btree_insert_entry *insert)
{
	struct bch_fs *c = trans->c;
	struct bkey_cached *ck = (void *) insert->iter->l[0].b;
	int difference;

	BUG_ON(trans->flags & BTREE_INSERT_JOURNAL_REPLAY);
	BUG_ON(insert->k->u64s > ck->u64s);
	BUG_ON(jset_u64s(insert->k->u64s) > trans->journal_preres.u64s);

	difference = jset_u64s(insert->k->u64s) - ck->res.u64s;
	if (difference > 0) {
		trans->journal_preres.u64s	-= difference;
		ck->res.u64s			+= difference;
	}

	bkey_copy(ck->k, insert->k);
	if (!test_bit(BKEY_CACHED_DIRTY, &ck->flags)) {
		mutex_lock(&c->btree_key_cache.lock);
		list_del_init(&ck->list);

		set_bit(BKEY_CACHED_DIRTY, &ck->flags);
		mutex_unlock(&c->btree_key_cache.lock);
	}

	bch2_journal_add_keys(&c->journal, &trans->journal_res,
			      ck->key.btree_id, ck->k);
	bch2_journal_set_has_inode(&c->journal, &trans->journal_res,
				   ck->k->k.p.inode);
	if (trans->journal_seq)
		*trans->journal_seq = trans->journal_res.seq;

	bch2_journal_pin_update(&c->journal, trans->journal_res.seq,
				&ck->journal, btree_key_cache_journal_flush);
}

int bch2_btree_key_cache_flush(struct btree_trans *trans,
			       enum btree_id id, struct bpos pos)
{
	struct bch_fs *c = trans->c;
	struct btree_iter *iter;
	struct bkey_cached *ck;
	int ret;

	iter = bch2_trans_get_iter(trans, id, pos,
			BTREE_ITER_CACHED|
			BTREE_ITER_CACHED_NOFILL|
			BTREE_ITER_INTENT);
	ret = PTR_ERR_OR_ZERO(iter);
	if (ret)
		goto err;

	ret = bch2_btree_iter_traverse(iter);
	if (ret)
		goto err;

	ck = (void *) iter->l[0].b;
	if (!ck)
		goto out;

	ret = btree_key_cache_flush_trans(trans, ck);
	if (ret)
		goto err;

	BUG_ON(!btree_node_intent_locked(iter, 0));

	mark_btree_node_unlocked(iter, 0);
	iter->l[0].b = NULL;

	six_lock_write(&ck->lock);

	mutex_lock(&c->btree_key_cache.lock);
	bkey_cached_evict(&c->btree_key_cache, ck);
	bkey_cached_free(&c->btree_key_cache, ck);
	mutex_unlock(&c->btree_key_cache.lock);
out:
	bch2_trans_iter_free(trans, iter);
	return 0;
err:
	bch2_trans_iter_put(trans, iter);
	return ret;
}

void bch2_fs_btree_key_cache_exit(struct btree_key_cache *c)
{
	struct bkey_cached *ck, *n;

	mutex_lock(&c->lock);
	list_for_each_entry_safe(ck, n, &c->clean, list) {
		kfree(ck->k);
		kfree(ck);
	}
	list_for_each_entry_safe(ck, n, &c->freed, list)
		kfree(ck);
	mutex_unlock(&c->lock);

	bch2_htable_exit(&c->table);
}

void bch2_fs_btree_key_cache_init_early(struct btree_key_cache *c)
{
	mutex_init(&c->lock);
	INIT_LIST_HEAD(&c->freed);
	INIT_LIST_HEAD(&c->clean);
}

int bch2_fs_btree_key_cache_init(struct btree_key_cache *c)
{
	return bch2_htable_init(&c->table);
}
