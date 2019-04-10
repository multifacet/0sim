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
#include <linux/percpu.h>
#include <linux/module.h>

#include <asm/ptrace.h>
#include <asm/topology.h>

/* Various values for trace->flags */

// Type of event
#define ZEROSIM_TRACE_TASK_SWITCH   (0x00000001)
#define ZEROSIM_TRACE_INTERRUPT     (0x00000002)
#define ZEROSIM_TRACE_FAULT         (0x00000003)
#define ZEROSIM_TRACE_SYSCALL       (0x00000004)
#define ZEROSIM_TRACE_SOFTIRQ       (0x00000005)
#define ZEROSIM_TRACE_VMENTEREXIT   (0x00000006)
#define ZEROSIM_TRACE_VMDELAY       (0x00000007)

// Set if this event is a start. Not set if end or N/A.
#define ZEROSIM_TRACE_START         (0x80000000)

/* A single event, packed to take 2 words */
struct trace {
    // The timestamp of the event
    u64 timestamp;
    // Some identifier for the event. This allows us to match corresponding
    // start-end events, identify tasks being switched, etc.
    u32 id;
    // Flags to indicate what kind of event this is.
    u32 flags;
    // PID of `current` at time of event.
    u32 pid;
    // Any extra useful info (e.g. error codes, prev task pid, etc).
    u32 extra;
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

/*
 * Used for signaling that the buffer is being copied currently.
 *
 * You must hold all buffer locks to change this.
 */
static atomic_t hold_buffers = ATOMIC_INIT(0);

static u64 trace_buf_size = 1 << 12;

/* Each CPU has a buffer */
DEFINE_PER_CPU_SHARED_ALIGNED(struct trace_buffer, zerosim_trace_buffers);

__init int zerosim_trace_init(void)
{
    struct trace_buffer * tb;
    int cpu;

    for_each_possible_cpu(cpu) {
        tb = &per_cpu(zerosim_trace_buffers, cpu);
        tb->buf = NULL;
        tb->len = 0;
        tb->next = 0;
        spin_lock_init(&tb->buffer_lock);
    }

    return 0;
}

static long grab_all_locks(void)
{
    struct trace_buffer * tb;
    int cpu;
    unsigned long flags = 0;

    for_each_possible_cpu(cpu) {
        tb = &per_cpu(zerosim_trace_buffers, cpu);
        if (cpu == 0) {
            // We save flags for the first lock
            spin_lock_irqsave(&tb->buffer_lock, flags);
        } else {
            spin_lock(&tb->buffer_lock);
        }
    }

    smp_mb();

    return flags;
}

static void release_all_locks(unsigned long flags)
{
    struct trace_buffer * tb;
    unsigned long ncpus = num_possible_cpus();
    int cpu;

    smp_mb();

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
 * Set the size of each per-cpu buffer, allocating memory appropriately. If
 * tracing is currently, it will be turned off and any snapshot discarded.
 */
SYSCALL_DEFINE1(zerosim_trace_size,
                unsigned long, ntrace)
{
    struct trace_buffer * tb;
    int cpu, node;

    // Make sure that nobody is tracing and that we are not copying the buffers.
    unsigned long flags = grab_all_locks();

    if (atomic_read(&hold_buffers)) {
        release_all_locks(flags);
        return -EAGAIN;
    }

    atomic_set(&tracing_enabled, 0);
    atomic_set(&ready, 0);
    release_all_locks(flags);

    // If we get here, we know that nobody is using the buffers or copying them.

    // Allocate new buffers, and free any existing buffers.
    for_each_possible_cpu(cpu) {
        node = cpu_to_node(cpu);

        tb = &per_cpu(zerosim_trace_buffers, cpu);

        if (tb->buf) {
            kfree(tb->buf);
        }

        tb->buf = kmalloc_node(ntrace * sizeof(struct trace),
                               GFP_KERNEL | __GFP_ZERO,
                               node);

        if (tb->buf == NULL) {
            printk(KERN_WARNING "Unable to alloc for zerosim_trace. kmalloc failed.\n");
            return -ENOMEM;
        } else {
            printk(KERN_WARNING "Allocated zerosim_trace buffer for cpu %d\n", cpu);
        }

        tb->len = ntrace;
        tb->next = 0;
    }

    // Atomically set buffer size and enable
    flags = grab_all_locks();
    WRITE_ONCE(trace_buf_size, ntrace);
    atomic_set(&ready, 1);
    release_all_locks(flags);

    return 0;
}

/*
 * Begin tracing on all cores. This should be called once per call to snapshot.
 */
SYSCALL_DEFINE0(zerosim_trace_begin)
{
    unsigned long flags = grab_all_locks();

    if (!atomic_read(&ready)) {
        release_all_locks(flags);
        return -ENOMEM;
    }

    if (atomic_add_unless(&tracing_enabled, 1, 1)) {
        release_all_locks(flags);
        printk(KERN_WARNING "zerosim_trace begin\n");
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
 * len must be at least trace_buf_size * num_cpus() * sizeof(trace) bytes.
 */
SYSCALL_DEFINE2(zerosim_trace_snapshot,
                void*,          user_buf,
                unsigned long,  len)
{
    struct trace_buffer * tb;
    int cpu;
    unsigned long uncopied;
    struct trace *user_buf_offset = (struct trace *) user_buf;

    unsigned long flags = grab_all_locks();

    if (!atomic_add_unless(&tracing_enabled, -1, 0)) {
        release_all_locks(flags);
        return -EBADE; // didn't call begin
    }
    if (!atomic_add_unless(&ready, -1, 0)) {
        release_all_locks(flags);
        return -ENOMEM; // wasn't ready
    }

    // Check that the user buffer is large enough
    if (len < (trace_buf_size * num_possible_cpus() * sizeof(struct trace))) {
        release_all_locks(flags);
        printk(KERN_WARNING "user buffer of size %lu is too small. need %lu * %u * %lu = %llu.\n",
                len, (unsigned long)trace_buf_size, num_possible_cpus(), sizeof(struct trace),
                trace_buf_size * num_possible_cpus() * sizeof(struct trace));
        return -EINVAL;
    }

    // Signal that we shouldn't re-allocate buffers now
    atomic_set(&hold_buffers, 1);

    release_all_locks(flags);

    // If we get here, it means that nobody is using or freeing buffers.

    // We have to release the locks because `copy_to_user` can block.

    // Read to user buff. This may block.
    for_each_possible_cpu(cpu) {
        tb = &per_cpu(zerosim_trace_buffers, cpu);

        // Copy the first part of the buffer...
        uncopied = copy_to_user(user_buf_offset, &tb->buf[tb->next],
                                (tb->len - tb->next) * sizeof(struct trace));
        if (uncopied > 0) {
            printk(KERN_WARNING "unable to copy %lu bytes from cpu %d\n", uncopied, cpu);
        }
        user_buf_offset += tb->len - tb->next;

        // ... and then wrap around to the beginning.
        uncopied = copy_to_user(user_buf_offset, tb->buf,
                                tb->next * sizeof(struct trace));
        if (uncopied > 0) {
            printk(KERN_WARNING "unable to copy %lu bytes from cpu %d\n", uncopied, cpu);
        }
        user_buf_offset += tb->next;
    }

    // Zero all trace buffers
    for_each_possible_cpu(cpu) {
        tb = &per_cpu(zerosim_trace_buffers, cpu);
        memset(tb->buf, 0, trace_buf_size * sizeof(struct trace));
        tb->next = 0;
    }

    flags = grab_all_locks();
    atomic_set(&hold_buffers, 0);
    atomic_set(&ready, 1);
    release_all_locks(flags);

    printk(KERN_WARNING "zerosim_trace snapshot\n");

    return 0;
}

/* Actually add the given event into the trace buffer, potentially overwriting
 * the oldest event in the buffer and updating the tail pointer.
 *
 * The buffer lock _should not_ be held by the caller.
 */
static inline void zerosim_trace_event(struct trace *ev)
{
    struct trace_buffer *buf;
    unsigned long flags;

    // Guard against reads of uninitialized per_cpu
    if (!atomic_read(&ready)) {
        return;
    }

    buf = &per_cpu(zerosim_trace_buffers, smp_processor_id());

    spin_lock_irqsave(&buf->buffer_lock, flags);

    smp_mb();

    // check if tracing is enabled and ready
    if (!atomic_read(&tracing_enabled) || !atomic_read(&ready)) {
        spin_unlock_irqrestore(&buf->buffer_lock, flags);
        return;
    }

    // push to buf
    buf->buf[buf->next] = *ev;
    buf->next = (buf->next + 1) % buf->len;

    spin_unlock_irqrestore(&buf->buffer_lock, flags);
}

void zerosim_trace_task_switch(struct task_struct *prev,
                               struct task_struct *curr)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) curr->pid,
        .flags = ZEROSIM_TRACE_TASK_SWITCH,
        .pid = (u32) curr->pid,
        .extra = (u32) prev->pid,
    };

    zerosim_trace_event(&tr);
}

asmlinkage void zerosim_trace_syscall_start(struct pt_regs *regs)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) regs->orig_ax, // syscall nr
        .flags = ZEROSIM_TRACE_SYSCALL | ZEROSIM_TRACE_START,
        .pid = (u32) current->pid,
        .extra = 0,
    };

    zerosim_trace_event(&tr);
}

