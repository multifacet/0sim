#include<linux/fs.h>
#include<linux/proc_fs.h>
#include<linux/seq_file.h>
#include<linux/kernel.h>
#include<linux/init.h>
#include<linux/utsname.h>

unsigned long profile_hist_alloc_order[MAX_ORDER];

static int order_profile_show(struct seq_file *m,void *v)
{
    int i;

    for ( i = 0; i < MAX_ORDER ; i++ ) {
        seq_printf(m, "\nprofile_hist_alloc_order[%d] : %ld",i,profile_hist_alloc_order[i]);
    }

    seq_printf(m, "\n");

    return 0;
}

static int order_profile_open(struct inode *inode,struct file *file)
{
    return single_open(file, order_profile_show, NULL);
}

static const struct file_operations order_profile_fops = {
    .open = order_profile_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static int __init order_profile_init(void)
{
    proc_create("order_profile", 0, NULL, &order_profile_fops);
    return 0;
}
fs_initcall(order_profile_init);
