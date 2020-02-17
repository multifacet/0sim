/*
 * Stats about the memory management subsystem.
 */

#include <linux/mm_stats.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#define MM_STATS_INSTR_BUFSIZE 24

struct mm_hist;

static int hist_sprintf(struct file *file, char __user *ubuf,
        size_t count, loff_t *ppos, struct mm_hist *hist);

#define MM_STATS_PROC_CREATE_INT_INNER(type, name, default_val, fmt, extra_code) \
    type name = default_val; \
    static struct proc_dir_entry *name##_ent; \
    \
    static ssize_t name##_read_cb( \
            struct file *file, char __user *ubuf,size_t count, loff_t *ppos) \
    { \
        char buf[MM_STATS_INSTR_BUFSIZE]; \
        int len=0; \
 \
        if(*ppos > 0) \
            return 0; \
 \
        len += sprintf(buf, fmt "\n", name); \
 \
        if(count < len) \
            return 0; \
 \
        if(copy_to_user(ubuf, buf, len)) \
            return -EFAULT; \
 \
        *ppos = len; \
        return len; \
    } \
    \
    static ssize_t name##_write_cb( \
            struct file *file, const char __user *ubuf, size_t len, loff_t *offset) \
    { \
        int num; \
        type val; \
        char input[MM_STATS_INSTR_BUFSIZE]; \
 \
        if(*offset > 0 || len > MM_STATS_INSTR_BUFSIZE) { \
            return -EFAULT; \
        } \
 \
        if(copy_from_user(input, ubuf, len)) { \
            return -EFAULT; \
        } \
 \
        num = sscanf(input, fmt, &val); \
        if(num != 1) { \
            return -EINVAL; \
        } \
 \
        name = val; \
        extra_code \
 \
        printk(KERN_WARNING "mm-econ: %s = " fmt "\n", #name, name); \
 \
        return len; \
    } \
    \
    static struct file_operations name##_ops = \
    { \
        .write = name##_write_cb, \
        .read = name##_read_cb, \
    };

#define MM_STATS_PROC_CREATE_INT(type, name, default_val, fmt) \
    MM_STATS_PROC_CREATE_INT_INNER(type, name, default_val, fmt, { /* nothing */ })

#define MM_STATS_INIT_INT(name) \
    name##_ent = proc_create(#name, 0444, NULL, &name##_ops);

#define MM_STATS_PROC_CREATE_HIST_OUTPUT(name) \
    static struct proc_dir_entry *name##_ent; \
    \
    static ssize_t name##_read_cb( \
            struct file *file, char __user *ubuf,size_t count, loff_t *ppos) \
    { \
        return hist_sprintf(file, ubuf, count, ppos, &name); \
    } \
    \
    static struct file_operations name##_ops = \
    { \
        .read = name##_read_cb, \
    };

