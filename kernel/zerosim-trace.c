/*
 * Defines a sort-of-lightweight tracing mechanism inspired by Dick Site's
 * kutrace tracer.
 *
 * See `include/kernel/zerosim-trace.h` for documentation.
 */

#include <linux/zerosim-trace.h>

#include <linux/syscalls.h>
#include <linux/spinlock.h>
#include <linux/compiler.h>
#include <linux/slab.h>

#include <asm/ptrace.h>
#include <asm/topology.h>

/* Various values for trace->flags */

// Type of event
#define ZEROSIM_TRACE_TASK_SWITCH   (0x00000001)
#define ZEROSIM_TRACE_INTERRUPT     (0x00000002)
#define ZEROSIM_TRACE_FAULT         (0x00000004)
#define ZEROSIM_TRACE_SYSCALL       (0x00000008)

// Set if this event is a start. Not set if end or N/A.
#define ZEROSIM_TRACE_START         (0x00000010)

// Number of events to buffer.
#define TRACE_BUF_SIZE (1000000)

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
    // The tail pointer.
    //
    // Points to the next slot _to be written_, so we should write and then
    // increment the pointer.
    //
    // We don't keep track of the number of events. The
    // buffer should be zeroed at the beginning and all valid events have
    // non-zero timestamps and flags, so we can tell when we get to the first
    // invalid element.
    u64 next;

    // Lock for this buffer.
    //
    // Since the lock can be grabbed from within an interrupt handler, it
    // should always be grabbed with spin_lock_irqsave/restore.
    spinlock_t buffer_lock;
};

/*
 * Global atomic flag indicating if tracing is on or not. It begins OFF (0) and
 * is switched ON (1) only during tracing.
 *
 * You must hold all buffer locks to change this.
 */
static atomic_t tracing_enabled = ATOMIC_INIT(0);

/*
 * Indicates whether the tracing system is ready. It might be unready due to
 * failed initialization or due to the fact that a snapshot is currently being
 * taken.
 *
 * You must hold all buffer locks to change this.
 */
static atomic_t ready = ATOMIC_INIT(0);

/* Each CPU has a buffer */
DEFINE_PER_CPU_SHARED_ALIGNED(struct trace_buffer, zerosim_trace_buffers);

/*
 * Init the zerosim tracer. This will allocate tracing buffer space for
 * everything. This happens during boot after arch initcalls.
 */
static __init int zerosim_trace_init(void)
{
    struct trace_buffer * tb;
    int cpu, node;

    for_each_possible_cpu(cpu) {
        node = cpu_to_node(cpu);

        tb = &per_cpu(zerosim_trace_buffers, cpu);

        tb->buf = kmalloc_node(TRACE_BUF_SIZE * sizeof(struct trace),
                               GFP_KERNEL | __GFP_ZERO,
                               node);

        if (tb->buf == NULL) {
            printk(KERN_WARNING "Unable to init zerosim_trace. kmalloc failed.\n");
            return -1; // Does nothing
        }

        tb->len = TRACE_BUF_SIZE;
        tb->next = 0;
    }

    atomic_set(&ready, 1);

    return 0;
}
subsys_initcall(zerosim_trace_init);

static long grab_all_locks(void)
{
    struct trace_buffer * tb;
    int cpu;
    unsigned long flags;

    for_each_possible_cpu(cpu) {
        tb = &per_cpu(zerosim_trace_buffers, cpu);
        if (cpu == 0) {
            // We save flags for the first lock
            spin_lock_irqsave(&tb->buffer_lock, flags);
        } else {
            spin_lock(&tb->buffer_lock);
        }
    }
}

static void release_all_locks(unsigned long flags)
{
    struct trace_buffer * tb;
    unsigned long ncpus = num_possible_cpus();
    int cpu;

    // release in reverse order
    for_each_possible_cpu(cpu) {
        tb = &per_cpu(zerosim_trace_buffers, ncpus - cpu - 1);
        if ((ncpus - cpu - 1) == 0) {
            spin_unlock_irqrestore(&tb->buffer_lock, flags);
        } else {
            spin_unlock(&tb->buffer_lock);
        }
    }
}

/*
 * Begin tracing on all cores. This should be called once per call to snapshot.
 */
SYSCALL_DEFINE0(zerosim_trace_begin)
{
    unsigned long flags = grab_all_locks();

    if (!READ_ONCE(&ready)) {
        release_all_locks(flags);
        return -ENOMEM;
    }

    if (atomic_add_unless(&tracing_enabled, 1, 1)) {
        release_all_locks(flags);
        return 0; // OK
    } else {
        release_all_locks(flags);
        return -EINPROGRESS; // Already tracing.
    }
}

/*
 * Stop tracing on all cores and copy a snapshot of the trace into the given
 * userspace buffer. This should only be called once per call to begin, and
 * must be called after begin is called.
 *
 * len must be at least TRACE_BUF_SIZE * num_cpus() bytes.
 */
