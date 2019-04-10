#ifndef _ZEROSIM_TRACE_H_
#define _ZEROSIM_TRACE_H_

#include <linux/types.h>

asmlinkage void zerosim_trace_syscall_start(struct pt_regs);
asmlinkage void zerosim_trace_syscall_end(u64 syscall_retval, struct pt_regs);

#endif
