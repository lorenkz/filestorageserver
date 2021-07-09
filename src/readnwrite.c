#include <readnwrite.h>

#include <unistd.h>
#include <errno.h>

ssize_t readn(int fd, void* buf, size_t size)
{
  size_t nleft = size;
  ssize_t nread;
  char* bufptr = (char*)buf;
  while (nleft > 0) {
    if ((nread = read(fd, bufptr, nleft)) == -1) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (nread == 0) {
      return 0;
    }
    nleft -= nread;
    bufptr += nread;
  }
  return size;
}

ssize_t writen(int fd, void* buf, size_t size)
{
  size_t nleft = size;
  ssize_t nwritten;
  char* bufptr = (char*)buf;
  while (nleft > 0) {
    if ((nwritten = write(fd, bufptr, nleft)) == -1) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (nwritten == 0) {
      return 0;
    }
    nleft -= nwritten;
    bufptr += nwritten;
  }
  return 1;
}
