/*
 * A module which makes another device look like a non-rotational device.
 *
 * After loading this module, the sysfs can be used to make a device
 * non-rotational. The device should be mounted, but should not be in active
 * use. To reset, reboot the machine.
 */

#include <linux/module.h>
#include <linux/blkdev.h>

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
// Make another device NONROT
////////////////////////////////////////////////////////////////////////////////

// The maximum length of device path accepted
#define MAX_PATH_LEN 256

// The block device we are filtering
static struct block_device *blkdev;

static int isspace(int c) {
    switch (c) {
        case '\n':
        case ' ':
        case '\t':
        case '\r':
            return true;
        default:
            return false;
    }
}

// strncpy, but also it
// - strips whitespace
// - returns the new (stripped) length
static unsigned long strncpy_strip(char *dest, const char *src, unsigned long max)
{
    unsigned long i, j;
    for(i = 0, j = 0;
        (dest[j] = src[i]) && j < max && i < max;
        j+=!isspace(src[i++]));

    return j;
}

// Set the current device to the given path. This operation is also idempotent.
static int set_device(const char *path)
{
    unsigned long path_len;
    char buf[MAX_PATH_LEN] = {0};

    BUG_ON(!path);

    path_len = strnlen(path, MAX_PATH_LEN);
    if (path_len == MAX_PATH_LEN) {
        printk(KERN_INFO "path is too long");
        return -EINVAL;
    }

    printk(KERN_INFO "ssdswap set device: %s", path);

    // strip any whitespace
    path_len = strncpy_strip(buf, path, MAX_PATH_LEN);

    // Set the new device (if provided)
    if (path_len) {
        blkdev = lookup_bdev(buf);
        if (IS_ERR(blkdev))     {
            blkdev = NULL;
            printk ("No such block device.\n");
            return -EINVAL;
        }

        if (!blkdev->bd_disk) {
            blkdev = NULL;
            printk ("Cannot use a child device.\n");
            return -EINVAL;
        }

        BUG_ON(!blkdev->bd_disk);

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

static int __init ssdswap_init(void)
{
    printk(KERN_INFO "ssdswap on");

    return 0;
}

static void __exit ssdswap_exit(void)
{
    printk(KERN_INFO "ssdswap off");
}

module_init(ssdswap_init);
module_exit(ssdswap_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mark Mansi <markm@cs.wisc.edu>");
MODULE_DESCRIPTION("A block filter device that makes another device look like an SSD");
