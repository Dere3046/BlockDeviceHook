// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/bio.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include "hbd_shadow.h"

struct hbd_shadow *hbd_shadow_alloc(size_t max_pages)
{
	struct hbd_shadow *s;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return NULL;
	xa_init(&s->xa);
	mutex_init(&s->lock);
	s->max_pages = max_pages;
	return s;
}

void hbd_shadow_free(struct hbd_shadow *s)
{
	unsigned long idx;
	void *page;

	if (!s)
		return;
	xa_for_each(&s->xa, idx, page)
		kfree(page);
	xa_destroy(&s->xa);
	kfree(s);
}

size_t hbd_shadow_pages(struct hbd_shadow *s)
{
	size_t n;

	mutex_lock(&s->lock);
	n = s->nr_pages;
	mutex_unlock(&s->lock);
	return n;
}

static void bio_copy_bytes(struct bio *bio, u64 byte_off, void *buf,
			   size_t len, bool to_bio)
{
	struct bio_vec bvec;
	struct bvec_iter iter;
	u64 cur = 0;

	bio_for_each_segment(bvec, bio, iter) {
		u64 end = cur + bvec.bv_len;
		size_t take;
		char *base;
		char *dst;

		if (byte_off >= end) {
			cur = end;
			continue;
		}
		take = min((size_t)(end - byte_off), len);
		base = kmap_local_page(bvec.bv_page);
		dst = base + bvec.bv_offset + (byte_off - cur);
		if (to_bio)
			memcpy(dst, buf, take);
		else
			memcpy(buf, dst, take);
		kunmap_local(base);
		buf += take;
		len -= take;
		byte_off += take;
		cur = end;
		if (!len)
			break;
	}
}

static void bio_zero_bytes(struct bio *bio, u64 byte_off, size_t len)
{
	struct bio_vec bvec;
	struct bvec_iter iter;
	u64 cur = 0;

	bio_for_each_segment(bvec, bio, iter) {
		u64 end = cur + bvec.bv_len;
		size_t take;
		char *base;
		char *dst;

		if (byte_off >= end) {
			cur = end;
			continue;
		}
		take = min((size_t)(end - byte_off), len);
		base = kmap_local_page(bvec.bv_page);
		dst = base + bvec.bv_offset + (byte_off - cur);
		memset(dst, 0, take);
		kunmap_local(base);
		len -= take;
		byte_off += take;
		cur = end;
		if (!len)
			break;
	}
}

void hbd_bio_zero(struct bio *bio)
{
	bio_zero_bytes(bio, 0, bio->bi_iter.bi_size);
}

static u64 block_start(u64 sect)
{
	u64 idx = sect >> PAGE_SECTORS_SHIFT;

	return idx << (PAGE_SECTORS_SHIFT + SECTOR_SHIFT);
}

static size_t block_take(u64 sect, u64 total)
{
	u64 sec_off = sect << SECTOR_SHIFT;
	u64 off = sec_off - block_start(sect);
	size_t left_in_block = PAGE_SIZE - off;

	return min_t(u64, total, left_in_block);
}

int hbd_shadow_write(struct hbd_shadow *s, struct bio *bio, u64 sect)
{
	u64 bytes = bio->bi_iter.bi_size;
	u64 pos = 0;
	int ret = 0;

	mutex_lock(&s->lock);
	while (pos < bytes) {
		u64 sec = sect + (pos >> SECTOR_SHIFT);
		u64 idx = sec >> PAGE_SECTORS_SHIFT;
		size_t take = block_take(sec, bytes - pos);
		void *page = xa_load(&s->xa, idx);

		if (!page) {
			if (s->max_pages && s->nr_pages >= s->max_pages) {
				ret = -ENOSPC;
				goto out;
			}
			page = kzalloc(PAGE_SIZE, GFP_NOIO);
			if (!page) {
				ret = -ENOMEM;
				goto out;
			}
			if (xa_is_err(xa_store(&s->xa, idx, page, GFP_NOIO))) {
				kfree(page);
				ret = -ENOMEM;
				goto out;
			}
			s->nr_pages++;
		}
		bio_copy_bytes(bio, pos, page + ((sec << SECTOR_SHIFT) -
						 block_start(sec)), take, false);
		pos += take;
	}
out:
	mutex_unlock(&s->lock);
	return ret;
}

int hbd_shadow_read(struct hbd_shadow *s, struct bio *bio, u64 sect,
		    bool zero_missing)
{
	u64 bytes = bio->bi_iter.bi_size;
	u64 pos = 0;
	bool missing = false;

	mutex_lock(&s->lock);
	while (pos < bytes) {
		u64 sec = sect + (pos >> SECTOR_SHIFT);
		u64 idx = sec >> PAGE_SECTORS_SHIFT;
		size_t take = block_take(sec, bytes - pos);
		void *page = xa_load(&s->xa, idx);

		if (page)
			bio_copy_bytes(bio, pos, page + ((sec << SECTOR_SHIFT) -
							 block_start(sec)),
				       take, true);
		else if (zero_missing)
			bio_zero_bytes(bio, pos, take);
		else
			missing = true;
		pos += take;
	}
	mutex_unlock(&s->lock);
	return missing ? -ENOENT : 0;
}

int hbd_shadow_zero(struct hbd_shadow *s, u64 sect, u64 bytes)
{
	u64 pos = 0;

	mutex_lock(&s->lock);
	while (pos < bytes) {
		u64 sec = sect + (pos >> SECTOR_SHIFT);
		u64 idx = sec >> PAGE_SECTORS_SHIFT;
		size_t take = block_take(sec, bytes - pos);
		void *page = xa_load(&s->xa, idx);

		if (page)
			memset(page + ((sec << SECTOR_SHIFT) -
				       block_start(sec)), 0, take);
		pos += take;
	}
	mutex_unlock(&s->lock);
	return 0;
}
