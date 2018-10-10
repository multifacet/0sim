
#include "x86_timing.h"

#include <linux/module.h>

static unsigned long long elapsed = 0;

void kvm_x86_elapse_time(unsigned long long extra) {
    elapsed += extra;
    printk("%llu extra\n", extra);
}
EXPORT_SYMBOL(kvm_x86_elapse_time);

void kvm_x86_reset_time(void)
{
    printk("reset\n");
    elapsed = 0;
}

unsigned long long kvm_x86_get_time(void)
{
    printk("host elapse %llu\n", elapsed);
    return elapsed;
}
