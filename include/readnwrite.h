#ifndef READNWRITE_H
#define READNWRITE_H

#include <sys/types.h>
#include <stddef.h>

/**
 * Read up to 'size' bytes from a descriptor
 *
 * Return 'size' on success, 0 if read return 0 (EOF has been read), -1 on error (set errno)
 */
ssize_t readn(int fd, void* buf, size_t size);

/**
 * Write up to 'size' bytes to a descriptor
 *
 * Return 1 on success, 0 if write return 0, -1 on error (set errno)
 */
ssize_t writen(int fd, void* buf, size_t size);

#endif
