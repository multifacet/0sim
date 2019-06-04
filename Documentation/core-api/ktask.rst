.. SPDX-License-Identifier: GPL-2.0+

============================================
ktask: parallelize CPU-intensive kernel work
============================================

:Date: November, 2018
:Author: Daniel Jordan <daniel.m.jordan@oracle.com>


Introduction
============

ktask is a generic framework for parallelizing CPU-intensive work in the
kernel.  The intended use is for big machines that can use their CPU power to
speed up large tasks that can't otherwise be multithreaded in userland.  The
API is generic enough to add concurrency to many different kinds of tasks--for
example, page clearing over an address range or freeing a list of pages--and
aims to save its clients the trouble of splitting up the work, choosing the
number of helper threads to use, maintaining an efficient concurrency level,
starting these threads, and load balancing the work between them.


Motivation
==========

A single CPU can spend an excessive amount of time in the kernel operating on
large amounts of data.  Often these situations arise during initialization- and
destruction-related tasks, where the data involved scales with system size.
These long-running jobs can slow startup and shutdown of applications and the
system itself while extra CPUs sit idle.

To ensure that applications and the kernel continue to perform well as core
counts and memory sizes increase, the kernel harnesses these idle CPUs to
complete such jobs more quickly.

For example, when booting a large NUMA machine, ktask uses additional CPUs that
would otherwise be idle until the machine is fully up to avoid a needless
bottleneck during system boot and allow the kernel to take advantage of unused
memory bandwidth.  Similarly, when starting a large VM using VFIO, ktask takes
advantage of the VM's idle CPUs during VFIO page pinning rather than have the
VM's boot blocked on one thread doing all the work.

ktask is not a substitute for single-threaded optimization.  However, there is
a point where a single CPU hits a wall despite performance tuning, so
parallelize!


Concept
=======

ktask is built on unbound workqueues to take advantage of the thread management
facilities it provides: creation, destruction, flushing, priority setting, and
NUMA affinity.

A little terminology up front:  A 'task' is the total work there is to do and a
'chunk' is a unit of work given to a thread.

To complete a task using the ktask framework, a client provides a thread
function that is responsible for completing one chunk.  The thread function is
defined in a standard way, with start and end arguments that delimit the chunk
as well as an argument that the client uses to pass data specific to the task.

In addition, the client supplies an object representing the start of the task
and an iterator function that knows how to advance some number of units in the
task to yield another object representing the new task position.  The framework
uses the start object and iterator internally to divide the task into chunks.

Finally, the client passes the total task size and a minimum chunk size to
indicate the minimum amount of work that's appropriate to do in one chunk.  The
sizes are given in task-specific units (e.g. pages, inodes, bytes).  The
framework uses these sizes, along with the number of online CPUs and an
internal maximum number of threads, to decide how many threads to start and how
many chunks to divide the task into.

For example, consider the task of clearing a gigantic page.  This used to be
done in a single thread with a for loop that calls a page clearing function for
each constituent base page.  To parallelize with ktask, the client first moves
the for loop to the thread function, adapting it to operate on the range passed
to the function.  In this simple case, the thread function's start and end
arguments are just addresses delimiting the portion of the gigantic page to
clear.  Then, where the for loop used to be, the client calls into ktask with
the start address of the gigantic page, the total size of the gigantic page,
and the thread function.  Internally, ktask will divide the address range into
an appropriate number of chunks and start an appropriate number of threads to
complete these chunks.


Configuration
=============

To use ktask, configure the kernel with CONFIG_KTASK=y.

If CONFIG_KTASK=n, calls to the ktask API are simply #define'd to run the
thread function that the client provides so that the task is completed without
concurrency in the current thread.


Interface
=========

.. kernel-doc:: include/linux/ktask.h


Resource Limits
===============

ktask has resource limits on the number of work items it sends to workqueue.
In ktask, a workqueue item is a thread that runs chunks of the task until the
task is finished.

These limits support the different ways ktask uses workqueues:
 - ktask_run to run threads on the calling thread's node.
 - ktask_run_numa to run threads on the node(s) specified.
 - ktask_run_numa with nid=NUMA_NO_NODE to run threads on any node in the
   system.

To support these different ways of queueing work while maintaining an efficient
concurrency level, we need both system-wide and per-node limits on the number
of threads.  Without per-node limits, a node might become oversubscribed
despite ktask staying within the system-wide limit, and without a system-wide
limit, we can't properly account for work that can run on any node.

The system-wide limit is based on the total number of CPUs, and the per-node
limit on the CPU count for each node.  A per-node work item counts against the
system-wide limit.  Workqueue's max_active can't accommodate both types of
limit, no matter how many workqueues are used, so ktask implements its own.

If a per-node limit is reached, the work item is allowed to run anywhere on the
machine to avoid overwhelming the node.  If the global limit is also reached,
ktask won't queue additional work items until we fall below the limit again.

These limits apply only to workqueue items--that is, helper threads beyond the
one starting the task.  That way, one thread per task is always allowed to run.


Scheduler Interaction
=====================

Even within the resource limits, ktask must take care to run a number of
threads appropriate for the system's current CPU load.  Under high CPU usage,
starting excessive helper threads may disturb other tasks, unfairly taking CPU
time away from them for the sake of an optimized kernel code path.

ktask plays nicely in this case by setting helper threads to the lowest
scheduling priority on the system (MAX_NICE).  This way, helpers' CPU time is
appropriately throttled on a busy system and other tasks are not disturbed.

The main thread initiating the task remains at its original priority so that it
still makes progress on a busy system.

It is possible for a helper thread to start running and then be forced off-CPU
by a higher priority thread.  With the helper's CPU time curtailed by MAX_NICE,
the main thread may wait longer for the task to finish than it would have had
it not started any helpers, so to ensure forward progress at a single-threaded
pace, once the main thread is finished with all outstanding work in the task,
the main thread wills its priority to one helper thread at a time.  At least
one thread will then always be running at the priority of the calling thread.


Cgroup Awareness
================

Given the potentially large amount of CPU time ktask threads may consume, they
should be aware of the cgroup of the task that called into ktask and
appropriately throttled.

TODO: Implement cgroup-awareness in unbound workqueues.


Power Management
================

Starting additional helper threads may cause the system to consume more energy,
which is undesirable on energy-conscious devices.  Therefore ktask needs to be
aware of cpufreq policies and scaling governors.

If an energy-conscious policy is in use (e.g. powersave, conservative) on any
part of the system, that is a signal that the user has strong power management
preferences, in which case ktask is disabled.

TODO: Implement this.


Backward Compatibility
======================

ktask is written so that existing calls to the API will be backwards compatible
should the API gain new features in the future.  This is accomplished by
restricting API changes to members of struct ktask_ctl and having clients make
an opaque initialization call (DEFINE_KTASK_CTL).  This initialization can then
be modified to include any new arguments so that existing call sites stay the
same.


Error Handling
==============

Calls to ktask fail only if the provided thread function fails.  In particular,
ktask avoids allocating memory internally during a task, so it's safe to use in
sensitive contexts.

Tasks can fail midway through their work.  To recover, the finished chunks of
work need to be undone in a task-specific way, so ktask allows clients to pass
an "undo" callback that is responsible for undoing one chunk of work.  To avoid
multiple levels of error handling, this "undo" callback should not be allowed
to fail.  For simplicity and because it's a slow path, undoing is not
multithreaded.

Each call to ktask_run and ktask_run_numa returns a single value,
KTASK_RETURN_SUCCESS or a client-specific value.  Since threads can fail for
different reasons, however, ktask may need the ability to return
thread-specific error information.  This can be added later if needed.
