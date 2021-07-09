#include <storage.h>

#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include <communication_protocol.h>
#include <error_handling.h>
#include <concurrency.h>
#include <free_item.h>

/**
 * Create a file
 *
 * Return a pointer to the created file on success, NULL on error (set errno)
 */
static file_t* file_create(const char* pathname)
{
  if (!pathname || !strlen(pathname)) {
    errno = EINVAL;
    return NULL;
  }
  file_t* new_file;
  if ((new_file = calloc(1, sizeof(file_t))) == NULL) {
    goto end;
  }
  if ((new_file->pathname = calloc(1, sizeof(char) * (strlen(pathname) + 1))) == NULL) {
    goto end;
  }
  strncpy(new_file->pathname, pathname, strlen(pathname));
  EXIT_ON_NZ(pthread_mutex_init(&(new_file->mutex), NULL));

  return new_file;

  end:
  free_item((void**)&(new_file->pathname));
  free_item((void**)&new_file);
  return NULL;
}

void file_dealloc(file_t* file)
{
  free_item((void**)&(file->pathname));
  free_item((void**)&(file->content));
  free_item((void**)&file);
}

/**
 * Destroy a file in the storage
 * (assume that the storage is locked)
 */
static void file_destroy(storage_t* storage, file_t* file, user_node_t** pending_locks, const char dealloc)
{
  LOCK(&(file->ordering));
  LOCK(&(file->mutex));

  while (file->active_readers || file->active_writers) {
    WAIT(&(file->cond), &(file->mutex));
  }

  // remove the file from the list structure
  if (file->previous) {
    file->previous->next = file->next;
  } else {
    storage->head = file->next;
  }
  if (file->next) {
    file->next->previous = file->previous;
  } else {
    storage->tail = file->previous;
  }

  if (file->pending_locks) {
    if (pending_locks) {
      // give back the pending locks list to the caller
      *pending_locks = file->pending_locks;
    } else {
      // destroy the list
      user_node_t* tmp = NULL;
      while ((tmp = file->pending_locks)) {
        file->pending_locks = (file->pending_locks)->next;
	free_item((void**)&tmp);
      }
    }
  }

  // destroy the list of users who have opened the file
  user_node_t* tmp = NULL;
  while ((tmp = file->opened_by)) {
    file->opened_by = (file->opened_by)->next;
    free_item((void**)&tmp);
  }

  storage->file_number--;
  storage->size -= file->size;

  EXIT_ON_NZ(pthread_cond_destroy(&(file->cond)));
  UNLOCK(&(file->ordering));
  UNLOCK(&(file->mutex));
  EXIT_ON_NZ(pthread_mutex_destroy(&(file->ordering)));
  EXIT_ON_NZ(pthread_mutex_destroy(&(file->mutex)));

  // remove the file from the dictionary structure
  EXIT_ON_NZ(icl_hash_delete(storage->dictionary, file->pathname, NULL, NULL));

  if (dealloc) {
    file_dealloc(file);
  }
}

storage_t* storage_create(const size_t max_file_number, const size_t max_size)
{
  storage_t* storage;
  if ((storage = calloc(1, sizeof(storage_t))) == NULL) {
    goto end;
  }
  if ((storage->dictionary = icl_hash_create(max_file_number / 10 + 1, NULL, NULL)) == NULL) {
    goto end;
  }
  EXIT_ON_NZ(pthread_mutex_init(&(storage->mutex), NULL));
  storage->max_file_number = max_file_number;
  storage->max_size = max_size;

  return storage;

  end:
  if (storage->dictionary) EXIT_ON_NEG_ONE(icl_hash_destroy(storage->dictionary, NULL, NULL));
  free_item((void**)&storage);
  return NULL;
}

int storage_destroy(storage_t* storage)
{
  if (!storage) {
    errno = EINVAL;
    return -1;
  }
  // destroy all files
  file_t* current_file;
  while ((current_file = storage->head)) {
    storage->head = (storage->head)->next;
    file_destroy(storage, current_file, NULL, 1);
  }
  EXIT_ON_NEG_ONE(icl_hash_destroy(storage->dictionary, NULL, NULL));
  EXIT_ON_NZ(pthread_mutex_destroy(&(storage->mutex)));
  free_item((void**)&storage);
  return 0;
}

