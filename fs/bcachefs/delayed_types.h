/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _BCACHEFS_DELAYED_TYPES_IO_H
#define _BCACHEFS_DELAYED_TYPES_IO_H

#include <linux/closure.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>

struct bch2_barrier_op {
    struct closure *parent;
    struct list_head node;
    int status;
    u64 seq;
};

struct bch2_barrier_group_node {
    struct list_head node;
    u64 seq;
};

struct bch2_barrier_group {
    struct list_head head;
    u64 seq;
};

struct bch2_delayed_controller {
    struct bch2_barrier_group dirty_writes;
    struct bch2_barrier_group pending_ops;

    spinlock_t lock;
};

#endif