#include<linux/types.h>
#include<linux/utsname.h>
#include<linux/syscalls.h>

/*int order_count_en;
pid_t order_count_process;
extern unsigned long profile_hist_alloc_order[MAX_ORDER];
*/
SYSCALL_DEFINE2(order_count, pid_t, pid, int, condition)
{
/*    int i;

    if ( condition > 0 ) {
        order_count_en = 1;
        order_count_process = pid;

        for ( i = 0 ; i < MAX_ORDER ; i++ )
            profile_hist_alloc_order[i] = 0;
    }

    if ( condition <= 0 ) {
        order_count_en = 0;
    }
*/
    return 0;
}

