#ifndef UBUFFER_H
#define UBUFFER_H

#include <pthread.h>

typedef struct node_s {
  void* data;
  struct node_s* next;
} node_t;

typedef struct {
  node_t* front;
  node_t* back;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
} ubuffer_t;

/**
 * Create a new unbounded buffer
 *
 * Return pointer to buffer on success, NULL on error (set errno)
 */
ubuffer_t* ubuffer_create();

/**
 * Destroy an unbounded buffer
 *
 * Return 0 on success, -1 on error (set errno)
 */
int ubuffer_destroy(ubuffer_t* buffer);

/**
 * Add a data to the buffer
 *
 * Return 0 on success, -1 on error (set errno)
 */
int ubuffer_enqueue(ubuffer_t* buffer, void* data);

/**
 * Remove a data from the buffer
 *
 * Return pointer to data on success, NULL on error (set errno)
 */
void* ubuffer_dequeue(ubuffer_t* buffer);

#endif
