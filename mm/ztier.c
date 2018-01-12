/*
 * ztier.c
 *
 * ztier is an special purpose allocator for storing compressed pages. It is
 * intended to retain the simplicity and determinism of zbud, but with higher
 * density to avoid wasting space.
 *
 * ztier allocates chunks of sizes 2KB, 1KB, or 256KB, based on empirical
 * observations. Each chunk size has its own free list, organized as a rb-tree.
 *
 * ztier has the same interface as zbud and zsmalloc: The ztier API differs
 * from that of conventional allocators in that the allocation function,
 * ztier_alloc(), returns an opaque handle to the user, not a dereferenceable
 * pointer.  The user must map the handle using ztier_map() in order to get a
 * usable pointer by which to access the allocation data and unmap the handle
 * with ztier_unmap() when operations on the allocation data are complete.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/rbtree.h>
#include <linux/ztier.h>
#include <linux/zpool.h>

// Some useful constants
enum TIERS {
    TIER0 = 0,
    TIER1 = 1,
    TIER2 = 2,

    NUM_TIERS = 3,
};

static const unsigned long TIER_SIZES[NUM_TIERS] = {
    [TIER0] = (1<<11), // 2KB
    [TIER1] = (1<<10), // 1KB
    [TIER2] = (1<< 8), // 256B
};

/*****************
 * Structures
*****************/

/**
 * struct ztier_pool - stores metadata for each ztier pool
 */
struct ztier_pool {
    // Lock to protect the tree
    spinlock_t lock;

    // A set of free lists. Each free list is a rbtree to make it easier to
    // find pages that are completely unused.
    struct rb_root free_lists[NUM_TIERS];

    // Keep track of the size of the pool (in bytes)
    unsigned long size;

    // A pointer to a struct of user-defined ops specified at construction.
    const struct ztier_ops *ops;

#ifdef CONFIG_ZPOOL
    struct zpool *zpool;
    const struct zpool_ops *zpool_ops;
#endif
};

/**
 * Metadata for a single allocatable chunk. The size of the chunk is determined
 * by the tree the chunk is in, since all chunks in the same tree have the same
 * size.
 *
 * Instead of allocating separate memory for this structure, the structure is
 * placed at the beginning of a free chunk before it is inserted into the free
 * list. Only free chunks are in lists, so this is ok.
 *
 * Note that this must fit inside of an allocation, so the minimum size of a
 * tier is sizeof(ztier_chunk).
 */
struct ztier_chunk {
    // ztier_chunks are the contents of the rbtrees in ztier_pool
    struct rb_node node;
};

/*****************
 * zpool
 *
 * These implementations are basically copied from zbud.
 ****************/

#ifdef CONFIG_ZPOOL

static int ztier_zpool_evict(struct ztier_pool *pool, unsigned long handle)
{
    if (pool->zpool && pool->zpool_ops && pool->zpool_ops->evict)
        return pool->zpool_ops->evict(pool->zpool, handle);
    else
        return -ENOENT;
}

static const struct ztier_ops ztier_zpool_ops = {
    .evict =    ztier_zpool_evict
};

static void *ztier_zpool_create(const char *name, gfp_t gfp,
                   const struct zpool_ops *zpool_ops,
                   struct zpool *zpool)
{
    struct ztier_pool *pool;

    pool = ztier_create_pool(gfp, zpool_ops ? &ztier_zpool_ops : NULL);
    if (pool) {
        pool->zpool = zpool;
        pool->zpool_ops = zpool_ops;
    }
    return pool;
}

static void ztier_zpool_destroy(void *pool)
{
    ztier_destroy_pool(pool);
}

static int ztier_zpool_malloc(void *pool, size_t size, gfp_t gfp,
            unsigned long *handle)
{
    return ztier_alloc(pool, size, gfp, handle);
}
static void ztier_zpool_free(void *pool, unsigned long handle)
{
    ztier_free(pool, handle);
}

static int ztier_zpool_shrink(void *pool, unsigned int pages,
            unsigned int *reclaimed)
{
    unsigned int total = 0;
    int ret = -EINVAL;

    while (total < pages) {
        ret = ztier_reclaim_page(pool, 8);
        if (ret < 0)
            break;
        total++;
    }

    if (reclaimed)
        *reclaimed = total;

    return ret;
}