/**
 * Add a file to the storage
 * (assume that the storage is locked)
 */
static void storage_add(storage_t* storage, file_t* file)
{
  // add the file to the queue structure
  if (!storage->head) {
    storage->head = file;
  } else {
    file->previous = storage->tail;
    if (storage->tail) {
      (storage->tail)->next = file;
    }
  }
  storage->tail = file;

  // add the file to the dictionary structure
  EXIT_ON_NULL(icl_hash_insert(storage->dictionary, file->pathname, file));

  storage->file_number++;
  storage->size += file->size;
}

/**
 * Search for a file in the storage
 *
 * Return a pointer to the file on success, NULL on error (set errno)
 */
static file_t* storage_find(const storage_t* storage, const char* pathname)
{
  if (!storage || !pathname || !strlen(pathname)) {
    errno = EINVAL;
    return NULL;
  }
  file_t* file;
  if ((file = (file_t*)icl_hash_find(storage->dictionary, (void*)pathname)) == NULL) {
    errno = ENOENT;
  }
  return file;
}

/**
 * Search for a suitable victim file to remove from the storage
 *
 * Return a pointer to the victim on success, NULL on error (set errno)
 */
static file_t* get_victim(storage_t* storage, const file_t* spare)
{
  if (!storage) {
    errno = EINVAL;
    return NULL;
  }
  file_t* victim = storage->head;
  while (victim) {
    if (victim != spare && victim->modified) {
      return victim;
    }
    victim = victim->next;
  }
  errno = ENOENT;
  return NULL;
}

char* storage_copy(const file_t* file, const size_t extra_space)
{
  if (!file) {
    errno = EINVAL;
    return NULL;
  }
  if (file->content == NULL && !extra_space) {
    // the file has no content
    errno = ENODATA;
    return NULL;
  }
  char* copy_buffer;
  if ((copy_buffer = calloc(1, sizeof(char) * (file->size + extra_space))) == NULL) {
    return NULL;
  }
  memcpy(copy_buffer, file->content, file->size);
  return copy_buffer;
}

/**
 * Put a user at the end of a user list
 *
 * Return 0 on success, -1 on error (set errno)
 */
static int enqueue_user(user_node_t** list, const int user)
{
  if (!list || user <= 0) {
    errno = EINVAL;
    return -1;
  }
  user_node_t* new_node;
  if ((new_node = malloc(sizeof(user_node_t))) == NULL) {
    return -1;
  }
  new_node->user = user;
  new_node->next = NULL;

  // FIFO insertion
  user_node_t** tmp = list;
  while (*tmp) {
    tmp = &((*tmp)->next);
  }
  *tmp = new_node;
  return 0;
}

/**
 * Remove a user from a user list, or the front element if the user is <0
 *
 * Return user on success, 0 on error or if user is not contained (set errno)
 */
static int dequeue_user(user_node_t** list, int user)
{
  if (!(*list)) {
    errno = EINVAL;
    return 0;
  }
  user_node_t* node = NULL;
  if (user < 0) {
    // get the front element
    node = *list;
    *list = (*list)->next;
  } else {
    // get the specified user
    user_node_t* current_node = *list;
    user_node_t* previous_node = NULL;
    while (current_node) {
      if (current_node->user == user) {
        node = current_node;
	if (previous_node) {
	  previous_node->next = current_node->next;
	} else {
	  *list = current_node->next;
	}
	break;
      }
      previous_node = current_node;
      current_node = current_node->next;
    }
  }
  if (!node) {
    errno = ENOENT;
    return 0;
  }
  int tmp = node->user;
  free_item((void**)&node);
  return tmp;
}

char contains_user(user_node_t* list, const int user)
{
  user_node_t* current_node = list;
  while (current_node) {
    if (current_node->user == user) {
      return 1;
    }
    current_node = current_node->next;
  }
  return 0;
}

/**
 * Make the last element of dest list point to the first element of src list
 */
static void concatenate_lists(user_node_t** dest, user_node_t* src)
{
  if (!(*dest)) {
    *dest = src;
    return;
  }
  while ((*dest)->next) {
    dest = &((*dest)->next);
  }
  (*dest)->next = src;
}

