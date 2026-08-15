# hbd API

block device protection library. hooks the block layer entry points
with inline hooks (LKMhook `hk_inline`) and filters every bio, request
and deletion path against a registered target table. supports
read block, read zero, write sink, write shadow, delete protection for
whole disks and single partitions.

## Hook points

five entry hooks, all through `hk_inline`:

| symbol | path |
|--------|------|
| `submit_bio_noacct` | every bio, read and write, the only choke point for block IO |
| `blk_execute_rq` | SG_IO passthrough, matched by request queue |
| `del_gendisk` | whole disk removal |
| `bdev_del_partition` | BLKPG_DEL_PARTITION |
| `bdev_disk_changed` | BLKRRPART partition rescan |

at the `submit_bio_noacct` entry the bio sector is still partition
relative, the partition remap happens inside the function. the shadow
store keys on those relative sectors per target, no absolute address
math needed.

## Flags

```c
enum hbd_flag {
	HBD_RD_BLOCK	= 1 << 0,	// read -> BLK_STS_IOERR
	HBD_RD_ZERO	= 1 << 1,	// read -> zero filled, success
	HBD_WR_SINK	= 1 << 2,	// write -> dropped, success
	HBD_WR_SHADOW	= 1 << 3,	// write -> RAM shadow, reads see it
	HBD_NODEL_DISK	= 1 << 4,	// block del_gendisk, whole disk only
	HBD_NODEL_PART	= 1 << 5,	// block partition delete, partition only
};
```

`HBD_RD_BLOCK` and `HBD_RD_ZERO` are mutually exclusive, same for
`HBD_WR_SINK` and `HBD_WR_SHADOW`. `HBD_NODEL_DISK` is rejected on a
partition target, `HBD_NODEL_PART` on a whole disk target.

when `HBD_WR_SHADOW` is set, reads are always intercepted: shadowed
sectors return the shadow data, unshadowed sectors return zeros unless
`HBD_RD_BLOCK` is set (then they fail). write family ops are all
covered: WRITE, WRITE_ZEROES, DISCARD, SECURE_ERASE. FLUSH has no data
and passes through to the original, which completes it as success.

delete protection is strictly per registration: protecting a partition
does not protect del_gendisk of its disk, the whole disk must be
registered with `HBD_NODEL_DISK` for that. `bdev_disk_changed` is
blocked when any target sits on the disk, because a rescan would drop
the protected partitions.

## Lifecycle

**int hbd_init(void)**

install the five inline hooks. requires LKMhook `hk_init` with a
resolver first. returns the first hook failure, previously installed
hooks are uninstalled.

**void hbd_exit(void)**

uninstall the hooks and clear every target. call before `hk_exit`.

## Registration

**int hbd_protect(const char *name, u32 flags, hbd_read_cb cb, void *priv)**

protect the device named by name. name resolution tries
`/dev/block/by-name/<name>` first, then `/dev/<name>`, absolute paths
are used as is, so `sda1`, `abl_a` and `/dev/ram0` all work. the
target is keyed by the exact device number, registering a whole disk
covers only the whole disk IO, registering a partition covers only
that partition. -EEXIST when the name is already registered.
-EINVAL on bad flags or target kind. the read callback and priv are
stored on the target.

**int hbd_unprotect(const char *name)**

remove the target. waits for in-flight hooks (synchronize_rcu) and
releases the reference, memory is freed after the RCU grace period.
-ENOENT when the name is not registered.

**void hbd_clear(void)**

remove every target.

**int hbd_for_each(int (*fn)(struct hbd_target *t, void *arg), void *arg)**

walk the target table under RCU, fn is called for each target until it
returns non-zero. read only, the target must not be freed or modified.

**void hbd_set_shadow_cap(size_t max_pages)**

cap for shadow allocations, 0 means unlimited. applies to targets
registered afterwards.

## Read callback

```c
typedef int (*hbd_read_cb)(struct hbd_target *t, struct bio *bio, u64 sect);
```

optional per target. called on every intercepted read with the
partition relative sector.

- returns > 0: the callback completed the bio itself, the library
  skips everything
- returns 0: the library applies the flag driven default
- returns < 0: the bio is completed with BLK_STS_IOERR

the callback runs in the bio submission context, safe to sleep and to
call bio_endio.

## Target

```c
struct hbd_target {
	char name[HBD_NAME_MAX];
	dev_t dev;		// exact device number
	dev_t disk_dev;		// whole disk base device number
	unsigned int partno;	// 0 = whole disk
	struct request_queue *queue;
	u32 flags;
	hbd_read_cb on_read;
	void *priv;
	struct hbd_shadow *shadow;
	struct list_head node;
	struct kref ref;
	struct rcu_head rcu;
};
```

created by hbd_protect, do not allocate or free manually.

## Lookup API

used by the io and del hook layers, exported for consumers that want
to test a device against the table.

**struct hbd_target \*hbd_lookup_dev(dev_t dev)**

find the exact target, takes a reference. the caller must release it
with hbd_put_target.

**struct hbd_target \*hbd_lookup_queue(struct request_queue \*q)**

find a target on the queue, takes a reference. used by the
blk_execute_rq hook, queue matching means SG_IO protection is per
disk, partitions share the queue.

**bool hbd_lookup_disk_whole(dev_t ddev)**

true when a whole disk target with HBD_NODEL_DISK sits on the disk.

**bool hbd_lookup_disk_part(dev_t ddev, int partno)**

true when a partition target with HBD_NODEL_PART matches.

**bool hbd_lookup_disk_any(dev_t ddev)**

true when any target sits on the disk.

**void hbd_put_target(struct hbd_target \*t)**

drop a reference taken by hbd_lookup_dev or hbd_lookup_queue.

## Shadow

`HBD_WR_SHADOW` allocates a shadow store per target: an xarray keyed
by 4K block, pages allocated lazily with GFP_NOIO on first write,
capped by hbd_set_shadow_cap. a write copies the bio data into the
store and completes with success, the disk is never touched. reads
serve shadow data for written blocks, zeros (or an error with
HBD_RD_BLOCK) for untouched ones.

## Behavior notes

- the matching is exact device numbers, no gendisk pointers are held,
  no dangling pointer risk when a device goes away
- unprotected devices pay one detour branch per bio, the wrappers
  fall through to the original immediately
- SG_IO passthrough is blocked at blk_execute_rq: the request never
  reaches the device, the ioctl reports a no-op
- the target table is RCU protected, the hot path takes no locks
