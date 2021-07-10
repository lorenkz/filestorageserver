#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>
#include <pthread.h>

#include <icl_hash.h>

typedef struct user_node_s {
  int user;
  struct user_node_s* next;
} user_node_t;

typedef struct file_s {
  char* pathname;
  char* content;
  size_t size;
  // the user that can perform the first write to the file (0 if none)
  int owner;
  // used in replacement algorithms,
  // 0 by default, becomes 1 when the file is modified for the first time
  char modified;
  // list of users who have opened the file
  user_node_t* opened_by;
  // the user who locked the file (0 if unlocked)
  int locked_by;
  // list of users waiting to lock the file
  user_node_t* pending_locks;
  char active_writers;
  size_t active_readers;
  pthread_mutex_t mutex;
  pthread_mutex_t ordering;
  pthread_cond_t cond;
  struct file_s* previous;
  struct file_s* next;
} file_t;

typedef struct {
  size_t file_number;
  size_t size;
  size_t max_file_number;
  size_t max_size;
  icl_hash_t* dictionary;
  file_t* head;
  file_t* tail;
  pthread_mutex_t mutex;
  // used to print a summary of the operations performed
  size_t max_file_number_reached;
  size_t max_size_reached;
  size_t replacement_counter;
} storage_t;

/**
 * Deallocate a file
 */
void file_dealloc(file_t* file);

/**
 * Create a storage
 *
 * Return a pointer to the created storage on success, NULL on error (set errno)
 */
storage_t* storage_create(const size_t max_file_number, const size_t max_size);

/**
 * Destroy a storage
 *
 * Return 0 on success, -1 on error (set errno)
 */
int storage_destroy(storage_t* storage);

/**
 * Print a summary of the operations performed in the storage
 */
void storage_print_summary(storage_t* storage);

/**
 * Copy the contents of a file into a newly allocated buffer (with optional extra space)
 *
 * Return a pointer to the allocated buffer on success, NULL on error (set errno)
 */
char* storage_copy(const file_t* file, const size_t extra_space);

/**
 * Check if a user is contained in a user list
 *
 * Return 1 if it is contained, 0 if not
 */
char contains_user(user_node_t* list, const int user);

/**
 * Open a file in the storage
 *
 * Return 0 on success, -1 on error (set errno)
 */
int storage_open(storage_t* storage, const char* pathname, const int flags, user_node_t** pending_locks, const int user);

/**
 * Read a file in the storage
 *
 * Return 0 on success, -1 on error (set errno)
 */
int storage_read(storage_t* storage, const char* pathname, void** buffer, size_t* size, const int user);

/**
 * Read many files in the storage
 *
 * Return the number of files read on success, -1 on error (set errno)
 */
int storage_read_many(storage_t* storage, const long up_to, void** buffer, size_t* size, const int user);

/**
 * Check if a user has write permission on a file in the storage
 *
 * Return 1 if user has write permission, 0 if not (set errno)
 */
char storage_can_write(storage_t* storage, const char* pathname, const int user);

/**
 * Append content to a file in the storage
 *
 * Return 0 on success, -1 on error (set errno)
 */
int storage_append(storage_t* storage, const char* pathname, const char* new_content, const size_t new_content_length, user_node_t** pending_locks, file_t** removed_list, const int user);

/**
 * Lock a file in the storage
 *
 * Return 0 on success, -1 on error (set errno)
 */
int storage_lock(storage_t* storage, const char* pathname, const int user);

/**
 * Unlock a file in the storage
 *
 * Return 0 on success, -1 on error (set errno)
 */
int storage_unlock(storage_t* storage, const char* pathname, int* waiter, const int user);

/**
 * Close a file in the storage
 *
 * Return 0 on success, -1 on error (set errno)
 */
int storage_close(storage_t* storage, const char* pathname, const int user);

/**
 * Remove a file from the storage
 *
 * Return 0 on success, -1 on error (set errno)
 */
int storage_remove(storage_t* storage, const char* pathname, user_node_t** pending_locks, const int user);

/**
 * Manage a user exit from the storage
 *
 * Return 0 on success, -1 on error (set errno)
 */
int storage_user_exit(storage_t* storage, user_node_t** pending_locks, const int user);

#endif