int storage_open(storage_t* storage, const char* pathname, const int flags, user_node_t** pending_locks, const int user)
{
  if (!storage || !pathname || !strlen(pathname) || user <= 0) {
    errno = EINVAL;
    return -1;
  }
  const char create_flag = IS_SET(O_CREATE, flags);
  const char lock_flag = IS_SET(O_LOCK, flags);

  LOCK(&(storage->mutex));

  // search for the file
  file_t* file;
  if ((file = storage_find(storage, pathname)) == NULL) {
    // file not found
    if (!create_flag) {
      // no file to do the operation on
      UNLOCK(&(storage->mutex));
      errno = ENOENT;
      return -1;
    }

    // check if storage can store the file
    if (storage->file_number == storage->max_file_number) {
      // need to remove a file
      file_t* victim;
      if ((victim = get_victim(storage, NULL)) == NULL) {
        // could not find eligible victim
	UNLOCK(&(storage->mutex));
	errno = ENOMEM;
	return -1;
      }
      file_destroy(storage, victim, pending_locks, 1);
    }

    // create file
    if ((file = file_create(pathname)) == NULL) {
      UNLOCK(&(storage->mutex));
      return -1;
    }
    if (lock_flag) {
      file->locked_by = user;
      // the first write to the file can be performed by the current user
      file->owner = user;
    }
    EXIT_ON_NEG_ONE(enqueue_user(&(file->opened_by), user));
    // add file to storage
    storage_add(storage, file);

  } else {
    // file exists
    if (create_flag) {
      // file already exists
      UNLOCK(&(storage->mutex));
      errno = EEXIST;
      return -1;
    }

    LOCK(&(file->ordering));
    LOCK(&(file->mutex));
    
    if (lock_flag) {
      if (!file->locked_by) {
        file->locked_by = user;
      } else {
        // file is already locked
        UNLOCK(&(file->ordering));
	UNLOCK(&(file->mutex));
	UNLOCK(&(storage->mutex));
	errno = EACCES;
	return -1;
      }
    }
    // add the user to the list of those who have opened the file
    EXIT_ON_NEG_ONE(enqueue_user(&(file->opened_by), user));
    UNLOCK(&(file->ordering));
    UNLOCK(&(file->mutex));
  }

  UNLOCK(&(storage->mutex));
  return 0;
}

int storage_read(storage_t* storage, const char* pathname, void** buffer, size_t* size, const int user)
{
  if (!storage || !pathname || !strlen(pathname) || !buffer || !size || user <= 0) {
    errno = EINVAL;
    return -1;
  }

  LOCK(&(storage->mutex));

  // search for the file
  file_t* file;
  if ((file = storage_find(storage, pathname)) == NULL) {
    // file not found
    UNLOCK(&(storage->mutex));
    return -1;
  }

  LOCK(&(file->ordering));
  LOCK(&(file->mutex));
  UNLOCK(&(storage->mutex));

  if ((file->locked_by && file->locked_by != user) || !contains_user(file->opened_by, user)) {
    // user cannot access the file
    UNLOCK(&(file->ordering));
    UNLOCK(&(file->mutex));
    errno = EACCES;
    return -1;
  }

  while (file->active_writers) {
    WAIT(&(file->cond), &(file->mutex));
  }
  file->active_readers++;

  UNLOCK(&(file->ordering));
  UNLOCK(&(file->mutex));

  int errnosav = 0;

  // copy file content
  if ((*buffer = storage_copy(file, 0)) == NULL) {
    //could not copy file content
    errnosav = errno;
  } else {
    *size = file->size;
  }

  LOCK(&(file->mutex));
  file->active_readers--;
  if (!errnosav) {
    // the first write to the file can no longer be performed
    file->owner = 0;
  }
  if (!file->active_readers) {
    SIGNAL(&(file->cond));
  }
  UNLOCK(&(file->mutex));

  errno = errnosav;
  return (errno ? -1 : 0);
}

