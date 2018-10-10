#ifndef __X86_TIMING_H__
#define __X86_TIMING_H__

void kvm_x86_elapse_time(unsigned long long);
unsigned long long kvm_x86_get_time(void);
void kvm_x86_reset_time(void);

#endif