static void *ztier_zpool_map(void *pool, unsigned long handle,
            enum zpool_mapmode mm)
{
    return ztier_map(pool, handle);
}
static void ztier_zpool_unmap(void *pool, unsigned long handle)
{
    ztier_unmap(pool, handle);
}

static u64 ztier_zpool_total_size(void *pool)
{
    return ztier_get_pool_size(pool);
}

static struct zpool_driver ztier_zpool_driver = {
    .type =     "ztier",
    .owner =    THIS_MODULE,
    .create =   ztier_zpool_create,
    .destroy =  ztier_zpool_destroy,
    .malloc =   ztier_zpool_malloc,
    .free =     ztier_zpool_free,
    .shrink =   ztier_zpool_shrink,
    .map =      ztier_zpool_map,
    .unmap =    ztier_zpool_unmap,
    .total_size =   ztier_zpool_total_size,
};

MODULE_ALIAS("zpool-ztier");
#endif /* CONFIG_ZPOOL */

/*****************
 * Helpers
*****************/

static void ztier_rb_insert(struct rb_root *tree, struct ztier_chunk *new_chunk)
{
    struct rb_node **link = &tree->rb_node;
    struct rb_node *parent;
    struct ztier_chunk *chunk;

    // Find the right spot
	while (*link)
	{
	    parent = *link;
	    chunk = rb_entry(parent, struct ztier_chunk, node);

	    if (chunk > new_chunk) {
            link = &(*link)->rb_left;
        } else {
            BUG_ON(chunk == new_chunk);
            link = &(*link)->rb_right;
        }
	}

    // Put the new node there
	rb_link_node(&new_chunk->node, parent, link);
	rb_insert_color(&new_chunk->node, tree);
}

static void ztier_init_page(struct rb_root *tree,
                            struct page *page,
                            const int tier)
{
    u8 *raw_page = page_address(page);
    struct ztier_chunk *chunk;
    int i;

    for (i = 0; i < PAGE_SIZE; i += TIER_SIZES[tier]) {
        chunk = (struct ztier_chunk *)&raw_page[i];
        ztier_rb_insert(tree, chunk);
    }
}

/*****************
 * API Functions -- modified from zbud
*****************/
/**
 * ztier_create_pool() - create a new ztier pool
 * @gfp:    gfp flags when allocating the ztier pool structure
 * @ops:    user-defined operations for the ztier pool
 *
 * Return: pointer to the new ztier pool or NULL if the metadata allocation
 * failed.
 */
struct ztier_pool *ztier_create_pool(gfp_t gfp, const struct ztier_ops *ops)
{
    struct ztier_pool *pool;
    int i;

    pool = kzalloc(sizeof(struct ztier_pool), gfp);
    if (!pool)
        return NULL;

    spin_lock_init(&pool->lock);
    for (i = 0; i < NUM_TIERS; i++) {
        pool->free_lists[i] = RB_ROOT;
    }
    pool->size = 0;
    pool->ops = ops;

    return pool;
}

/**
 * ztier_destroy_pool() - destroys an existing ztier pool
 * @pool:   the ztier pool to be destroyed
 *
 * The pool should be emptied before this function is called.
 */
void ztier_destroy_pool(struct ztier_pool *pool)
{
    kfree(pool);
}

/**
 * ztier_alloc() - allocates a region of a given size
 * @pool:   ztier pool from which to allocate
 * @size:   size in bytes of the desired allocation
 * @gfp:    gfp flags used if the pool needs to grow
 * @handle: handle of the new allocation
 *
 * This function will attempt to find a free region in the pool large enough to
 * satisfy the allocation request.  A search of the free lists is performed
 * first. If no suitable free region is found, then a new page is allocated and
 * added to the pool to satisfy the request.
 *
 * gfp should not set __GFP_HIGHMEM as highmem pages cannot be used
 * as ztier pool pages.
 *
 * Return: 0 if success and handle is set, otherwise -EINVAL if the size or
 * gfp arguments are invalid or -ENOMEM if the pool was unable to allocate
 * a new page.
 */
