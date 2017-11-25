/*
 * sballoc.c
 *
 * A simple allocator that only allocates 1B at a time for use as a zpool!
 * sballoc stands for 'single-byte allocator'.
 *
 * Of course, zswap will also write a zswap header word with each piece of
 * data, so actually we are building an allocator of 9B allocations.
 *
 * Currently, this is done by having a 448 9B allocations in each raw page. The
 * remaining space is used for 448 "used" bits, which indicate if the
 * corresponding allocation is used.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/zpool.h>

///////////////////////////////////////////////////////////////////////////////
// Data structures
///////////////////////////////////////////////////////////////////////////////

// The number of allocations per page
#define SBALLOC_PER_PAGE_ALLOCS (PAGE_SIZE * 8 / (8 * 9 + 1))

// The offset into the page of the start of the bitmap
#define SBALLOC_BITMAP_START (SBALLOC_PER_PAGE_ALLOCS * 9)

// The size (in bytes) of the bitmap (for convenience round down to the nearest
// whole byte). This may have the effect of making a few allocations unusable
// for some page sizes. (It doesn't for 4K pages).
#define SBALLOC_BITMAP_BYTES (SBALLOC_PER_PAGE_ALLOCS / 8)

// Get the address of the start of the bitmap from the address of the raw page.
#define SBALLOC_BITMAP_ADDR(page) (&(((char *)(page))[SBALLOC_BITMAP_START]))

// Get the address of the allocation from its index.
#define SBALLOC_IDX_TO_ENTRY(page, idx) (&(((struct entry *)(page))[idx]))

// Get the index of the allocation from its address.
#define SBALLOC_ENTRY_TO_IDX(entry) ((((unsigned long)(entry)) % PAGE_SIZE) / 9)

// Get the address of the raw page an entry is on.
#define SBALLOC_ENTRY_PAGE(entry) ((void *)(((unsigned long)(entry)) & PAGE_MASK))

// Get the address of the heuristic counter
#define SBALLOC_HEURISTIC_ADDR(page) ((unsigned long *)&(SBALLOC_BITMAP_ADDR(page) \
                                                        [SBALLOC_BITMAP_BYTES]))

/*
 * Make a linked list of allocated pages.
 *
 * Each page has the following format:
 * - The first 4032B consist of 448 9B allocations.
 * - Starting from byte 4032, the next 56B consist of used bits.
 * - Last 8B used to keep a rough heuristic for where the next free allocation is.
 */
struct sballoc_page {
    struct list_head list;
    struct page *page;
};

/*
 * The allocator's state
 */
struct sballoc_pool {
    // Guards the metadata.
    //
    // Make sure to grab this lock before editing metadata.
    spinlock_t lock;

    // A list of raw pages grabbed from the page frame allocator
    struct list_head pages;

    // The total number of raw physical pages used for this allocator.
    u64 nr_pages;
};

/*
 * The format of a single allocation (a 9B struct).
 */
struct entry {
    u64 data1;
    u8  data2;
} __attribute__((packed));

///////////////////////////////////////////////////////////////////////////////
// Utitlities for the pool impl
///////////////////////////////////////////////////////////////////////////////

/*
 * Find a free allocation in the given page and return it.
 *
 * NOTE: the caller should already be holding the lock.
 */
static struct entry *sballoc_find_free_in_page(struct page *page)
{
    void *page_raw = page_address(page);
    char *bitmap = SBALLOC_BITMAP_ADDR(page_raw);

    unsigned long *heuristic = SBALLOC_HEURISTIC_ADDR(page_raw);

    int i;

    BUG_ON(!page_raw);

    // Start from the heuristic address... It is possible that we waste
    // some space by doing this (e.g. we miss allocations because of it).
    // We will have to quantify how bad this is...
    //
    // Empirically, on some basic microbenchmarks, this halves the time
    // spent in this function, but doubles space usage. Also, once the
    // space becomes fragmented, we start spending a lot of time here again.
    for (i = *heuristic / 8; i < SBALLOC_BITMAP_BYTES; i++) {
        if (bitmap[i] < 0xFF) {
            // Check which allocation is free
            if (!(bitmap[i] & 0x01)) {
                 return SBALLOC_IDX_TO_ENTRY(page_raw, i * 8 + 0);
            }
            if (!(bitmap[i] & 0x02)) {
                 return SBALLOC_IDX_TO_ENTRY(page_raw, i * 8 + 1);
            }
            if (!(bitmap[i] & 0x04)) {
                 return SBALLOC_IDX_TO_ENTRY(page_raw, i * 8 + 2);
            }
            if (!(bitmap[i] & 0x08)) {
                 return SBALLOC_IDX_TO_ENTRY(page_raw, i * 8 + 3);
            }
            if (!(bitmap[i] & 0x10)) {
                 return SBALLOC_IDX_TO_ENTRY(page_raw, i * 8 + 4);
            }
            if (!(bitmap[i] & 0x20)) {
                 return SBALLOC_IDX_TO_ENTRY(page_raw, i * 8 + 5);
            }
            if (!(bitmap[i] & 0x40)) {
                 return SBALLOC_IDX_TO_ENTRY(page_raw, i * 8 + 6);
            }
            if (!(bitmap[i] & 0x80)) {
                 return SBALLOC_IDX_TO_ENTRY(page_raw, i * 8 + 7);
            }
        }
    }

