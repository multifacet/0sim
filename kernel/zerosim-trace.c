/*
 * Defines a sort-of-lightweight tracing mechanism inspired by Dick Site's
 * kutrace tracer.
 */

#include <linux/zerosim-trace.h>

/*
 * Trace from syscall handler start. %rax contains the syscall nr.
 */
asmlinkage void zerosim_trace_syscall_start(struct pt_regs)
{
    // TODO(markm)
}

/*
 * Trace from syscall handler end. %rax still contains the syscall nr.
 * `syscall_retval` is the return value of the system call.
 */
asmlinkage void zerosim_trace_syscall_end(u64 syscall_retval, struct pt_regs)
{
    // TODO(markm)
}
