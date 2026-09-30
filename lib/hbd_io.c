// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/bio.h>
#include <linux/blk-mq.h>
#include <linux/blk_types.h>
#include <linux/blkdev.h>
#include <linux/printk.h>
#include <linux/version.h>

#include "hk.h"
#include "hk_inline.h"
#include "hbd.h"
#include "hbd_shadow.h"

void __nocfi hbd_bio_wrap(struct bio *bio);
blk_status_t __nocfi hbd_sg_wrap(struct request *rq, bool at_head);

typedef void (*hbd_bio_orig_fn)(struct bio *bio);
typedef blk_status_t (*hbd_sg_orig_fn)(struct request *rq, bool at_head);

static struct hk_inline g_bio_inline;
static struct hk_inline g_sg_inline;
static hbd_bio_orig_fn g_bio_orig;
static hbd_sg_orig_fn g_sg_orig;

static void hbd_handle_read(struct hbd_target *t, struct bio *bio)
{
	u64 sect = bio->bi_iter.bi_sector;
	int ret = 0;

	if (t->on_read) {
		ret = t->on_read(t, bio, sect);
		if (ret > 0)
			return;
		if (ret < 0) {
			bio->bi_status = BLK_STS_IOERR;
			bio_endio(bio);
			return;
		}
	}
	if (t->flags & HBD_WR_SHADOW) {
		ret = hbd_shadow_read(t->shadow, bio, sect,
				      !(t->flags & HBD_RD_BLOCK));
		if (!ret) {
			bio->bi_status = BLK_STS_OK;
			bio_endio(bio);
			return;
		}
	}
	if (t->flags & HBD_RD_ZERO) {
		hbd_bio_zero(bio);
		bio->bi_status = BLK_STS_OK;
		bio_endio(bio);
		return;
	}
	bio->bi_status = BLK_STS_IOERR;
	bio_endio(bio);
}

static void hbd_handle_write(struct hbd_target *t, struct bio *bio)
{
	int op = bio_op(bio);
	int ret;

	if (t->flags & HBD_WR_SHADOW) {
		switch (op) {
		case REQ_OP_WRITE:
			ret = hbd_shadow_write(t->shadow, bio,
					       bio->bi_iter.bi_sector);
			if (ret) {
				bio->bi_status = BLK_STS_IOERR;
				bio_endio(bio);
				return;
			}
			break;
		case REQ_OP_WRITE_ZEROES:
		case REQ_OP_DISCARD:
		case REQ_OP_SECURE_ERASE:
			hbd_shadow_zero(t->shadow, bio->bi_iter.bi_sector,
					bio->bi_iter.bi_size);
			break;
		default:
			break;
		}
	}
	bio->bi_status = BLK_STS_OK;
	bio_endio(bio);
}

static bool hbd_read_protected(const struct hbd_target *t)
{
	return (t->flags & (HBD_RD_BLOCK | HBD_RD_ZERO | HBD_WR_SHADOW)) ||
	       t->on_read;
}

static dev_t hbd_bio_dev(struct bio *bio)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
	return bio->bi_bdev->bd_dev;
#else
	return MKDEV(bio->bi_disk->major,
		     bio->bi_disk->first_minor + bio->bi_partno);
#endif
}

void __nocfi hbd_bio_wrap(struct bio *bio)
{
	struct hbd_target *t;
	int op;

	if (!bio)
		goto orig;
	t = hbd_lookup_dev(hbd_bio_dev(bio));
	if (!t)
		goto orig;

	op = bio_op(bio);
	switch (op) {
	case REQ_OP_READ:
		if (!hbd_read_protected(t))
			break;
		hbd_handle_read(t, bio);
		hbd_put_target(t);
		return;
	case REQ_OP_WRITE:
	case REQ_OP_WRITE_ZEROES:
	case REQ_OP_DISCARD:
	case REQ_OP_SECURE_ERASE:
		if (!(t->flags & (HBD_WR_SINK | HBD_WR_SHADOW)))
			break;
		hbd_handle_write(t, bio);
		hbd_put_target(t);
		return;
	default:
		break;
	}
	hbd_put_target(t);
orig:
	g_bio_orig(bio);
}

blk_status_t __nocfi hbd_sg_wrap(struct request *rq, bool at_head)
{
	struct hbd_target *t;
	blk_status_t ret;

	if (!rq || !rq->q)
		goto orig;
	t = hbd_lookup_queue(rq->q);
	if (!t)
		goto orig;
	if (!(t->flags & (HBD_RD_BLOCK | HBD_RD_ZERO | HBD_WR_SINK |
			  HBD_WR_SHADOW))) {
		hbd_put_target(t);
		goto orig;
	}
	hbd_put_target(t);
	return BLK_STS_IOERR;

orig:
	return g_sg_orig(rq, at_head);
}

int hbd_io_init(void)
{
	int ret;

	ret = hk_inline_hook(&g_bio_inline, "submit_bio_noacct",
			     "hbd_bio_wrap");
	if (ret)
		return ret;
	g_bio_orig = (hbd_bio_orig_fn)g_bio_inline.orig;

	ret = hk_inline_hook(&g_sg_inline, "blk_execute_rq",
			     "hbd_sg_wrap");
	if (ret) {
		hk_inline_unhook(&g_bio_inline);
		return ret;
	}
	g_sg_orig = (hbd_sg_orig_fn)g_sg_inline.orig;
	return 0;
}

void hbd_io_exit(void)
{
	hk_inline_unhook(&g_sg_inline);
	hk_inline_unhook(&g_bio_inline);
}
