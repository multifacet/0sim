#ifndef _ZEROSIM_PARAMS_H_
#define _ZEROSIM_PARAMS_H_

#include <linux/proc_fs.h>

#define ZEROSIM_INSTR_BUFSIZE 256

#define ZEROSIM_PROC_CREATE(type, name, default_val, fmt) \
    static type name = default_val; \
    static struct proc_dir_entry *name##_ent; \
    \
    static ssize_t name##_read_cb( \
            struct file *file, char __user *ubuf,size_t count, loff_t *ppos) \
    { \
        char buf[ZEROSIM_INSTR_BUFSIZE]; \
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
        char input[ZEROSIM_INSTR_BUFSIZE]; \
 \
        if(*offset > 0 || len > ZEROSIM_INSTR_BUFSIZE) { \
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
 \
        printk(KERN_WARNING "zerosim: %s = " fmt "\n", #name, name); \
 \
        return len; \
    } \
    \
    static struct file_operations name##_ops = \
    { \
        .write = name##_write_cb, \
        .read = name##_read_cb, \
    };

#endif
