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

// This bit is set in the struct page's `private` field when the page
// is going through reclaimation.
static const unsigned long RECLAIM_FLAG = (1ul << 63);

// Everything except the RECLAIM_FLAG
static const unsigned long TIER_MASK = GENMASK(62, 0);

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

    // A set of chunks whose pages are under reclamation. These chunks are
    // not available for allocation.
    struct rb_root under_reclaim;

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

/* Return the least element of the tree that is >= chunk */
static struct rb_node *ztier_rb_ceil(struct rb_root *tree,
                                      struct ztier_chunk *chunk)
{
    struct rb_node *node = tree->rb_node;
    struct ztier_chunk *found;

    while (node) {
        found = rb_entry(node, struct ztier_chunk, node);

        if (found < chunk) {
            node = node->rb_right;
        } else if (found == chunk) {
            return node;
        } else { // node > chunk
            // If node.left < chunk then return node; otherwise,
            // keep going down the tree.
            found = rb_entry(node->rb_left, struct ztier_chunk, node);
            if (found < chunk) {
                return node;
            }

            node = node->rb_left;
        }
    }
    return NULL;
}

/* Return the greatest element of the tree that is <= chunk */
static struct rb_node *ztier_rb_floor(struct rb_root *tree,
                                      struct ztier_chunk *chunk)
{
    struct rb_node *node = tree->rb_node;
    struct ztier_chunk *found;

    while (node) {
        found = rb_entry(node, struct ztier_chunk, node);

        if (found > chunk) {
            node = node->rb_left;
        } else if (found == chunk) {
            return node;
        } else { // node < chunk
            // If node.right > chunk then return node; otherwise,
            // keep going down the tree.
            found = rb_entry(node->rb_right, struct ztier_chunk, node);
            if (found > chunk) {
                return node;
            }

            node = node->rb_right;
        }
    }
    return NULL;
}

/* Return true if chunk is in tree */
static bool ztier_rb_contains(struct rb_root *tree, struct ztier_chunk *chunk)
{
    struct rb_node *floor = ztier_rb_floor(tree, chunk);
    struct ztier_chunk *found = rb_entry(floor, struct ztier_chunk, node);
    return found == chunk;
}