    return NULL;
}

/*
 * Find a free allocation and return it.
 *
 * NOTE: the caller should already be holding the lock.
 */
static struct entry *sballoc_find_free(struct sballoc_pool *pool)
{
    struct list_head *pos;
    struct entry *found = NULL;

    // For each page in the LRU list, search that page for free allocations
    //
    // TODO: It is likely for free pages to be at the beginning of the list,
    // rather than at the ends, so scanning later pages could just waste time.
    // On the other hand, this is a relatively rare event, so maybe it doesn't
    // matter that much?
    list_for_each(pos, &pool->pages) {
        struct sballoc_page *sbpage = list_entry(pos, struct sballoc_page, list);
        found = sballoc_find_free_in_page(sbpage->page);

        // If found something, exit
        if (found) {
            break;
        }
    }

    return found;
}

/*
 * Mark the given entry used.
 *
 * NOTE: the caller should already be holding the lock.
 */
static void mark_used(struct entry* entry)
{
    unsigned long idx = SBALLOC_ENTRY_TO_IDX(entry);
    void *page_raw = SBALLOC_ENTRY_PAGE(entry);

    // Compute the bit and byte in the bitmap
    int byte = idx / 8;
    int bit  = idx % 8;

    unsigned long *heuristic = SBALLOC_HEURISTIC_ADDR(page_raw);

    if (!page_raw) {
        printk(KERN_ERR "null page_raw. entry=%p. idx=%lu\n", entry, idx);
        BUG();
    }

    // Set the bit
    SBALLOC_BITMAP_ADDR(page_raw)[byte] |= 1 << bit;

    // Update the heuristic
    *heuristic += 1;
}

/*
 * Mark the given entry free.
 *
 * NOTE: the caller should already be holding the lock.
 */
static void mark_free(struct entry* entry)
{
    unsigned long idx = SBALLOC_ENTRY_TO_IDX(entry);
    void *page_raw = SBALLOC_ENTRY_PAGE(entry);

    // Compute the bit and byte in the bitmap
    int byte = idx / 8;
    int bit  = idx % 8;

    unsigned long *heuristic = SBALLOC_HEURISTIC_ADDR(page_raw);

    BUG_ON(!page_raw);

    // Clear the bit
    SBALLOC_BITMAP_ADDR(page_raw)[byte] &= ~(1 << bit);

    // Try not to waste to much space. I am not sure how well this will work,
    // but the gist is that if there is a page with some free allocations that
    // is otherwise full, we will reset the counter...
    if (*heuristic == SBALLOC_PER_PAGE_ALLOCS) {
        // TODO: this seems to decrease performance a lot when the space
        // becomes fragmented. By commenting out this line, we will waste a
        // whole lot of space, but possibly improve performance.
        *heuristic = 0;
    }
}

///////////////////////////////////////////////////////////////////////////////
// zpool interface implementation
///////////////////////////////////////////////////////////////////////////////

/**
 * Create a new sballoc pool
 */
static void *sballoc_zpool_create(
        const char *name,
        gfp_t gfp,
        const struct zpool_ops *zpool_ops,
        struct zpool *zpool)
{
    // Allocate
    struct sballoc_pool *pool = kzalloc(sizeof(*pool), gfp);
    if (!pool) {
        return NULL;
    }

    // Init
    spin_lock_init(&pool->lock);
    INIT_LIST_HEAD(&pool->pages);
    pool->nr_pages = 0;

    pr_debug("Created sballoc_pool %p\n", pool);

    return pool;
}

/*
 * Free the memory occupied by the zpool object.
 *
 * The pool should be freed before calling this!
 */
static void sballoc_zpool_destroy(void *pool)
{
    pr_debug("Destroy sballoc_pool %p\n", pool);

    kfree(pool);
}

/*
 * Allocate 1B from the given pool and return a handle through the `handle`
 * argument.
 *
 * gfp should not set __GFP_HIGHMEM as highmem pages cannot be used
 * as zbud pool pages.
 *
 * If more than 1B is requested, -ENOMEM is returned. If 0B is requested,
 * -EINVAL is returned.
 */
