/*
 * Defines a sort-of-lightweight tracing mechanism inspired by Dick Site's
 * kutrace tracer.
 */

#include <linux/zerosim-trace.h>

/* A single event, packed to take 2 words */
struct trace {
    // The timestamp of the event
    u64 timestamp;
    // Some identifier for the event. This allows us to match corresponding
    // start-end events, identify tasks being switched, etc.
    u32 id;
    // Flags to indicate what kind of event this is.
    u32 flags;
};

/* A per-cpu buffer for trace events */
struct trace_buffer {
    // The buffer itself.
    struct trace *buf;
    // The number of `struct traces` of the buffer (not bytes);
    u64 len;
    // The tail pointer. We don't keep track of the number of events. The
    // buffer should be zeroed at the beginning and all valid events have
    // non-zero timestamps and flags, so we can tell when we get to the first
    // invalid element.
    u64 next;
};

/* Each CPU has a buffer */
DECLARE_PER_CPU(struct trace_buffer *, zerosim_trace_buffers);

// TODO init somewhere

void zerosim_trace_task_switch(struct task *prev, struct task *current)
{
    // TODO(markm)
}

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

/*
 * Start trace from interrupt handler for the given vector.
 */
void zerosim_trace_interrupt_start(struct pt_regs *regs)
{
    // TODO(markm)
}
/*
 * End trace from interrupt handler for the given vector.
 */
void zerosim_trace_interrupt_end(struct pt_regs *regs)
{
    // TODO(markm)
}

/*
 * Start trace from exception handler for the given exception.
 */
dotraplinkage void zerosim_trace_exception_start(struct pt_regs *regs, long error_code)
{
    // TODO(markm)
}

/*
 * Start trace from exception handler for the given exception.
 */
dotraplinkage void zerosim_trace_exception_end(struct pt_regs *regs, long error_code)
{
    // TODO(markm)
}