/* Insert the given free new_chunk into the given free list (tree) */
static void ztier_rb_insert(struct rb_root *tree, struct ztier_chunk *new_chunk)
{
    struct rb_node **link = &tree->rb_node;
    struct rb_node *parent = NULL;
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

/* Move from tree `from` to tree `to` all entries >= `start` and < `after`.
 * If `to` == NULL, then the chunks are discarded.
 */
static void ztier_rb_move_range(struct rb_root *from,
                                struct rb_root *to,
                                struct ztier_chunk *start,
                                struct ztier_chunk *after)
{
    struct rb_node *node = ztier_rb_ceil(from, start);
    struct ztier_chunk *chunk;

    while (node) {
        chunk = rb_entry(node, struct ztier_chunk, node);
        if (chunk >= after) return;

        // Remove from old tree and add to new tree
        rb_erase(node, from);
        RB_CLEAR_NODE(node);
        if (to) {
            ztier_rb_insert(to, chunk);
        }

        // Go to next node
        node = rb_next(node);
    }
}

/* Break the given allocated page into a chunks based on the given tier and
 * insert the new chunks into the given free list (tree).
 *
 * Moreover, record what size of chunk the page was broken into using the
 * `private` field of the struct page.
 */
static void ztier_init_page(struct rb_root *tree,
                            struct page *page,
                            const int tier)
{
    u8 *raw_page = page_address(page);
    struct ztier_chunk *chunk;
    int i;

    // Record the size of the chunks of the page and unset the RECLAIM_FLAG
    page->private = tier;

    // Break into chunks and insert to tree
    for (i = 0; i < PAGE_SIZE; i += TIER_SIZES[tier]) {
        chunk = (struct ztier_chunk *)&raw_page[i];
        RB_CLEAR_NODE(&chunk->node);
        ztier_rb_insert(tree, chunk);
    }
}

/* Returns true if all tiers are empty. */
static bool ztier_all_tiers_empty(struct ztier_pool *pool)
{
    int i;

    for (i = 0; i < NUM_TIERS; i++) {
        if (rb_last(&pool->free_lists[i])) {
            return false;
        }
    }

    return true;
}

/* Free all memory in all tiers in the given pool */
static void ztier_free_all(struct ztier_pool *pool) {
    unsigned long chunk, page_start, page_end;
    struct page *page;
    struct rb_node *next;
    int i;

    for (i = 0; i < NUM_TIERS; i++) {
        while ((next = rb_first(&pool->free_lists[i]))) {
            chunk = (unsigned long)rb_entry(next, struct ztier_chunk, node);
            page_start = chunk & PAGE_MASK;
            page_end = chunk + PAGE_SIZE;

            // Remove all chunks
            ztier_rb_move_range(&pool->free_lists[i],
                                NULL,
                                (struct ztier_chunk *)page_start,
                                (struct ztier_chunk *)page_end);

            // Free the page
            page = virt_to_page(page_start);
            page->private = 0;
            __free_page(page);

            // Update size
            BUG_ON(pool->size < PAGE_SIZE);
            pool->size -= PAGE_SIZE;
        }
    }
}

/* Helper for ztier_reclaim_select_page */
static unsigned long ztier_reclaim_select_page_in_tier(struct ztier_pool *pool,
                                                      int *current_tier,
                                                      unsigned long *current_addr)
{
    struct rb_root *tree = &pool->free_lists[*current_tier];
    struct rb_node *chunk = ztier_rb_floor(tree, (struct ztier_chunk *)*current_addr);
    unsigned long addr = 0;

    while (chunk) {
        chunk = rb_prev(chunk);

        if (chunk) {
            addr = (unsigned long)rb_entry(chunk, struct ztier_chunk, node);
        }

        if (addr < ((*current_addr) - PAGE_SIZE)) {
            break;
        }
    }

    return addr;
}

/* Select a page to attempt to reclaim from the given pool. This is a stateful
 * routine that keeps track of what pages it has previously selected via the
 * current_tier and current_addr pointers.
 *
 * @current_tier a pointer to state that says what tier we have gotten to so far.
 * @current_addr a pointer to the latest address in current_tier we have checked.
 *
 * Caller should already hold lock.
 *
 * Returns: the selected page
 */
static struct page *ztier_reclaim_select_page(struct ztier_pool *pool,
                                              int *current_tier,
                                              unsigned long *current_addr)
{
    struct rb_node *chunk_node;
    unsigned long chunk;

    // For each tier starting with *current_tier
    while (*current_tier < NUM_TIERS) {
        // If *current_addr is NULL, then return the last chunk in the tier.
        // If the tier is empty move to the next tier and try again.
        if (!current_addr) {
            chunk_node = rb_last(&pool->free_lists[*current_tier]);
            if (!chunk_node) {
                (*current_tier)++;
                continue;
            }
            chunk = (unsigned long)rb_entry(chunk_node, struct ztier_chunk, node);
        }
        // Otherwise, find the first chunk in the same page as current
        // *current_addr. Move backward in the tree until either we reach the
        // beginning of the free list or we reach a chunk in a different page.
        else {
            chunk = ztier_reclaim_select_page_in_tier(pool, current_tier, current_addr);
        }

        // If we reach the beginning of the free list, move to the next tier
        // and try again.
        if (!chunk) {
            (*current_tier)++;
            continue;
        }

        // Otherwise, return the found page.
        return virt_to_page((*current_addr) = chunk & PAGE_MASK);
    }

    // Nothing was found...
    return NULL;
}

/*
 * Move all free chunks of the given page from the free lists to under_reclaim
 *
 * Caller should already hold lock.
 */
static void ztier_page_chunks_under_reclaim(struct ztier_pool *pool,
                                            struct page *page)
{
    int tier = page->private & TIER_MASK;
    unsigned long vaddr = (unsigned long)page_address(page);

    ztier_rb_move_range(&pool->free_lists[tier],
                        &pool->under_reclaim,
                        (struct ztier_chunk *)vaddr,
                        (struct ztier_chunk *)vaddr + PAGE_SIZE);
}

/*
 * Move all free chunks of the given page from the under_reclaim back to the
 * appropriate free list.
 *
 * Caller should already hold lock.
 */
static void ztier_page_chunks_from_under_reclaim(struct ztier_pool *pool,
                                                 struct page *page)
{
    int tier = page->private & TIER_MASK;
    unsigned long vaddr = (unsigned long)page_address(page);

    ztier_rb_move_range(&pool->under_reclaim,
                        &pool->free_lists[tier],
                        (struct ztier_chunk *)vaddr,
                        (struct ztier_chunk *)vaddr + PAGE_SIZE);
}

/*
 * For each chunk of the given page not in the under_reclaim set, attempt to
 * evict the chunk.
 *
 * Caller should NOT hold lock.
 */
static void ztier_attempt_evict_page_chunks(struct ztier_pool *pool,
                                            struct page *page)
{
    int tier = page->private & TIER_MASK;
    unsigned long vaddr = (unsigned long)page_address(page);
    unsigned long handle;
    int i;
    int ret;

    spin_lock(&pool->lock);

    // For each chunk in the page
    for (i = 0; i < PAGE_SIZE; i += TIER_SIZES[tier]) {
        handle = (vaddr + i);

        // If the chunk is not already under_reclaim
        if (!ztier_rb_contains(&pool->under_reclaim, (struct ztier_chunk *) handle))
        {
            spin_unlock(&pool->lock);

            // Attempt to evict it
            ret = pool->ops->evict(pool, handle);
            if (ret) {
                return;
            }

            spin_lock(&pool->lock);
        }
    }

    spin_unlock(&pool->lock);
}

/*
 * If all chunks of the selected page are now in under_reclaim, remove the
 * chunks from under_reclaim, free the page, and return 0. Otherwise, return 1
 * and do nothing.
 *
 * Caller should already hold lock.
 */
static bool ztier_page_chunks_reclaimed(struct ztier_pool *pool,
                                        struct page *page)
{
    int tier = page->private & TIER_MASK;
    unsigned long vaddr = (unsigned long)page_address(page);
    int i;

    // for each chunk in the page, if that chunk is not in under_reclaim,
    // return false. Otherwise, proceed.
    for (i = 0; i < PAGE_SIZE; i += TIER_SIZES[tier]) {
        if (!ztier_rb_contains(&pool->free_lists[tier],
                               (struct ztier_chunk *) (vaddr + i)))
        {
            return 1;
        }
    }

    // Remove all of the chunks of the given page from under_reclaim
    ztier_rb_move_range(&pool->under_reclaim,
                        NULL,
                        (struct ztier_chunk *)vaddr,
                        (struct ztier_chunk *)vaddr + PAGE_SIZE);

    // Free the page
    page->private = 0;
    __free_page(page);

    // Update size
    BUG_ON(pool->size < PAGE_SIZE);
    pool->size -= PAGE_SIZE;

    return 0;
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
    pool->under_reclaim = RB_ROOT;
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
    // Sanity: there should be no pages under reclaim
    BUG_ON(rb_first(&pool->under_reclaim));

    // Free any remaining pages in the pool. It is safe to do so because the
    // pool is empty. This means that all chunks that were allocated have been
    // returned, and all pages that were allocated can be found from the free
    // lists.
    ztier_free_all(pool);

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

    spin_lock(&pool->lock);

    // look in the free list for the first chunk
    free = rb_first(tree);

    // if there is no free chunk, allocate a new page and add it to the pool
    if (!free) {
        // Allocate a new page
        page = alloc_page(gfp);
        if (!page) {
            spin_unlock(&pool->lock);
            return -ENOMEM;
        }

        // split into chunks, adding each chunk to the tree
        ztier_init_page(tree, page, tier);

        // Update size
        pool->size += PAGE_SIZE;

        // then retry removing the first chunk
        free = rb_first(tree);
    }

    rb_erase(free, tree);
    RB_CLEAR_NODE(free);

    BUG_ON(!free);

    spin_unlock(&pool->lock);

    // return the allocation
    *handle = (unsigned long)free;
    return 0;
}

/**
 * ztier_free() - frees the allocation associated with the given handle
 * @pool:   pool in which the allocation resided
 * @handle: handle associated with the allocation returned by ztier_alloc()
 *
 * This does not actually deallocate any memory. It simply puts the chunk
 * back into the free list. Memory is only given back to the kernel allocator
 * when `ztier_reclaim_page` or `ztier_destroy` are called. This is different
 * from the zbud allocator, which uses the `under_reclaim` flag. ztier
 * implements `free` this way to avoid keeping track of non-free chunks, which
 * saves space and is faster/simpler.
 *
 * One caveat: if the RECLAIM_FLAG bit is set in the `private` field of the
 * chunk's struct page, then the page is placed into the `under_reclaim` tree
 * rather than the free list.
 */
void ztier_free(struct ztier_pool *pool, unsigned long handle)
{
    // NOTE: The size of the chunks is stored in the `private` field of the
    // struct page, which can be gotten from the address of the page (which is
    // the handle).

    // Get the struct page of the handle so we can find out how large the
    // allocation is.
    struct page *page = virt_to_page((void *)(handle & PAGE_MASK));
    struct ztier_chunk *chunk = (struct ztier_chunk *)handle;
    int tier = page->private & TIER_MASK;
    bool is_reclaim = !!(page->private & RECLAIM_FLAG);

    // Sanity check: make sure the alignment of the given handle makes sense
    BUG_ON(handle % TIER_SIZES[tier] != 0);

    RB_CLEAR_NODE(&chunk->node);

    spin_lock(&pool->lock);

    // Insert into free list or under_reclaim
    if (is_reclaim) {
        ztier_rb_insert(&pool->under_reclaim, chunk);
    } else {
        ztier_rb_insert(&pool->free_lists[tier], chunk);
    }

    spin_unlock(&pool->lock);
}

/**
 * ztier_reclaim_page() - evicts allocations from a pool page and frees it
 * @pool:   pool from which a page will attempt to be evicted
 * @retires:    number of pages on the LRU list for which eviction will
 *      be attempted before failing
 *
 * Reclaim is done the same way as zbud. The comment is reproduced (with
 * appropriate modifications) here for posterity.
 *
 * ztier reclaim is different from normal system reclaim in that the reclaim is
 * done from the bottom, up. This is because only the bottom layer, ztier, has
 * information on how the allocations are organized within each page. This
 * has the potential to create interesting locking situations between ztier and
 * the user, however.
 *
 * To avoid these, this is how ztier_reclaim_page() should be called:

 * The user detects a page should be reclaimed and calls ztier_reclaim_page().
 * ztier_reclaim_page() will choose a page from the end of the free list, which
 * is presumably where less used chunks will end up since allocations come from
 * the beginning of the list. ztier then tries to free the page containing this
 * free allocations. If all of the chunks in the page are also in the free
 * list, then reclaiming the page is easy. Otherwise, we go through the
 * eviction protocol laid out below. Note that we can always know what page a
 * chunk is in and what other chunks should be in the page because we know
 * which free list the chunk is in.
 *
 * NOTE: we start with larger tiers (e.g 2KB as opposed to 256B because this
 * means trying to evict fewer pages -> less I/O).
 *
 * To evict an allocation, the user-defined eviction handler is called with the
 * pool and handle as arguments.
 *
 * If the handle can not be evicted, the eviction handler should return
 * non-zero. ztier_reclaim_page() will try the next page in the tree that it
 * has not already tried up to a user defined number of retries.
 *
 * NOTE: eviction should not be attempted on the same page concurrently, so
 * we need a way to ensure this never happens.
 *
 * If the handle is successfully evicted, the eviction handler should
 * return 0 _and_ should have called ztier_free() on the handle. ztier_free
 * will not put the page back into the free lists because ztier_reclaim_page
 * sets the RECLAIM_FLAG bit. Instead, ztier_free places the chunks into the
 * `under_reclaim` tree in the pool. ztier_reclaim_page is responsible for
 * making sure the chunks go back to the appropriate free list in the case
 * of failure. This avoids the annoying livelock where a page is continually
 * allocated and freed over and over again.
 *
 * If all chunks in the page are successfully evicted, then the page can be
 * returned to the kernel.
 *
 * NOTE: Reclaim is the only way to free pages from the pool. This means that
 * ztier_destroy needs to free any remaining pages in the pool. It is safe to
 * do so because any allocated paged would have had `free` called on them
 * according to the invariants of `destroy`.
 *
 * Returns: 0 if page is successfully freed, otherwise -EINVAL if there are
 * no pages to evict or an eviction handler is not registered, -EAGAIN if
 * the retry limit was hit.
 */
int ztier_reclaim_page(struct ztier_pool *pool, unsigned int retries)
{
    struct page *page;

    // Keep track of the last tier and chunk address tried
    int current_tier = 0;
    unsigned long current_addr = (unsigned long)-1l;

    spin_lock(&pool->lock);

    if (!pool->ops ||
        !pool->ops->evict ||
        ztier_all_tiers_empty(pool) ||
        retries == 0)
    {
        spin_unlock(&pool->lock);
        return -EINVAL;
    }

    while (retries-- > 0) {
        // Select a victim page by taking the last chunk from the largest tier
        // with pages.
        //
        // We start with larger tiers (e.g 2KB as opposed to 256B because this
        // means trying to evict fewer pages -> less I/O).
        page = ztier_reclaim_select_page(pool, &current_tier, &current_addr);
        if (!page) {
            spin_unlock(&pool->lock);
            return -EAGAIN;
        }

        // set the RECLAIM_FLAG on the page
        page->private |= RECLAIM_FLAG;

        // move all free chunks of the page from the free list to under_reclaim
        ztier_page_chunks_under_reclaim(pool, page);

        spin_unlock(&pool->lock);

        // for each chunk of the page not in the under_reclaim set, attempt an
        // eviction.
        ztier_attempt_evict_page_chunks(pool, page);

        spin_lock(&pool->lock);

        // if all chunks of the selected page are now in under_reclaim, remove
        // the chunks from under_reclaim, free the page, and return sucess
        if (ztier_page_chunks_reclaimed(pool, page)) {
            spin_unlock(&pool->lock);
            return 0;
        }

        // otherwise, replace all of the chunks from that page to the
        // appropriate free list again.
        ztier_page_chunks_from_under_reclaim(pool, page);
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

