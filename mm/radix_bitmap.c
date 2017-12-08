
#include <linux/radix_bitmap.h>

/*
 * Initialize the given radix bitmap struct to a valid empty bitmap.
 */
void radix_bitmap_create(struct radix_bitmap *rb) {
    // TODO
}

/*
 * Destroy the given bitmap and free all of its memory.
 */
void radix_bitmap_destroy(struct radix_bitmap *rb) {
    // TODO
}

/*
 * Returns the bit.
 */
bool radix_bitmap_get(struct radix_bitmap *rb, unsigned long idx) {
    return 0; // TODO
}

/*
 * Set the given bit.
 */
void radix_bitmap_set(struct radix_bitmap *rb, unsigned long idx) {
    // TODO
}

/*
 * Unset the given bit.
 */
void radix_bitmap_unset(struct radix_bitmap *rb, unsigned long idx) {
    // TODO
}