int storage_read_many(storage_t* storage, const long up_to, void** buffer, size_t* size, const int user)
{
  if (!storage || !buffer || !size || user <= 0) {
    errno = EINVAL;
    return -1;
  }

  LOCK(&(storage->mutex));

  // calculate the size of the buffer to return
  long file_count = 0;
  size_t return_size = 0;
  file_t* current_file = storage->head;
  while (current_file && (file_count != up_to || up_to <= 0)) {
    if (!current_file->size) {
      // the file is empty
      current_file = current_file->next;
      continue;
    }
    return_size += METADATA_LENGTH + strlen(current_file->pathname) + METADATA_LENGTH + current_file->size;
    file_count++;
    current_file = current_file->next;
  }
  if (!file_count) {
    // there is no content to read
    UNLOCK(&(storage->mutex));
    errno = ENODATA;
    return -1;
  }
  // add 1 for null terminator
  return_size += 1;

  // allocate the return buffer
  char* return_buffer = NULL;
  if ((return_buffer = calloc(1, return_size)) == NULL) {
    UNLOCK(&(storage->mutex));
    return -1;
  }

  // fill return buffer
  size_t new_return_size = 0;
  file_count = 0;
  current_file = storage->head;
  while (current_file && (file_count != up_to || up_to <= 0)) {
    if (!current_file->size) {
      // the file is empty
      current_file = current_file->next;
      continue;
    }
    // assemble the response
    sprintf(return_buffer + new_return_size, "%010ld%s%010ld", strlen(current_file->pathname), current_file->pathname, current_file->size);
    new_return_size += METADATA_LENGTH + strlen(current_file->pathname) + METADATA_LENGTH;
    // append file content
    char* file_content = NULL;
    if ((file_content = storage_copy(current_file, 0)) == NULL) {
      UNLOCK(&(storage->mutex));
      free_item((void**)&return_buffer);
      return -1;
    }
    memcpy(return_buffer + new_return_size, file_content, current_file->size);
    new_return_size += current_file->size;
    free_item((void**)&file_content);

    file_count++;
    current_file = current_file->next;
  }
  // add 1 for null terminator
  new_return_size += 1;

  UNLOCK(&(storage->mutex));

  if (new_return_size != return_size) {
    // there was an error while reading
    free_item((void**)&return_buffer);
    errno = ECANCELED;
    return -1;
  }

  *buffer = return_buffer;
  *size = new_return_size;

  return file_count;
}

char storage_can_write(storage_t* storage, const char* pathname, const int user)
{
  if (!storage || !pathname || !strlen(pathname) || user <= 0) {
    errno = EINVAL;
    return 0;
  }

  LOCK(&(storage->mutex));

  // search for the file
  file_t* file;
  if ((file = storage_find(storage, pathname)) == NULL) {
    // file not found
    UNLOCK(&(storage->mutex));
    return 0;
  }

  LOCK(&(file->ordering));
  LOCK(&(file->mutex));
  UNLOCK(&(storage->mutex));

  char write_permission = (file->owner == user);

  UNLOCK(&(file->ordering));
  UNLOCK(&(file->mutex));

  return write_permission;
}

