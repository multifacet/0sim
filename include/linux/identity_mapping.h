#ifndef __IDENTITY_MAPPING_H__
#define __IDENTITY_MAPPING_H__

#define MAX_PROC_NAME_LEN 16

char identity_mapping_process[CONFIG_NR_CPUS][MAX_PROC_NAME_LEN];
int start_tracking = 0;
int is_process_of_identity_mapping(const char* proc_name);

#endif
