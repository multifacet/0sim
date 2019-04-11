#ifndef _ZEROSIM_TRACE_H_
#define _ZEROSIM_TRACE_H_

#include <linux/types.h>

// Trace task switch
void zerosim_trace_task_switch(struct task *prev, struct task *next);

// Trace syscalls
asmlinkage void zerosim_trace_syscall_start(struct pt_regs *);
asmlinkage void zerosim_trace_syscall_end(u64 syscall_retval, struct pt_regs *);

// Trace IRQs and IPIs
void zerosim_trace_interrupt_start(struct pt_regs *regs);
void zerosim_trace_interrupt_end(struct pt_regs *regs);

// Trace Exception
dotraplinkage void zerosim_trace_exception_start(struct pt_regs *regs, long error_code);
dotraplinkage void zerosim_trace_exception_end(struct pt_regs *regs, long error_code);

#endif