int ztier_alloc(struct ztier_pool *pool, size_t size, gfp_t gfp,
            unsigned long *handle)
{
    struct rb_root *tree;
    struct rb_node *free;
    struct page *page;
    int tier;

    // sanity checks
	if (!size || (gfp & __GFP_HIGHMEM))
		return -EINVAL;
	if (size > TIER_SIZES[TIER0])
		return -ENOSPC;

    // choose the appropriate tree
    for (tier = 0; tier < NUM_TIERS; tier++) {
        if (size <= TIER_SIZES[tier]) {
            tree = &pool->free_lists[tier];
            break;
        }
    }
    BUG_ON(!tree);

    // look in the free list for the first chunk
    free = rb_first(tree);

    // if there is no free chunk, allocate a new page and add it to the pool
    if (!free) {
        // Allocate a new page
        page = alloc_page(gfp);
        if (!page) {
            return -ENOMEM;
        }

        // split into chunks, adding each chunk to the tree
        ztier_init_page(tree, page, tier);

        // then retry removing the first chunk
        free = rb_first(tree);
    }

    BUG_ON(!free);

    // return the allocation
    *handle = (unsigned long)free;
    return 0;
}

/**
 * ztier_free() - frees the allocation associated with the given handle
 * @pool:   pool in which the allocation resided
 * @handle: handle associated with the allocation returned by ztier_alloc()
 *
 * TODO TODO TODO
 *
 * In the case that the ztier page in which the allocation resides is under
 * reclaim, as indicated by the PG_reclaim flag being set, this function
 * only sets the first|last_chunks to 0.  The page is actually freed
 * once both buddies are evicted (see zbud_reclaim_page() below).
 */
void zbud_free(struct zbud_pool *pool, unsigned long handle)
{
    struct zbud_header *zhdr;
    int freechunks;

    spin_lock(&pool->lock);
    zhdr = handle_to_zbud_header(handle);

    /* If first buddy, handle will be page aligned */
    if ((handle - ZHDR_SIZE_ALIGNED) & ~PAGE_MASK)
        zhdr->last_chunks = 0;
    else
        zhdr->first_chunks = 0;

    if (zhdr->under_reclaim) {
        /* zbud page is under reclaim, reclaim will free */
        spin_unlock(&pool->lock);
        return;
    }

    /* Remove from existing buddy list */
    list_del(&zhdr->buddy);

    if (zhdr->first_chunks == 0 && zhdr->last_chunks == 0) {
        /* zbud page is empty, free */
        list_del(&zhdr->lru);
        free_zbud_page(zhdr);
        pool->pages_nr--;
    } else {
        /* Add to unbuddied list */
        freechunks = num_free_chunks(zhdr);
        list_add(&zhdr->buddy, &pool->unbuddied[freechunks]);
    }

    spin_unlock(&pool->lock);
}

#define list_tail_entry(ptr, type, member) \
    list_entry((ptr)->prev, type, member)

/**
 * zbud_reclaim_page() - evicts allocations from a pool page and frees it
 * @pool:   pool from which a page will attempt to be evicted
 * @retires:    number of pages on the LRU list for which eviction will
 *      be attempted before failing
 *
 * zbud reclaim is different from normal system reclaim in that the reclaim is
 * done from the bottom, up.  This is because only the bottom layer, zbud, has
 * information on how the allocations are organized within each zbud page. This
 * has the potential to create interesting locking situations between zbud and
 * the user, however.
 *
 * To avoid these, this is how zbud_reclaim_page() should be called:

 * The user detects a page should be reclaimed and calls zbud_reclaim_page().
 * zbud_reclaim_page() will remove a zbud page from the pool LRU list and call
 * the user-defined eviction handler with the pool and handle as arguments.
 *
 * If the handle can not be evicted, the eviction handler should return
 * non-zero. zbud_reclaim_page() will add the zbud page back to the
 * appropriate list and try the next zbud page on the LRU up to
 * a user defined number of retries.
 *
 * If the handle is successfully evicted, the eviction handler should
 * return 0 _and_ should have called zbud_free() on the handle. zbud_free()
 * contains logic to delay freeing the page if the page is under reclaim,
 * as indicated by the setting of the PG_reclaim flag on the underlying page.
 *
 * If all buddies in the zbud page are successfully evicted, then the
 * zbud page can be freed.
 *
 * Returns: 0 if page is successfully freed, otherwise -EINVAL if there are
 * no pages to evict or an eviction handler is not registered, -EAGAIN if
 * the retry limit was hit.
 */
