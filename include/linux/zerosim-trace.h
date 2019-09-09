#ifndef _ZEROSIM_TRACE_H_
#define _ZEROSIM_TRACE_H_

#include <linux/types.h>
#include <linux/sched.h>

#include <asm/traps.h>

/*
 * Init the zerosim tracer. This will allocate tracing buffer space for
 * everything. This happens during boot before the init process is created.
 */
__init int zerosim_trace_init(void);

/*
 * Trace from task switch. We are given the previous and current (new) tasks.
 */
void zerosim_trace_task_switch(struct task_struct *prev, struct task_struct *curr);

/*
 * Trace from syscall handler start. %rax contains the syscall nr.
 */
asmlinkage void zerosim_trace_syscall_start(struct pt_regs *);

/*
 * Trace from syscall handler end. %rax still contains the syscall nr.
 * `syscall_retval` is the return value of the system call.
 */
asmlinkage void zerosim_trace_syscall_end(u64 syscall_retval, struct pt_regs *);

/*
 * Start trace from interrupt handler for the given vector.
 */
void zerosim_trace_interrupt_start(struct pt_regs *regs);

/*
 * End trace from interrupt handler for the given vector.
 */
void zerosim_trace_interrupt_end(struct pt_regs *regs);

/*
 * Start trace from exception handler for the given exception.
 */
dotraplinkage void zerosim_trace_exception_start(struct pt_regs *regs, long error_code);

/*
 * Start trace from exception handler for the given exception.
 */
dotraplinkage void zerosim_trace_exception_end(struct pt_regs *regs, long error_code);

/*
 * Start trace from softirq processing.
 */
void zerosim_trace_softirq_start(void);

/*
 * End trace from softirq processing.
 */
void zerosim_trace_softirq_end(void);

/*
 * Trace from VM enter.
 */
void zerosim_trace_vm_enter(int vcpu_id);

/*
 * Trace from VM exit.
 */
void zerosim_trace_vm_exit(unsigned long reason, unsigned long qual);

/*
 * Trace from VM delay begin.
 */
void zerosim_trace_vm_delay_begin(int vcpu_id, unsigned long behind);

/*
 * Trace from VM delay end.
 */
void zerosim_trace_vm_delay_end(int vcpu_id);

#endif
