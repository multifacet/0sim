#ifndef __X86_TIMING_H__
#define __X86_TIMING_H__

#include <linux/types.h>

#define MAX_VCPUS 256

int zerosim_elapsed_init(void);

void zerosim_report_guest_offset(int, s64);
unsigned long long kvm_x86_get_time(int);

void kvm_x86_set_entry_exit_time(int);
unsigned long long kvm_x86_get_entry_exit_time(void);

void kvm_x86_set_page_fault_time(unsigned long long);
unsigned long long kvm_x86_get_page_fault_time(void);

#endif