asmlinkage void zerosim_trace_syscall_end(u64 syscall_retval, struct pt_regs *regs)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) regs->orig_ax, // syscall nr
        .flags = ZEROSIM_TRACE_SYSCALL,
        .pid = (u32) current->pid,
        .extra = (u32) (syscall_retval & 0xFFFFFFFFul), // truncated return value
    };

    zerosim_trace_event(&tr);
}

void zerosim_trace_interrupt_start(struct pt_regs *regs)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) ~regs->orig_ax, // interrupt nr
        .flags = ZEROSIM_TRACE_INTERRUPT | ZEROSIM_TRACE_START,
        .pid = (u32) current->pid,
        .extra = 0,
    };

    zerosim_trace_event(&tr);
}

void zerosim_trace_interrupt_end(struct pt_regs *regs)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) ~regs->orig_ax, // interrupt nr
        .flags = ZEROSIM_TRACE_INTERRUPT,
        .pid = (u32) current->pid,
        .extra = 0,
    };

    zerosim_trace_event(&tr);
}

dotraplinkage void zerosim_trace_exception_start(struct pt_regs *regs, long error_code)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) error_code,
        .flags = ZEROSIM_TRACE_FAULT | ZEROSIM_TRACE_START,
        .pid = (u32) current->pid,
        .extra = 0,
    };

    zerosim_trace_event(&tr);
}

