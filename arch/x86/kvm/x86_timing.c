
#include "x86_timing.h"

#include <linux/kvm_host.h>
#include <linux/module.h>
#include <linux/proc_fs.h>

///////////////////////////////////////////////////////////////////////////////
// Mechanism for reporting the current offsets.

// 16-digit hex + space
#define ELAPSED_BUF_SIZE ((16 + 1) * KVM_MAX_VCPUS)

static s64 guest_tsc_offsets[KVM_MAX_VCPUS] = {};

// Define a procfs file to get the amount of time elapsed on each vcpu
static struct proc_dir_entry *elapsed_ent;

static ssize_t elapsed_read_cb(
        struct file *file, char __user *ubuf, size_t count, loff_t *ppos)
{
    char buf[ELAPSED_BUF_SIZE];
    int len=0;
    int vcpu;

    if(*ppos > 0)
        return 0;

    // For each vcpu, print offset
    for (vcpu = 0; vcpu < KVM_MAX_VCPUS; ++vcpu) {
        if (len + 17 < ELAPSED_BUF_SIZE) {
            len += sprintf(buf + len, "%lld ", -guest_tsc_offsets[vcpu]);
        } else {
            printk(KERN_WARNING "out of space in elapsed_read_cb\n");
        }
    }

    buf[len++] = '\n'; // new line
    buf[len] = 0; // null terminate

    if(count < len)
        return 0;

    if(copy_to_user(ubuf, buf, len))
        return -EFAULT;

    *ppos = len;
    return len;
}

static struct proc_ops elapsed_ops =
{
    .proc_read = elapsed_read_cb,
};

int zerosim_elapsed_init(void)
{
	elapsed_ent =
        proc_create("zerosim_guest_offset", 0444, NULL, &elapsed_ops);

    printk(KERN_WARNING "inited elapsed\n");

	return 0;
}

// Set the current offset for the guest, so that it shows up in /proc/zerosim_guest_offset
void zerosim_report_guest_offset(int vcpu_id, s64 new_offset) {
    guest_tsc_offsets[vcpu_id] = new_offset;
}
EXPORT_SYMBOL(zerosim_report_guest_offset);

////////////////////////////////////////////////////////////////////////////////
// Mechanisms for tweaking the reported time.

static unsigned long long entry_exit_time = 0;
static unsigned long long page_fault_time = 0;

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

void kvm_x86_set_page_fault_time(unsigned long long time) {
    page_fault_time = time;
    printk(KERN_WARNING "page fault time calibrated to %llu\n", page_fault_time);
}

unsigned long long kvm_x86_get_page_fault_time() {
    return page_fault_time;
}
EXPORT_SYMBOL(kvm_x86_get_page_fault_time);

unsigned long long kvm_x86_get_time(int vcpu_id)
{
    return (u64)(-guest_tsc_offsets[vcpu_id]);
}
