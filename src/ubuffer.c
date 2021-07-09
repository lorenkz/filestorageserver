#include <ubuffer.h>

#include <stdlib.h>
#include <errno.h>

#include <concurrency.h>

ubuffer_t* ubuffer_create()
{
  ubuffer_t* buffer;
  if ((buffer = malloc(sizeof(ubuffer_t))) == NULL) {
    return NULL;
  }
  buffer->front = buffer->back = NULL;
  // initialize mutex and condition variable
  EXIT_ON_NZ(pthread_mutex_init(&(buffer->mutex), NULL));
  EXIT_ON_NZ(pthread_cond_init(&(buffer->cond), NULL));

  return buffer;
}

int ubuffer_destroy(ubuffer_t* buffer)
{
  if (!buffer) {
    errno = EINVAL;
    return -1;
  }
  // empty buffer
  node_t* tmp;
  while ((tmp = buffer->front)) {
    buffer->front = (buffer->front)->next;
    free(tmp->data);
    free(tmp);
  }
  // destroy mutex and condition variable
  EXIT_ON_NZ(pthread_mutex_destroy(&(buffer->mutex)));
  EXIT_ON_NZ(pthread_cond_destroy(&(buffer->cond)));
  free(buffer);

  return 0;
}

/**
 * Check if the buffer is empty
 *
 * Return != 0 if empty, 0 if not
 */
static char is_empty(const ubuffer_t* buffer)
{
  return ((buffer->front == NULL) && (buffer->back == NULL));
}

int ubuffer_enqueue(ubuffer_t* buffer, void* data)
{
  if (!buffer || !data) {
    errno = EINVAL;
    return -1;
  }
  // create new node
  node_t* new_node;
  if ((new_node = malloc(sizeof(node_t))) == NULL) {
    return -1;
  }
  new_node->data = data;
  new_node->next = NULL;

  // lock buffer
  LOCK(&(buffer->mutex));

  // FIFO insertion
  if (is_empty(buffer)) {
    buffer->front = new_node;
  } else {
    (buffer->back)->next = new_node;
  }
  buffer->back = new_node;

  if (buffer->front == buffer->back) {
    // the buffer was empty before insertion,
    // wake up any consumer
    BROADCAST(&(buffer->cond));
  }

  // unlock buffer
  UNLOCK(&(buffer->mutex));

  return 0;
}

void* ubuffer_dequeue(ubuffer_t* buffer)
{
  if (!buffer) {
    errno = EINVAL;
    return NULL;
  }

  // lock buffer
  LOCK(&(buffer->mutex));

  while (is_empty(buffer)) {
    // wait producer
    WAIT(&(buffer->cond), &(buffer->mutex));
  }

  // FIFO removal
  node_t* tmp = buffer->front;
  if (buffer->front == buffer->back) {
    // there is only one node
    buffer->front = buffer->back = NULL;
  } else {
    // there are many nodes
    buffer->front = (buffer->front)->next;
  }

  // unlock buffer
  UNLOCK(&(buffer->mutex));
  
  void* data = tmp->data;
  free(tmp);

  return data;
}