SYSCALL_DEFINE2(zerosim_trace_snapshot,
                void*,          user_buf,
                unsigned long,  len)
{
    struct trace_buffer * tb;
    int cpu;

    unsigned long flags = grab_all_locks();

    if (!atomic_add_unless(&tracing_enabled, 0, -1)) {
        release_all_locks(flags);
        return -EBADE; // didn't call begin
    }
    if (!atomic_add_unless(&ready, 0, -1)) {
        release_all_locks(flags);
        return -EBADE; // wasn't ready
    }

    release_all_locks(flags);

    // We have to release the locks because `copy_to_user` can block.

    // Read to user buff. This may block.
    for_each_possible_cpu(cpu) {
        tb = &per_cpu(zerosim_trace_buffers, cpu);
        copy_to_user(user_buf, tb->buf, TRACE_BUF_SIZE * sizeof(struct trace));
    }

    // Zero all trace buffers
    for_each_possible_cpu(cpu) {
        tb = &per_cpu(zerosim_trace_buffers, cpu);
        memset(tb->buf, 0, TRACE_BUF_SIZE * sizeof(struct trace));
    }

    flags = grab_all_locks();
    atomic_set(&ready, 1);
    release_all_locks(flags);
}

/* Actually add the given event into the trace buffer, potentially overwriting
 * the oldest event in the buffer and updating the tail pointer.
 *
 * The buffer lock _should not_ be held by the caller.
 */
static inline zerosim_trace_event(struct trace *ev)
{
    struct trace_buffer *buf = this_cpu_ptr(zerosim_trace_buffers);
    unsigned long flags;
    spin_lock_irqsave(&buf->buffer_lock, flags);

    // check if tracing is enabled and ready
    if (!READ_ONCE(&tracing_enabled) || !READ_ONCE(&ready)) {
        spin_unlock_irqrestore(&buf->buffer_lock);
        return;
    }

    // push to buf
    buf->buf[buf->next] = *trace;
    buf->next = (buf->next + 1) % buf->len;

    spin_unlock_irqrestore(&buf->buffer_lock, flags);
}

void zerosim_trace_task_switch(struct task_struct *prev, struct task_struct *current)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) current->pid,
        .flags = ZEROSIM_TRACE_TASK_SWITCH,
    };

    // // check if tracing is enabled to avoid (probabalistically) waiting for the
    // // lock if someone is snapshotting.
    // if (!READ_ONCE(&tracing_enabled) || !READ_ONCE(&ready)) {
    //     return;
    // }

    zerosim_trace_event(&tr);
}

asmlinkage void zerosim_trace_syscall_start(struct pt_regs *reg)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) regs->orig_ax, // syscall nr
        .flags = ZEROSIM_TRACE_SYSCALL | ZEROSIM_TRACE_START,
    };

    // // check if tracing is enabled to avoid (probabalistically) waiting for the
    // // lock if someone is snapshotting.
    // if (!READ_ONCE(&tracing_enabled) || !READ_ONCE(&ready)) {
    //     return;
    // }

    zerosim_trace_event(&tr);
}

asmlinkage void zerosim_trace_syscall_end(u64 syscall_retval, struct pt_regs *regs)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) regs->orig_ax, // syscall nr
        .flags = ZEROSIM_TRACE_SYSCALL,
    };

    // // check if tracing is enabled to avoid (probabalistically) waiting for the
    // // lock if someone is snapshotting.
    // if (!READ_ONCE(&tracing_enabled) || !READ_ONCE(&ready)) {
    //     return;
    // }

    zerosim_trace_event(&tr);
}

void zerosim_trace_interrupt_start(struct pt_regs *regs)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) regs->orig_ax, // interrupt nr
        .flags = ZEROSIM_TRACE_INTERRUPT | ZEROSIM_TRACE_START,
    };

    // // check if tracing is enabled to avoid (probabalistically) waiting for the
    // // lock if someone is snapshotting.
    // if (!READ_ONCE(&tracing_enabled) || !READ_ONCE(&ready)) {
    //     return;
    // }

    zerosim_trace_event(&tr);
}

void zerosim_trace_interrupt_end(struct pt_regs *regs)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) regs->orig_ax, // interrupt nr
        .flags = ZEROSIM_TRACE_INTERRUPT,
    };

    // // check if tracing is enabled to avoid (probabalistically) waiting for the
    // // lock if someone is snapshotting.
    // if (!READ_ONCE(&tracing_enabled) || !READ_ONCE(&ready)) {
    //     return;
    // }

    zerosim_trace_event(&tr);
}

dotraplinkage void zerosim_trace_exception_start(struct pt_regs *regs, long error_code)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) error_code,
        .flags = ZEROSIM_TRACE_FAULT | ZEROSIM_TRACE_START,
    };

    // // check if tracing is enabled to avoid (probabalistically) waiting for the
    // // lock if someone is snapshotting.
    // if (!READ_ONCE(&tracing_enabled) || !READ_ONCE(&ready)) {
    //     return;
    // }

    zerosim_trace_event(&tr);
}

dotraplinkage void zerosim_trace_exception_end(struct pt_regs *regs, long error_code)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) error_code,
        .flags = ZEROSIM_TRACE_FAULT,
    };

    // // check if tracing is enabled to avoid (probabalistically) waiting for the
    // // lock if someone is snapshotting.
    // if (!READ_ONCE(&tracing_enabled) || !READ_ONCE(&ready)) {
    //     return;
    // }

    zerosim_trace_event(&tr);
}

