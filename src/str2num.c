#include <str2num.h>

#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <stdlib.h>

int str2num(const char* s, long* n)
{
  if (!s || !strlen(s)) {
    return 1;
  }
  char* e = NULL;
  errno = 0;
  long val = strtol(s, &e, 10);
  if (errno == ERANGE) {
    return 2;
  }
  if (e && *e == '\0') {
    *n = val;
    return 0;
  }
  return 1;
}
