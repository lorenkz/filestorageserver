#ifndef CONCURRENCY_H
#define CONCURRENCY_H

#include <pthread.h>

#include <error_handling.h>

/**
 * The following macros are used to simplify the use of the pthread library.
 *
 * The idea of using 'do/while' version is to make a macro which will expand
 * into a regular statement, not into a compound one.
 *
 * (putting ';' at the end of that definition would immediately defeat
 * the entire point of using 'do/while' and make that macro pretty much
 * equivalent to the compound-statement version)
 */

#define LOCK(X) \
  do { \
    EXIT_ON_NZ(pthread_mutex_lock(X)); \
  } while (0)

#define UNLOCK(X) \
  do { \
    EXIT_ON_NZ(pthread_mutex_unlock(X)); \
  } while (0)

#define WAIT(X, Y) \
  do { \
    EXIT_ON_NZ(pthread_cond_wait(X, Y)); \
  } while (0)

#define SIGNAL(X) \
  do { \
    EXIT_ON_NZ(pthread_cond_signal(X)); \
  } while (0)

#define BROADCAST(X) \
  do { \
    EXIT_ON_NZ(pthread_cond_broadcast(X)); \
  } while (0)

#endif
