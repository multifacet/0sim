
#include "x86_timing.h"

#include <linux/module.h>

static unsigned long long elapsed = 0;
static unsigned long long entry_exit_time = 0;

void kvm_x86_elapse_time(unsigned long long extra) {
    elapsed += extra + entry_exit_time;
    //printk(KERN_DEBUG "elapsed %llu\n", extra);
}
EXPORT_SYMBOL(kvm_x86_elapse_time);

void kvm_x86_set_entry_exit_time(int too_low) {
    if (too_low) {
        entry_exit_time += 10;
    } else {
        entry_exit_time -= 10;
    }
    printk(KERN_WARNING "entry exit time calibrated to %llu\n", entry_exit_time);
}

unsigned long long kvm_x86_get_entry_exit_time() {
    return entry_exit_time;
}
EXPORT_SYMBOL(kvm_x86_get_entry_exit_time);

void kvm_x86_reset_time(void)
{
    elapsed = 0;
    entry_exit_time = 0;
    //printk(KERN_DEBUG "elapsed reset");
}

unsigned long long kvm_x86_get_time(void)
{
    //printk(KERN_DEBUG "get elapsed %llu\n", elapsed);
    return elapsed;
}
