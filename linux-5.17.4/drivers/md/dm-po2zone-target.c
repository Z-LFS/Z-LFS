//ihhwang for NPO2

// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Samsung Electronics Co., Ltd.
 */

#include <linux/device-mapper.h>

#define DM_MSG_PREFIX "po2zone"

struct dm_po2z_target {
	struct dm_dev *dev;
	sector_t zone_size; /* Actual zone size of the underlying dev*/
	sector_t zone_size_po2; /* zone_size rounded to the nearest po2 value */
	unsigned int zone_size_po2_shift;
	sector_t zone_size_diff; /* diff between zone_size_po2 and zone_size */
	unsigned int nr_zones;
};

static inline unsigned int npo2_zone_no(struct dm_po2z_target *dmh,
					sector_t sect)
{
	return div64_u64(sect, dmh->zone_size);
}

static inline unsigned int po2_zone_no(struct dm_po2z_target *dmh,
				       sector_t sect)
{
	return sect >> dmh->zone_size_po2_shift;
}

static inline sector_t target_to_device_sect(struct dm_po2z_target *dmh,
					     sector_t sect)
{
	return sect - (po2_zone_no(dmh, sect) * dmh->zone_size_diff);
}

static inline sector_t device_to_target_sect(struct dm_po2z_target *dmh,
					     sector_t sect)
{
	return sect + (npo2_zone_no(dmh, sect) * dmh->zone_size_diff);
}

/*
 * This target works on the complete zoned device. Partial mapping is not
 * supported.
 * Construct a zoned po2 logical device: <dev-path>
 */
static int dm_po2z_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct dm_po2z_target *dmh = NULL;
	int ret;
	sector_t zone_size;
	sector_t dev_capacity;

	if (argc != 1)
		return -EINVAL;

	dmh = kmalloc(sizeof(*dmh), GFP_KERNEL);
	if (!dmh)
		return -ENOMEM;

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			    &dmh->dev);

	if (ret) {
		ti->error = "Device lookup failed";
		kfree(dmh);
		return ret;
	}

	zone_size = bdev_zone_sectors(dmh->dev->bdev);
	dev_capacity = get_capacity(dmh->dev->bdev->bd_disk);
	if (ti->len != dev_capacity || ti->begin) {
		DMERR("%pg Partial mapping of the target not supported",
		      dmh->dev->bdev);
		kfree(dmh);
		return -EINVAL;
	}

	if (is_power_of_2(zone_size))
		DMWARN("%pg:not a useful target for devices with po2 zone number of sectors",
		       dmh->dev->bdev);

	dmh->zone_size = zone_size;
	dmh->zone_size_po2 = 1 << get_count_order_long(zone_size);
	dmh->zone_size_po2_shift = ilog2(dmh->zone_size_po2);
	dmh->zone_size_diff = dmh->zone_size_po2 - dmh->zone_size;
	ti->private = dmh;
	ti->max_io_len = dmh->zone_size_po2;
	dmh->nr_zones = npo2_zone_no(dmh, ti->len);
	ti->len = dmh->zone_size_po2 * dmh->nr_zones;

	return 0;
}

static int dm_po2z_report_zones_cb(struct blk_zone *zone, unsigned int idx,
				   void *data)
{
	struct dm_report_zones_args *args = data;
	struct dm_po2z_target *dmh = args->tgt->private;

	zone->start = device_to_target_sect(dmh, zone->start);
	zone->wp = device_to_target_sect(dmh, zone->wp);
	zone->len = dmh->zone_size_po2;
	args->next_sector = zone->start + zone->len;

	return args->orig_cb(zone, args->zone_idx++, args->orig_data);
}

static int dm_po2z_report_zones(struct dm_target *ti,
				struct dm_report_zones_args *args,
				unsigned int nr_zones)
{
	struct dm_po2z_target *dmh = ti->private;
	sector_t sect = po2_zone_no(dmh, args->next_sector) * dmh->zone_size;

	return blkdev_report_zones(dmh->dev->bdev, sect, nr_zones,
				   dm_po2z_report_zones_cb, args);
}

