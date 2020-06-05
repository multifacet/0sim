/*
 * The `eager_paging` system call.
 */

#include <linux/types.h>
#include <linux/syscalls.h>
#include <linux/eager_paging.h>

// The number of different commands (actually, command prefixes) that can be made eager.
#define MAX_EAGER 16

// This string is a prefix of a command. If a process started later has one of these
// strings as a prefix of its command string, it will be started with eager
// paging, along with all of its children.
static char eager_paging_comm[MAX_EAGER][MAX_PROC_NAME_LEN];

// The next unset prefix slot.
static int next;

SYSCALL_DEFINE1(eager_paging, const char __user*, proc_name)
{
	int error;

	if (next >= MAX_EAGER) {
		return -ENOSPC;
	}

	error = strncpy_from_user(eager_paging_comm[next], proc_name, MAX_PROC_NAME_LEN);
	if (error < 0) {
		pr_err("eager_paging failed: error %d\n", error);
		return -EFAULT;
	}

	pr_info("eager_paging(%s) set.\n", eager_paging_comm[next]);

	++next;

	return 0;
}

static bool check_match(int i, const char *proc_name)
{
	size_t len;
	bool eq;

	BUG_ON(i >= next);

	// Compute the len first, this makes it a prefix check rather than an
	// exact string check.
	len = strnlen(eager_paging_comm[i], MAX_PROC_NAME_LEN);
	eq = strncmp(eager_paging_comm[i], proc_name, len) == 0;

	return eq;
}

/*
 * This function checks whether a process name provided matches from the list
 *
 */
bool is_eager_paging_process(const char* proc_name)
{
	int i;
	for (i = 0; i < next; ++i) {
		if (check_match(i, proc_name)) {
			return true;
		}
	}

	return false;
}

/* Initialize the globals */
void eager_paging_init(void)
{
	memset(eager_paging_comm, 0, MAX_EAGER * MAX_PROC_NAME_LEN);
	next = 0;
}