#define MM_STATS_INIT_MM_HIST(name) \
    reinit_mm_hist(&name, name##_nbins, name##_min, \
            name##_width, name##_is_exp)

#define MM_STATS_PROC_CREATE_HIST(name) \
    unsigned int name##_nbins; \
    u64 name##_min; \
    u64 name##_width; \
    int name##_is_exp; \
    struct mm_hist name; \
    MM_STATS_PROC_CREATE_INT_INNER(unsigned int, name##_nbins, 20, "%u", \
        { MM_STATS_INIT_MM_HIST(name); }); \
    MM_STATS_PROC_CREATE_INT_INNER(u64, name##_min, 0, "%llu", \
        { MM_STATS_INIT_MM_HIST(name); }); \
    MM_STATS_PROC_CREATE_INT_INNER(u64, name##_width, 1000, "%llu", \
        { MM_STATS_INIT_MM_HIST(name); }); \
    MM_STATS_PROC_CREATE_INT_INNER(int, name##_is_exp, 1, "%d", \
        { MM_STATS_INIT_MM_HIST(name); }); \
    MM_STATS_PROC_CREATE_HIST_OUTPUT(name);

#define MM_STATS_INIT_HIST(name) \
    MM_STATS_INIT_INT(name##_nbins); \
    MM_STATS_INIT_INT(name##_min); \
    MM_STATS_INIT_INT(name##_width); \
    MM_STATS_INIT_INT(name##_is_exp); \
    name##_ent = proc_create(#name, 0444, NULL, &name##_ops); \
    name.bins = NULL; \
    if (MM_STATS_INIT_MM_HIST(name)) { \
        pr_warn("mm-econ: unable to init histogram " #name); \
    }

/*
 * A histogram of u64 values. The minimum and number of bins are adjustable,
 * and there are counters for the number of values that fall outside of the
 * [min, max) range of the histogram. The histogram can also be set to be
 * linear or exponential.
 *
 * If the bins are linear, then bin i contains the frequency of range
 * [min + i*width, min + (i+1)*width).
 *
 * If the bins are exponential, the ith bin contains the frequency of range
 * [min + (2^i)*width, min + (2^(i+1))*width).
 */
struct mm_hist {
    // The number of bins.
    unsigned int n;
    // The smallest value.
    u64 min;
    // The width of a bin.
    u64 width;
    // Are the bin-widths exponentially increasing or linearly increasing?
    bool is_exp;

    // An array of u64 for the data.
    u64 *bins;

    // Frequency of values lower than min or higher than max.
    u64 too_lo_count;
    u64 too_hi_count;
};

static int reinit_mm_hist(struct mm_hist *hist, unsigned int n,
        u64 min, u64 width, bool is_exp) {
    pr_warn("mm-econ: reset mm_hist n=%u min=%llu width=%llu isexp=%d",
            n, min, width, is_exp);

    hist->n = n;
    hist->min = min;
    hist->width = width;
    hist->is_exp = is_exp;
    hist->too_hi_count = 0;
    hist->too_lo_count = 0;

    if (hist->bins) {
        vfree(hist->bins);
    }

    if (hist-> n < 1) {
        pr_warn("mm-econ: adjusting nbins to 1.");
        hist->n = 1;
    }

    hist->bins = (u64 *)vzalloc(n * sizeof(u64));

    if (hist->bins) {
        return 0;
    } else {
        pr_warn("mm-econ: unable to allocate histogram");
        return -ENOMEM;
    }
}

// Returns the bit index of the most-significant bit in val (starting from 0).
static inline int highest_bit(u64 val)
{
    int msb = 0;

    if (val == 0) {
        // There was no set bit. This is an error.
        pr_warn("mm-econ: no MSB, but expected one.");
        return -1;
    }

    if (val & 0xFFFFFFFF00000000) {
        msb += 32;
        val >>= 32;
    }

    if (val & 0xFFFF0000) {
        msb += 16;
        val >>= 16;
    }

    if (val & 0xFF00) {
        msb += 8;
        val >>= 8;
    }

    if (val & 0xF0) {
        msb += 4;
        val >>= 4;
    }

    if (val & 0xC) {
        msb += 2;
        val >>= 2;
    }

    if (val & 0x2) {
        msb += 1;
        val >>= 1;
    }

    return msb;
}

void mm_stats_hist_measure(struct mm_hist *hist, u64 val)
{
    unsigned int bin_idx;

    // Some sanity checking.
    BUG_ON(hist->n == 0);

    if (!hist->bins) {
        pr_warn("mm-econ: no bins allocated.");
        return;
    }

    // Find out if it is too low or too high.
    if (val < hist->min) {
        hist->too_lo_count++;
        return;
    }
    if (hist->is_exp) {
        if (val >= (hist->min + ((1 << (hist->n - 1)) * hist->width))) {
            hist->too_hi_count++;
            return;
        }
    } else {
        if (val >= (hist->min + (hist->n - 1) * hist->width)) {
            hist->too_hi_count++;
            return;
        }
    }

    // If we get here, we know there is a bin for the data. Find out which one.
    if (hist->is_exp) {
        bin_idx = (val - hist->min) / hist->width;
        if (bin_idx > 0) {
            bin_idx = highest_bit(bin_idx) + 1;
        }
    } else {
        bin_idx = (val - hist->min) / hist->width;
    }

    BUG_ON(bin_idx >= hist->n);
    hist->bins[bin_idx]++;
}

/*
 * Reads a histogram and creates the output string to be reported to the user
 */
static int hist_sprintf(struct file *file, char __user *ubuf,
        size_t count, loff_t *ppos, struct mm_hist *hist)
{
        size_t buf_size = min(count, (size_t)((hist->n + 2) * 16 + 1));
        char *buf;
        int len=0;
        int i;

        if(*ppos > 0)
            return 0;

        buf = (char *)vmalloc(buf_size);
        if (!buf) {
            pr_warn("mm-econ: Unable to allocate results string buffer.");
            return -ENOMEM;
        }

        len += sprintf(buf, "%llu %llu ", hist->too_lo_count, hist->too_hi_count);
        for (i = 0; i < hist->n; ++i) {
            len += sprintf(&buf[len], "%llu ", hist->bins[i]);
        }
        buf[len++] = '\0';

        if(count < len) {
            vfree(buf);
            return 0;
        }

        if(copy_to_user(ubuf, buf, len)) {
            vfree(buf);
            return -EFAULT;
        }

        vfree(buf);

        *ppos = len;
        return len;
}

///////////////////////////////////////////////////////////////////////////////
// Define various stats below.

// Histogram of page fault latency.
MM_STATS_PROC_CREATE_HIST(mm_page_fault_cycles);

// Histograms of compaction events.
MM_STATS_PROC_CREATE_HIST(mm_direct_compaction_cycles);
MM_STATS_PROC_CREATE_HIST(mm_indirect_compaction_cycles);

void mm_stats_init(void)
{
    MM_STATS_INIT_HIST(mm_page_fault_cycles);
    MM_STATS_INIT_HIST(mm_direct_compaction_cycles);
    MM_STATS_INIT_HIST(mm_indirect_compaction_cycles);
}