int storage_append(storage_t* storage, const char* pathname, const char* new_content, const size_t new_content_length, user_node_t** pending_locks, file_t** removed_list, const int user)
{
  if (!storage || !pathname || !strlen(pathname) || !new_content || !new_content_length || !pending_locks || !removed_list || user <= 0) {
    errno = EINVAL;
    return -1;
  }

  LOCK(&(storage->mutex));

  // search for the file
  file_t* file;
  if ((file = storage_find(storage, pathname)) == NULL) {
    // file not found
    UNLOCK(&(storage->mutex));
    return -1;
  }

  LOCK(&(file->ordering));
  LOCK(&(file->mutex));

  if ((file->locked_by && file->locked_by != user) || !contains_user(file->opened_by, user)) {
    // user cannot access the file
    UNLOCK(&(file->ordering));
    UNLOCK(&(file->mutex));
    UNLOCK(&(storage->mutex));
    errno = EACCES;
    return -1;
  }

  while (file->active_readers || file->active_writers) {
    WAIT(&(file->cond), &(file->mutex));
  }
  file->active_writers = 1;
  UNLOCK(&(file->ordering));
  UNLOCK(&(file->mutex));

  char* new_file_content;
  if ((new_file_content = storage_copy(file, new_content_length)) == NULL) {
    LOCK(&(file->mutex));
    file->active_writers = 0;
    BROADCAST(&(file->cond));
    UNLOCK(&(file->mutex));
    UNLOCK(&(storage->mutex));
    return -1;
  }
  // append new content to file content buffer
  memcpy(new_file_content + file->size, new_content, new_content_length);
  size_t new_file_size = file->size + new_content_length;

  // check if storage can store the new file content
  if (new_file_size > storage->max_size) {
    // new content cannot be stored
    free_item((void**)&new_file_content);
    LOCK(&(file->mutex));
    file->active_writers = 0;
    BROADCAST(&(file->cond));
    UNLOCK(&(file->mutex));
    UNLOCK(&(storage->mutex));
    errno = ENOMEM;
    return -1;
  }
  while (storage->size - file->size + new_file_size > storage->max_size) {
    // need to remove some files
    file_t* victim;
    if ((victim = get_victim(storage, file)) == NULL) {
      // could not find eligible victim
      free_item((void**)&new_file_content);
      LOCK(&(file->mutex));
      file->active_writers = 0;
      BROADCAST(&(file->cond));
      UNLOCK(&(file->mutex));
      UNLOCK(&(storage->mutex));
      errno = ENOMEM;
      return -1;
    }

    // build a list of removed files
    victim->next = *removed_list;
    *removed_list = victim;
    
    // get the list of users who were waiting to lock this file
    user_node_t* tmp_list = NULL;
    file_destroy(storage, victim, &tmp_list, 0);
    // create a single list of all users to be notified of the file removal
    concatenate_lists(pending_locks, tmp_list);
  }

  // update the storage size
  storage->size = storage->size - file->size + new_file_size;
  // update the written file
  free_item((void**)&(file->content));
  file->content = new_file_content;
  file->size = new_file_size;
  file->modified = 1;
  // the first write to the file can no longer be performed
  file->owner = 0;

  LOCK(&(file->mutex));
  file->active_writers = 0;
  BROADCAST(&(file->cond));
  UNLOCK(&(file->mutex));
  UNLOCK(&(storage->mutex));

  return 0;
}

int storage_lock(storage_t* storage, const char* pathname, const int user)
{
  if (!storage || !pathname || !strlen(pathname) || user <= 0) {
    errno = EINVAL;
    return -1;
  }

  LOCK(&(storage->mutex));

  // search for the file
  file_t* file;
  if ((file = storage_find(storage, pathname)) == NULL) {
    // file not found
    UNLOCK(&(storage->mutex));
    return -1;
  }

  LOCK(&(file->ordering));
  LOCK(&(file->mutex));
  UNLOCK(&(storage->mutex));

  while (file->active_readers || file->active_writers) {
    WAIT(&(file->cond), &(file->mutex));
  }

  if (file->locked_by && file->locked_by != user) {
    // user cannot lock the file at the moment
    // (put user in waiting list)
    EXIT_ON_NEG_ONE(enqueue_user(&(file->pending_locks), user));
    UNLOCK(&(file->ordering));
    UNLOCK(&(file->mutex));
    errno = EINPROGRESS;
    return -2;
  }

  file->active_writers = 1;
  UNLOCK(&(file->ordering));
  UNLOCK(&(file->mutex));

  file->locked_by = user;

  LOCK(&(file->mutex));
  file->active_writers = 0;
  // the first write to the file can no longer be performed
  file->owner = 0;

  BROADCAST(&(file->cond));
  UNLOCK(&(file->mutex));

  return 0;
}

