#ifndef FREE_ITEM_H
#define FREE_ITEM_H

/**
 * Free the item pointed by the pointer at the given address
 * and set the pointer to NULL to avoid undefined behaviors for future pointer references
 */
void free_item(void** ptr);

#endif