static int sballoc_zpool_malloc(
        void *zpool,
        size_t size,
        gfp_t gfp,
        unsigned long *handle)
{
    struct sballoc_pool *pool = zpool;
    struct entry *entry;
    struct page *page;
    struct sballoc_page *sbpage;

    pr_debug("ALLOC sballoc_pool %p %lx %x %p\n", pool, size, gfp, handle);

    // Check for invalid sizes
    if (size == 0) {
        return -EINVAL;
    }

    if (size > 9) {
        pr_debug("ALLOC REJECT SIZE=%lu\n", size);
        return -ENOMEM;
    }

	spin_lock(&pool->lock);

    // Try to find an existing free slot
    entry = sballoc_find_free(pool);

    // If failed, allocate a new page
    if (!entry) {
        void *page_raw;

        spin_unlock(&pool->lock);
        page = alloc_page(gfp);
        if (!page) {
            return -ENOMEM;
        }
        sbpage = kzalloc(sizeof(struct sballoc_page), gfp);
        if (!sbpage) {
            return -ENOMEM;
        }
        spin_lock(&pool->lock);

        // Zero the page
        page_raw = page_address(page);
        clear_page(page_raw);

        // Add the page to the allocator
        sbpage->page = page;
        list_add(&sbpage->list, &pool->pages);
        pool->nr_pages++;

        // Get a free allocation
        entry = sballoc_find_free_in_page(page);

        BUG_ON(!entry);
    }

    // At this point, we have a free entry, so we can complete the allocation.
    mark_used(entry);
    *handle = (unsigned long)entry;

	spin_unlock(&pool->lock);

	return 0;
}

/*
 * Free the given allocation.
 */
static void sballoc_zpool_free(void *zpool, unsigned long handle)
{
    struct sballoc_pool *pool = zpool;
    struct entry *entry;

    pr_debug("FREE sballoc_pool %p %lx\n", pool, handle);

	spin_lock(&pool->lock);

    // Convert handle back to entry pointer
    entry = (struct entry *)handle;

    mark_free(entry);

	spin_unlock(&pool->lock);
}

static int sballoc_zpool_shrink(
        void *pool,
        unsigned int pages,
        unsigned int *reclaimed)
{
    pr_debug("SHRINK sballoc_pool %p %d %p\n", pool, pages, reclaimed);
    // TODO
    return -EINVAL;
}

/*
 * Maps the given allocation associated with the given handle.
 *
 * In our case, this is trivial because the handle is the addr of the data for
 * the allocation.
 */
static void *sballoc_zpool_map(
        void *pool,
        unsigned long handle,
        enum zpool_mapmode mm)
{
    pr_debug("MAP sballoc_pool %p %lx\n", pool, handle);

    return (void *)handle;
}

/*
 * Unmap the allocation... nothing to do here...
 */
static void sballoc_zpool_unmap(void *pool, unsigned long handle)
{
    pr_debug("UNMAP sballoc_pool %p %lx\n", pool, handle);
}

/*
 * Get the total size in byets of the pool.
 */
static u64 sballoc_zpool_total_size(void *zpool)
{
    struct sballoc_pool *pool = zpool;

    pr_debug("TOTAL SIZE sballoc_pool %p\n", pool);

    return pool->nr_pages * PAGE_SIZE;
}

static struct zpool_driver sballoc_zpool_driver = {
	.type =		"sballoc",
	.owner =	THIS_MODULE,
	.create =	sballoc_zpool_create,
	.destroy =	sballoc_zpool_destroy,
	.malloc =	sballoc_zpool_malloc,
	.free =		sballoc_zpool_free,
	.shrink =	sballoc_zpool_shrink,
	.map =		sballoc_zpool_map,
	.unmap =	sballoc_zpool_unmap,
	.total_size =	sballoc_zpool_total_size,
};

MODULE_ALIAS("zpool-sballoc");

static int __init init_sballoc(void)
{
    // Make sure an entry fits in one word
	BUILD_BUG_ON(sizeof(struct entry) == sizeof(u64));
	pr_info("loaded\n");

	zpool_register_driver(&sballoc_zpool_driver);

	return 0;
}

static void __exit exit_sballoc(void)
{
#ifdef CONFIG_ZPOOL
	zpool_unregister_driver(&sballoc_zpool_driver);
#endif

	pr_info("unloaded\n");
}

module_init(init_sballoc);
module_exit(exit_sballoc);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mark Mansi <markm@cs.wisc.edu>");
MODULE_DESCRIPTION("Single Byte Allocator for Compressed Pages");
