#ifndef _ZTIER_H_
#define _ZTIER_H_

#include <linux/types.h>

struct ztier_pool;

struct ztier_ops {
	int (*evict)(struct ztier_pool *pool, unsigned long handle);
};

struct ztier_pool *ztier_create_pool(gfp_t gfp, const struct ztier_ops *ops);
void ztier_destroy_pool(struct ztier_pool *pool);
int ztier_alloc(struct ztier_pool *pool, size_t size, gfp_t gfp,
	unsigned long *handle);
void ztier_free(struct ztier_pool *pool, unsigned long handle);
int ztier_reclaim_page(struct ztier_pool *pool, unsigned int retries);
void *ztier_map(struct ztier_pool *pool, unsigned long handle);
void ztier_unmap(struct ztier_pool *pool, unsigned long handle);
u64 ztier_get_pool_size(struct ztier_pool *pool);

#endif /* _ZTIER_H_ */
