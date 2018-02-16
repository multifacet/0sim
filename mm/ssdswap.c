/*
 * This module defines a block device which makes another device look like a
 * SSD. This is not a filter deive (i.e. miscdevice) but a full-blown
 * block_device.
 *
 * The device to mask is configurable via sysfs and should not be mounted or in
 * use while this device is being configured or used! (Otherwise, you could get
 * weird results)...
 *
 * The device is configured by writing the path of the masked device to
 * /sys/module/ssdswap/device. To unset the masked device, write an empty
 * string.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>

////////////////////////////////////////////////////////////////////////////////
// Module parameters
////////////////////////////////////////////////////////////////////////////////

// The path to the device being filtered.
static char *device_path = NULL;

static int device_param_set(const char *, const struct kernel_param *);
static struct kernel_param_ops device_param_ops = {
    .set =      device_param_set,
    .get =      param_get_charp,
    .free =     param_free_charp,
};
module_param_cb(device, &device_param_ops, &device_path, 0644);

////////////////////////////////////////////////////////////////////////////////
// Masking and unmasking another device
////////////////////////////////////////////////////////////////////////////////

// This device (the mask)
static struct block_device ssdswap_dev;

// The block device we are filtering
static struct block_device *blkdev;

// A nop make_request_fn
static void no_mrf(struct request_queue *q, struct bio *bio) {
    // Do nothing...
}

// Unset the current device if there is one. This operation is idempotent.
static void unset_device(void) {
    if (blkdev) {
        ssdswap_dev.bd_disk->queue->make_request_fn = no_mrf;
        blkdev = NULL;
    }

    BUG_ON(blkdev);
}

// Set the current device to the given path. This operation is also idempotent.
static int set_device(const char *path)
{
    BUG_ON(!path);

    printk(KERN_INFO "ssdswap set device: %s", path);

    // Always unset...
    unset_device();
    BUG_ON(blkdev);

    // Set the new device (if provided)
    if (strlen(path)) { //strlen ok b/c we trust the kernel
        blkdev = lookup_bdev(path);
        if (IS_ERR(blkdev))     {
            printk ("No such block device.\n");
            return -EINVAL;
        }

        ssdswap_dev.bd_disk->queue->make_request_fn =
            blkdev->bd_disk->queue->make_request_fn;
    }

    return 0;
}

// Callback for setting the device path parameter.
static int device_param_set(const char *val, const struct kernel_param *kp)
{
    return set_device(val);
}

////////////////////////////////////////////////////////////////////////////////
// Block device driver
////////////////////////////////////////////////////////////////////////////////

// TODO: mask_req_fn should dispatch to the appropriate request fn of the masked dev

////////////////////////////////////////////////////////////////////////////////
// Module init
////////////////////////////////////////////////////////////////////////////////

static int __init filter_init(void)
{
    printk(KERN_INFO "ssdswap on");

    // TODO: register a block device
    int err = register_blkdev(major, "ssdswap", bdops);

    // TODO: init the queue with the proper request_fn_proc
    blk_init_queue(BLK_DEFAULT_QUEUE(major), mask_req_fn);

    // TODO: should set NONROT flag to convince swap system to do easy clustering

    if (err) { return err; }

    return 0;
}

static void __exit filter_exit(void)
{
    unset_device();

    // TODO: unregister the block device
    int err = unregister_blkdev(major, "ssdswap");

    blk_cleanup_queue(BLK_DEFAULT_QUEUE(major));

    printk(KERN_INFO "ssdswap off");
}

module_init(filter_init);
module_exit(filter_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mark Mansi <markm@cs.wisc.edu>");
MODULE_DESCRIPTION("A block filter device that makes another device look like an SSD");
