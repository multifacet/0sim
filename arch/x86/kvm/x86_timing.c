
#include "x86_timing.h"

#include <linux/module.h>

static unsigned long long elapsed = 0;

void kvm_x86_elapse_time(unsigned long long extra) {
    elapsed += extra;
    //printk(KERN_DEBUG "elapsed %llu\n", extra);
}
EXPORT_SYMBOL(kvm_x86_elapse_time);

void kvm_x86_reset_time(void)
{
    elapsed = 0;
    //printk(KERN_DEBUG "elapsed reset");
}

unsigned long long kvm_x86_get_time(void)
{
    //printk(KERN_DEBUG "get elapsed %llu\n", elapsed);
    return elapsed;
}