int storage_unlock(storage_t* storage, const char* pathname, int* waiter, const int user)
{
  if (!storage || !pathname || !strlen(pathname) || !waiter || user <= 0) {
    errno = EINVAL;
    return -1;
  }

  LOCK(&(storage->mutex));

  // search for the file
  file_t* file;
  if ((file = storage_find(storage, pathname)) == NULL) {
    // file not found
    UNLOCK(&(storage->mutex));
    return -1;
  }

  LOCK(&(file->ordering));
  LOCK(&(file->mutex));
  UNLOCK(&(storage->mutex));

  while (file->active_readers || file->active_writers) {
    WAIT(&(file->cond), &(file->mutex));
  }

  if (file->locked_by == user) {
    // get the first user waiting to lock the file
    // (if there are no pending locks, new_lock will be 0)
    int new_lock = dequeue_user(&(file->pending_locks), -1);

    *waiter = new_lock;
    file->locked_by = new_lock;
    // the first write to the file can no longer be performed
    file->owner = 0;
  } else {
    // user cannot unlock the file
    UNLOCK(&(file->ordering));
    UNLOCK(&(file->mutex));
    errno = EACCES;
    return -1;
  }

  BROADCAST(&(file->cond));
  UNLOCK(&(file->ordering));
  UNLOCK(&(file->mutex));

  return 0;
}

int storage_close(storage_t* storage, const char* pathname, const int user)
{
  if (!storage || !pathname || !strlen(pathname) || user <= 0) {
    errno = EINVAL;
    return -1;
  }

  LOCK(&(storage->mutex));

  // search for the file
  file_t* file;
  if ((file = storage_find(storage, pathname)) == NULL) {
    // file not found
    UNLOCK(&(storage->mutex));
    return -1;
  }

  LOCK(&(file->ordering));
  LOCK(&(file->mutex));

  while (file->active_readers || file->active_writers) {
    WAIT(&(file->cond), &(file->mutex));
  }
  file->active_writers = 1;

  UNLOCK(&(file->ordering));
  UNLOCK(&(file->mutex));

  // remove the user from the list of those who have opened the file
  if (!dequeue_user(&(file->opened_by), user)) {
    // an error occurred or user did not open the file
    LOCK(&(file->mutex));
    file->active_writers = 0;
    BROADCAST(&(file->cond));
    UNLOCK(&(file->mutex));
    UNLOCK(&(storage->mutex));
    errno = EINVAL;
    return -1;
  }

  LOCK(&(file->mutex));
  file->active_writers = 0;
  // the first write to the file can no longer be performed
  file->owner = 0;

  BROADCAST(&(file->cond));
  UNLOCK(&(file->mutex));
  UNLOCK(&(storage->mutex));

  return 0;
}

int storage_remove(storage_t* storage, const char* pathname, user_node_t** pending_locks, const int user)
{
  if (!storage || !pathname || !strlen(pathname) || user <= 0) {
    errno = EINVAL;
    return -1;
  }

  LOCK(&(storage->mutex));
  // search for the file
  file_t* file;
  if ((file = storage_find(storage, pathname)) == NULL) {
    // file not found
    UNLOCK(&(storage->mutex));
    return -1;
  }
  if (file->locked_by != user) {
    // user cannot remove the file
    UNLOCK(&(storage->mutex));
    errno = EACCES;
    return -1;
  }
  file_destroy(storage, file, pending_locks, 1);

  UNLOCK(&(storage->mutex));

  return 0;
}

int storage_user_exit(storage_t* storage, user_node_t** pending_locks, const int user)
{
  if (!storage || !pending_locks || user <= 0) {
    errno = EINVAL;
    return -1;
  }

  LOCK(&(storage->mutex));

  file_t* current_file = storage->head;
  while (current_file) {
    LOCK(&(current_file->ordering));
    LOCK(&(current_file->mutex));

    while (current_file->active_readers || current_file->active_writers) {
      WAIT(&(current_file->cond), &(current_file->mutex));
    }

    if (current_file->locked_by == user) {
      // get the first user waiting to lock the file
      // (if there are no pending locks, waiter will be 0)
      int waiter = dequeue_user(&(current_file->pending_locks), -1);
      // communicate waiter fd back to caller
      if (waiter > 0) {
        enqueue_user(pending_locks, waiter);
      }
      current_file->locked_by = waiter;
    }

    // remove the user form the list of users waiting to lock the file
    dequeue_user(&(current_file->pending_locks), user);
    // remove the user from the list of users that opened the file
    dequeue_user(&(current_file->opened_by), user);

    UNLOCK(&(current_file->ordering));
    UNLOCK(&(current_file->mutex));

    current_file = current_file->next;
  }

  UNLOCK(&(storage->mutex));

  return 0;
}
