#ifndef __EAGER_PAGING__
#define __EAGER_PAGING__

#define MAX_PROC_NAME_LEN 16

bool is_eager_paging_process(const char* proc_name);

void eager_paging_init(void);

#endif
