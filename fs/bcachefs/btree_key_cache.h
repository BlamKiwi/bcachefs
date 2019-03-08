#ifndef _BCACHEFS_BTREE_KEY_CACHE_H
#define _BCACHEFS_BTREE_KEY_CACHE_H

struct bkey_cached *
bch2_btree_key_cache_get(struct btree_iter *, enum six_lock_type, bool);

void bch2_btree_insert_key_cached(struct btree_trans *,
				  struct btree_insert_entry *);
int bch2_btree_key_cache_flush(struct btree_trans *,
			       enum btree_id, struct bpos);

void bch2_fs_btree_key_cache_exit(struct btree_key_cache *);
void bch2_fs_btree_key_cache_init_early(struct btree_key_cache *);
int bch2_fs_btree_key_cache_init(struct btree_key_cache *);

#endif /* _BCACHEFS_BTREE_KEY_CACHE_H */
