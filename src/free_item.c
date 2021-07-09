#include <free_item.h>

#include <stdlib.h>
#include <stddef.h>

void free_item(void** ptr)
{
  if (*ptr != NULL) {
    free(*ptr);
    *ptr = NULL;
  }
}
