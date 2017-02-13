#include <linux/apriori_paging_alloc.h>
#include <linux/syscalls.h>

/*
 * !!_AprioriPaging_!!
 * This system has two functionalities depending on the value of the option variable
 *
 * if ( option > 0 )
 * It will take the process name as an argument and
 * every process which will be created with
 * this name will use our allocation mechanism
 *
 * if ( option < 0 )
 * It will take the pid as an argument and will force the process that has
 * this pid to use our allocation mechanism
 *
 */

SYSCALL_DEFINE3(apriori_paging_alloc, const char __user**, proc_name, unsigned int, num_procs, int, option)
{
    unsigned int i;
    char *temp;
    char proc[MAX_PROC_NAME_LEN];
    unsigned long ret = 0;

    struct task_struct *tsk;
    unsigned long pid;


    if ( option > 0 ) {
        for ( i = 0 ; i < CONFIG_NR_CPUS ; i++ ) {
            if ( i < num_procs )
                ret = strncpy_from_user(proc, proc_name[i], MAX_PROC_NAME_LEN);
            else
                temp = strncpy(proc,"",MAX_PROC_NAME_LEN);
            temp = strncpy(apriori_paging_process[i], proc, MAX_PROC_NAME_LEN-1);
        }
    }

    if ( option < 0 ) {
        for ( i = 0 ; i < CONFIG_NR_CPUS ; i++ ) {
            if( i < num_procs ) {
                ret = kstrtoul(proc_name[i],10,&pid);
                if ( ret == 0 ) {
                    tsk = find_task_by_vpid(pid);
                    tsk->mm->apriori_paging_en = 1;
                }
            }
        }
    }


    return 0;
}

/*
 * This function checks whether a process name provided matches from the list
 *
 */

int is_process_of_apriori_paging(const char* proc_name)
{

    unsigned int i;

    for ( i = 0 ; i < CONFIG_NR_CPUS ; i++ )     {
        if (!strncmp(proc_name,apriori_paging_process[i],MAX_PROC_NAME_LEN))
            return 1;
    }

    return 0;
}