int zbud_reclaim_page(struct zbud_pool *pool, unsigned int retries)
{
    int i, ret, freechunks;
    struct zbud_header *zhdr;
    unsigned long first_handle = 0, last_handle = 0;

    spin_lock(&pool->lock);
    if (!pool->ops || !pool->ops->evict || list_empty(&pool->lru) ||
            retries == 0) {
        spin_unlock(&pool->lock);
        return -EINVAL;
    }
    for (i = 0; i < retries; i++) {
        zhdr = list_tail_entry(&pool->lru, struct zbud_header, lru);
        list_del(&zhdr->lru);
        list_del(&zhdr->buddy);
        /* Protect zbud page against free */
        zhdr->under_reclaim = true;
        /*
         * We need encode the handles before unlocking, since we can
         * race with free that will set (first|last)_chunks to 0
         */
        first_handle = 0;
        last_handle = 0;
        if (zhdr->first_chunks)
            first_handle = encode_handle(zhdr, FIRST);
        if (zhdr->last_chunks)
            last_handle = encode_handle(zhdr, LAST);
        spin_unlock(&pool->lock);

        /* Issue the eviction callback(s) */
        if (first_handle) {
            ret = pool->ops->evict(pool, first_handle);
            if (ret)
                goto next;
        }
        if (last_handle) {
            ret = pool->ops->evict(pool, last_handle);
            if (ret)
                goto next;
        }
next:
        spin_lock(&pool->lock);
        zhdr->under_reclaim = false;
        if (zhdr->first_chunks == 0 && zhdr->last_chunks == 0) {
            /*
             * Both buddies are now free, free the zbud page and
             * return success.
             */
            free_zbud_page(zhdr);
            pool->pages_nr--;
            spin_unlock(&pool->lock);
            return 0;
        } else if (zhdr->first_chunks == 0 ||
                zhdr->last_chunks == 0) {
            /* add to unbuddied list */
            freechunks = num_free_chunks(zhdr);
            list_add(&zhdr->buddy, &pool->unbuddied[freechunks]);
        } else {
            /* add to buddied list */
            list_add(&zhdr->buddy, &pool->buddied);
        }

        /* add to beginning of LRU */
        list_add(&zhdr->lru, &pool->lru);
    }
    spin_unlock(&pool->lock);
    return -EAGAIN;
}

/**
 * ztier_map() - maps the allocation associated with the given handle
 * @pool:   pool in which the allocation resides
 * @handle: handle associated with the allocation to be mapped
 *
 * Returns: a pointer to the mapped allocation
 */
void *ztier_map(struct ztier_pool *pool, unsigned long handle)
{
    return (void *)(handle);
}

/**
 * ztier_unmap() - maps the allocation associated with the given handle
 * @pool:   pool in which the allocation resides
 * @handle: handle associated with the allocation to be unmapped
 */
void ztier_unmap(struct ztier_pool *pool, unsigned long handle)
{
}

/**
 * ztier_get_pool_size() - gets the zbud pool size in bytes
 * @pool:   pool whose size is being queried
 *
 * Returns: size in bytes of the given pool.
 */
u64 ztier_get_pool_size(struct ztier_pool *pool)
{
    return pool->size;
}

static int __init init_ztier(void)
{
    // Make sure a struct rb_node can fit in the minimal tier size
    BUILD_BUG_ON(sizeof(struct rb_node) > TIER_SIZES[NUM_TIERS-1]);
    pr_info("loaded\n");

#ifdef CONFIG_ZPOOL
    zpool_register_driver(&ztier_zpool_driver);
#endif

    return 0;
}

static void __exit exit_ztier(void)
{
#ifdef CONFIG_ZPOOL
    zpool_unregister_driver(&ztier_zpool_driver);
#endif

    pr_info("unloaded\n");
}

module_init(init_ztier);
module_exit(exit_ztier);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mark Mansi <markm@cs.wisc.edu>");
MODULE_DESCRIPTION("Tiered Allocator for Compressed Pages");

