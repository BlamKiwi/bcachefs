/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_DELAYED_H
#define _BCACHEFS_DELAYED_H

#include "btree_types.h"
#include "delayed_types.h"

/*
 * This is not a cache.
 *
 * DELAYED WRITES:
 *
 * BcacheFS uses delayed write consistency. BcacheFS will not guarantee
 * on-disk resiliency (commit) writes until it is explicitly asked to. This allows
 * BCacheFS as much time as possible to try assemble complete stripes or replica sets.
 * This can improve performance in streaming/multimedia oriented workloads, and
 * can reduce total write amplification.
 *
 * BcacheFS will always replicate metadata eagerly.
 *
 * DESIGN TRADEOFFS:
 *
 * There are some issues that are introduced by not eagerly commiting writes to disk:
 * - Buffers handed to us by userspace/kernel may not live long enough. Data will be commited "at some point in the future".
 * - We have to track and traverse more intermediate state, effectively introducing a Resiliency journal.
 *
 * This means we have to bounce all writes (instead of bouncing some writes).
 * - Data buffer lifetime is now defined by barrier operations.
 *
 * Additionally we should write out data eagerly, and only delay the resiliency data.
 * - Can guarantee POSIX compliance wrt. read-after-write ordering.
 * - BcacheFS deployments without resiliency don't need to have their commits delayed.
 * - This preserves the behaviour of encryption, compression, copygc etc.
 * - If the system crashes, we can probably recover the original data.
 *
 * WRITE BARRIERS:
 *
 * bch_delayed_flush_* tells BcacheFS to commit previous writes BcacheFS
 * has responded to. This will typically cause BcacheFS to write out
 * parity blocks or replica sets.
 *
 * Given a set of ops
 * START -> W0 -> W1 -> END
 * Commit all ops at some point in the future
 *
 * START -> W0 -> W1 -> SYNC -> END
 * Commit all ops
 *
 * Given a set of ops
 * START -> W0 -> W1 -> SYNC0 -> W2 -> END
 * Commit ops 0-1 and commit op 2 at some point in the future
 */

int bch2_delayed_flush(struct journal *journal, u64 seq);
int bch2_delayed_flush_async(struct journal *journal, u64 seq, struct closure *parent);

void bch2_delayed_write(struct closure *parent);

void bch2_fs_delayed_exit(struct bch_fs *c);
int bch2_fs_delayed_init(struct bch_fs *c);

#endif /* _BCACHEFS_DELAYED_H */
