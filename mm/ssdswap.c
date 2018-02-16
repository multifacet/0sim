/*
 * A misc device which makes another device look like a non-rotational device.
 *
 * After loading this module, the sysfs can be used to choose a disk to make
 * non-rotational if it is not already The other device should not be mounted
 * or in use while this device is being configured or used! (Otherwise, you
 * could get weird results)...  The device is configured by writing the path of
 * the masked device to /sys/module/ssdswap/device. To unset the masked device,
 * write an empty string.
 */

#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/miscdevice.h>

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

// This device
static struct miscdevice ssdswap_dev;

// The block device we are filtering
static struct block_device *blkdev;

// The value of the QUEUE_NONROT flag before we fiddled with it.
static bool original_nonrot_flag;

// Unset the current device if there is one. This operation is idempotent.
static void unset_device(void) {
    if (blkdev) {
        if (!original_nonrot_flag) {
            queue_flag_clear_unlocked(QUEUE_FLAG_NONROT, bdev_get_queue(blkdev));
        }
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
    if (strlen(path)) { // strlen ok because we trust the kernel
        blkdev = lookup_bdev(path);
        if (IS_ERR(blkdev))     {
            printk ("No such block device.\n");
            return -EINVAL;
        }

        // Save the original value of the flag
        original_nonrot_flag = blk_queue_nonrot(bdev_get_queue(blkdev));

        // Then set it
        queue_flag_set_unlocked(QUEUE_FLAG_NONROT, bdev_get_queue(blkdev));
    }

    return 0;
}

// Callback for setting the device path parameter.
static int device_param_set(const char *val, const struct kernel_param *kp)
{
    return set_device(val);
}

////////////////////////////////////////////////////////////////////////////////
// Module init
////////////////////////////////////////////////////////////////////////////////

static int __init filter_init(void)
{
    int err;

    ssdswap_dev.minor = MISC_DYNAMIC_MINOR;
    ssdswap_dev.name = "ssdswap";

    err = misc_register(&ssdswap_dev);

    if (err) {
        return err;
    }

    printk(KERN_INFO "ssdswap on");

    return 0;
}

static void __exit filter_exit(void)
{
    unset_device();

    misc_deregister(&ssdswap_dev);

    printk(KERN_INFO "ssdswap off");
}

module_init(filter_init);
module_exit(filter_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mark Mansi <markm@cs.wisc.edu>");
MODULE_DESCRIPTION("A block filter device that makes another device look like an SSD");