dotraplinkage void zerosim_trace_exception_end(struct pt_regs *regs, long error_code)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) error_code,
        .flags = ZEROSIM_TRACE_FAULT,
        .pid = (u32) current->pid,
        .extra = (u32) (regs->ip & 0xFFFFFFFFul),
    };

    zerosim_trace_event(&tr);
}

void zerosim_trace_softirq_start(void)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = 0,
        .flags = ZEROSIM_TRACE_SOFTIRQ | ZEROSIM_TRACE_START,
        .pid = (u32) current->pid,
        .extra = 0,
    };

    zerosim_trace_event(&tr);
}

void zerosim_trace_softirq_end(void)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = 0,
        .flags = ZEROSIM_TRACE_SOFTIRQ,
        .pid = (u32) current->pid,
        .extra = 0,
    };

    zerosim_trace_event(&tr);
}

void zerosim_trace_vm_enter(int vcpu_id)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = 0,
        .flags = ZEROSIM_TRACE_VMENTEREXIT | ZEROSIM_TRACE_START,
        .pid = (u32) current->pid,
        .extra = (u32) vcpu_id,
    };

    zerosim_trace_event(&tr);
}
EXPORT_SYMBOL(zerosim_trace_vm_enter);

void zerosim_trace_vm_exit(unsigned long reason, unsigned long qual)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = (u32) (reason & 0xFFFF), // All other bits are 0
        .flags = ZEROSIM_TRACE_VMENTEREXIT,
        .pid = (u32) current->pid,
        .extra = (qual & 0xFFFFFFFFul), // All other bits are 0 for most exits
    };

    zerosim_trace_event(&tr);
}
EXPORT_SYMBOL(zerosim_trace_vm_exit);

void zerosim_trace_vm_delay_begin(int vcpu_id, unsigned long behind)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = vcpu_id,
        .flags = ZEROSIM_TRACE_VMDELAY | ZEROSIM_TRACE_START,
        .pid = (u32) current->pid,
        .extra = behind,
    };

    zerosim_trace_event(&tr);
}
EXPORT_SYMBOL(zerosim_trace_vm_delay_begin);

void zerosim_trace_vm_delay_end(int vcpu_id)
{
    struct trace tr = {
        .timestamp = rdtsc(),
        .id = vcpu_id,
        .flags = ZEROSIM_TRACE_VMDELAY,
        .pid = (u32) current->pid,
        .extra = 0,
    };

    zerosim_trace_event(&tr);
}
EXPORT_SYMBOL(zerosim_trace_vm_delay_end);
