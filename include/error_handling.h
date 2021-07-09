#ifndef ERROR_HANDLING_H
#define ERROR_HANDLING_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * The following macros are used to print an error message and terminate
 * the program in case of errors which are considered fatal.
 *
 * The idea of using 'do/while' version is to make a macro which will expand
 * into a regular statement, not into a compound one.
 *
 * (putting ';' at the end of that definition would immediately defeat
 * the entire point of using 'do/while' and make that macro pretty much
 * equivalent to the compound-statement version)
 */

#define EXIT_ON_NEG_ONE(f) \
do { \
    if ((f) == -1 && errno != EINTR) { \
      perror(#f); \
      exit(EXIT_FAILURE); \
    } \
  } while (0)

#define EXIT_ON_NULL(f) \
do { \
    if ((f) == NULL) { \
      perror(#f); \
      exit(EXIT_FAILURE); \
    } \
  } while (0)

#define EXIT_ON_NZ(f) \
  do { \
    if ((f) && errno != EINTR) { \
      perror(#f); \
      exit(EXIT_FAILURE); \
    } \
  } while (0)

#endif
