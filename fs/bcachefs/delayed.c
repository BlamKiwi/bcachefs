/* SPDX-License-Identifier: GPL-2.0 */
#include "delayed.h"
#include "journal.h"
#include "io.h"

int bch2_delayed_flush(struct journal *journal, u64 seq)
{
    return bch2_journal_flush_seq(journal, seq);
}

int bch2_delayed_flush_async(struct journal *journal, u64 seq, struct closure *parent)
{
    return bch2_journal_flush_seq_async(journal, seq, parent);
}

void bch2_delayed_write(struct closure *cl)
{
    bch2_write(cl);
}

static void barrier_group_init(struct bch2_barrier_group *g)
{
    INIT_LIST_HEAD(&g->head);
    g->seq = 0;
}

int bch2_fs_delayed_init(struct bch_fs *c)
{
    struct bch2_delayed_controller *delayed_cntl;
    
    delayed_cntl = &c->delayed_cntl;

    barrier_group_init(&delayed_cntl->dirty_writes);
    barrier_group_init(&delayed_cntl->pending_ops);

    spin_lock_init(&delayed_cntl->lock);

    return 0;
}

void bch2_fs_delayed_exit(struct bch_fs *c)
{
}