static int dm_po2z_end_io(struct dm_target *ti, struct bio *bio,
			  blk_status_t *error)
{
	struct dm_po2z_target *dmh = ti->private;

	if (bio->bi_status == BLK_STS_OK && bio_op(bio) == REQ_OP_ZONE_APPEND)
		bio->bi_iter.bi_sector =
			device_to_target_sect(dmh, bio->bi_iter.bi_sector);

	return DM_ENDIO_DONE;
}

static void dm_po2z_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct dm_po2z_target *dmh = ti->private;

	limits->chunk_sectors = dmh->zone_size_po2;
}

static bool bio_across_emulated_zone_area(struct dm_po2z_target *dmh,
					  struct bio *bio)
{
	unsigned int zone_idx = po2_zone_no(dmh, bio->bi_iter.bi_sector);
	sector_t nr_sectors = bio->bi_iter.bi_size >> SECTOR_SHIFT;

	return (bio->bi_iter.bi_sector + nr_sectors) >
	       (zone_idx * dmh->zone_size_po2) + dmh->zone_size;
}

static int dm_po2z_map_read_emulated_area(struct dm_po2z_target *dmh,
					  struct bio *bio)
{
	sector_t start_sect = bio->bi_iter.bi_sector;
	unsigned int zone_idx = po2_zone_no(dmh, start_sect);
	sector_t relative_sect_in_zone = start_sect - (zone_idx * dmh->zone_size_po2);
	sector_t split_io_pos;

	/*
	 * If the starting sector is in the emulated area then fill
	 * all the bio with zeros. If bio is across emulated zone boundary,
	 * split the bio across boundaries and fill zeros only for the
	 * bio that is between the zone capacity and the zone size.
	 */
	if (relative_sect_in_zone < dmh->zone_size) {
		split_io_pos = (zone_idx * dmh->zone_size_po2) + dmh->zone_size;
		dm_accept_partial_bio(bio, split_io_pos - start_sect);
		bio->bi_iter.bi_sector = target_to_device_sect(dmh, start_sect);

		return DM_MAPIO_REMAPPED;
	}

	zero_fill_bio(bio);
	bio_endio(bio);
	return DM_MAPIO_SUBMITTED;
}

static int dm_po2z_map(struct dm_target *ti, struct bio *bio)
{
	struct dm_po2z_target *dmh = ti->private;

	bio_set_dev(bio, dmh->dev->bdev);
	if (bio_sectors(bio) || op_is_zone_mgmt(bio_op(bio))) {
		if (!bio_across_emulated_zone_area(dmh, bio)) {
			bio->bi_iter.bi_sector = target_to_device_sect(dmh,
								       bio->bi_iter.bi_sector);
			return DM_MAPIO_REMAPPED;
		}
		/*
		 * Read operation on the emulated zone area (between zone capacity
		 * and zone size) will fill the bio with zeroes. Any other operation
		 * in the emulated area should return an error.
		 */
		if (bio_op(bio) == REQ_OP_READ)
			return dm_po2z_map_read_emulated_area(dmh, bio);

		return DM_MAPIO_KILL;
	}
	return DM_MAPIO_REMAPPED;
}

static int dm_po2z_iterate_devices(struct dm_target *ti,
				   iterate_devices_callout_fn fn, void *data)
{
	struct dm_po2z_target *dmh = ti->private;
	sector_t len = dmh->nr_zones * dmh->zone_size;

	return fn(ti, dmh->dev, 0, len, data);
}

static struct target_type dm_po2z_target = {
	.name = "po2zone",
	.version = { 1, 0, 0 },
	.features = DM_TARGET_ZONED_HM | DM_TARGET_EMULATED_ZONES,
	.map = dm_po2z_map,
	.end_io = dm_po2z_end_io,
	.report_zones = dm_po2z_report_zones,
	.iterate_devices = dm_po2z_iterate_devices,
	.module = THIS_MODULE,
	.io_hints = dm_po2z_io_hints,
	.ctr = dm_po2z_ctr,
};

static int __init dm_po2z_init(void)
{
	return dm_register_target(&dm_po2z_target);
}

static void __exit dm_po2z_exit(void)
{
	dm_unregister_target(&dm_po2z_target);
}

/* Module hooks */
module_init(dm_po2z_init);
module_exit(dm_po2z_exit);

MODULE_DESCRIPTION(DM_NAME "power-of-2 zoned target");
MODULE_AUTHOR("Pankaj Raghav <p.raghav@samsung.com>");
MODULE_LICENSE("GPL");

