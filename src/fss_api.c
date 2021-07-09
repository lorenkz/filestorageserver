#include <fss_api.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <unistd.h>
#include <limits.h>

#include <communication_protocol.h>
#include <error_handling.h>
#include <free_item.h>
#include <readnwrite.h>
#include <str2num.h>

#define WAIT_FOR_RESPONSE() \
  do { \
    char response_buffer[RESPONSE_CODE_LENGTH + 1] = {0}; \
    if (readn(fss_client_socket, response_buffer, RESPONSE_CODE_LENGTH) == -1) {\
      goto end; \
    } \
    if (str2num(response_buffer, &response_code) != 0) { \
      response_code = INVALID_RESPONSE; \
      errno = EINVAL; \
      goto end; \
    } \
    if (response_code != OK) { \
      errno = ECANCELED; \
      goto end; \
    } \
  } while (0)

int fss_client_socket;
char* fss_socket_name = NULL;
// verbose mode is disabled by default
char fss_verbose = 0;

/**
 * Print an error message on stderr based on the server response code
 */
static void print_error(const long response_code)
{
  char* msg;
  switch (response_code) {
    case RESPONSE_CODE_INIT:
      msg = "internal client error";
      break;
    case OK:
      msg = "request has succeeded";
      break;
    case FILE_NOT_FOUND:
      msg = "file not found";
      break;
    case ALREADY_EXISTS:
      msg = "file already exists";
      break;
    case NO_CONTENT:
      msg = "no content to read";
      break;
    case FORBIDDEN:
      msg = "client does not have access rights to the content";
      break;
    case OUT_OF_MEMORY:
      msg = "content too large to be stored";
      break;
    case INTERNAL_SERVER_ERROR:
      msg = "internal server error";
      break;
    case BAD_REQUEST:
      msg = "invalid request syntax";
      break;
    case INVALID_RESPONSE:
      msg = "invalid response from server";
      break;
    default:
      msg = "unknown error";
  }
  fprintf(stderr, "         (%s)\n", msg);
}

int store_file(const char* abs_pathname, const char* content, const size_t size, const char* directory)
{
  // variables initialization
  char* abs_directory = NULL;
  char* cwd = NULL;
  FILE* file = NULL;

  if (!abs_pathname || !strlen(abs_pathname) || !content || !size || !directory) {
    errno = EINVAL;
    goto end;
  }

  // get directory's absolute path
  if ((abs_directory = realpath(directory, NULL)) == NULL) {
    goto end;
  }
  // get current working directory
  if ((cwd = getcwd(NULL, PATH_MAX)) == NULL) {
    goto end;
  }
  // change working directory
  if (chdir(abs_directory) == -1) {
    goto end;
  }
  free_item((void**)&abs_directory);

  // get filename from the absolute pathname
  char* name;
  if ((name = strrchr(abs_pathname, '/')) == NULL || *(name + 1) == '\0') {
    // invalid pathname
    errno = EINVAL;
    goto end;
  }
  name += 1;
  char filename[NAME_MAX] = {0};
  strncpy(filename, name, strlen(name));

  // check for duplicates
  size_t duplicates = 0;
  while ((errno = 0, access(filename, F_OK)) == 0) {
    duplicates++;
    // make a new filename
    if (snprintf(filename, NAME_MAX - 1, "%s(%zu)", name, duplicates) == NAME_MAX - 1) {
      if (filename[NAME_MAX - 2] != ')' && filename[NAME_MAX - 2] != '\0') {
        // filename is too long and cannot be formatted properly
	goto end;
      }
    }
  }
  if (errno != ENOENT) {
    perror("access");
    goto end;
  }

  // store the file content
  if ((file = fopen(filename, "w+")) == NULL) {
    goto end;
  }
  if (fwrite(content, 1, size, file) != size) {
    int myerrno = errno;
    if (ferror(file)) {
      errno = myerrno;
      goto end;
    }
  }
  if (file != NULL) EXIT_ON_NZ(fclose(file));
  file = NULL;

  // return to the previous working directory
  if (chdir(cwd) == -1) {
    goto end;
  }
  free_item((void**)&cwd);

  return 0;

  end:
  free_item((void**)&abs_directory);
  if (cwd != NULL) {
    EXIT_ON_NEG_ONE(chdir(cwd));
    free_item((void**)&cwd);
  }
  if (file != NULL) EXIT_ON_NZ(fclose(file));
  return -1;
}

/**
 * Read files sent by the server and store them under 'dirname'
 *
 * Return the number of files received on success, -1 on error (set errno)
 */
static int receive_files(const char* dirname)
{
  // variables initialization
  char* pathname_buffer = NULL;
  char* file_buffer = NULL;

  int files_read = 0;
  char length_buffer[METADATA_LENGTH + 1] = {0};
  long pathname_length;
  long file_size;

  while (1) {
    // read pathname length
    if (readn(fss_client_socket, length_buffer, METADATA_LENGTH) == -1) {
      goto end;
    }
    if ((pathname_length = atol(length_buffer)) == 0) {
      // no more files to read
      break;
    }

    if ((pathname_buffer = calloc(1, sizeof(char) * (pathname_length + 1))) == NULL) {
      goto end;
    }
    // read pathname
    if (readn(fss_client_socket, pathname_buffer, pathname_length) == -1) {
      goto end;
    }

    // read file size
    if (readn(fss_client_socket, length_buffer, METADATA_LENGTH) == -1) {
      goto end;
    }
    file_size = atol(length_buffer);
    if ((file_buffer = calloc(1, sizeof(char) * (file_size + 1))) == NULL) {
      goto end;
    }
    // read file content
    if (readn(fss_client_socket, file_buffer, file_size) == -1) {
      goto end;
    }
    files_read++;

    // store file on disk
    if (dirname && store_file(pathname_buffer, file_buffer, file_size, dirname) == -1) {
      goto end;
    }
    free_item((void**)&pathname_buffer);
    free_item((void**)&file_buffer);
  }
  return files_read;

  end:
  free_item((void**)&pathname_buffer);
  free_item((void**)&file_buffer);
  return -1;
}

void sleep_for(const long msec)
{
  struct timespec req;
  if (msec > 999) {
    req.tv_sec = (int)(msec / 1000);
    req.tv_nsec = (msec - ((long)req.tv_sec * 1000)) * 1000000;
  } else {
    req.tv_sec = 0; // must be non-negative
    req.tv_nsec = 1000000 * msec; // must be in range [0, 999999999]
  }
  while (nanosleep(&req, &req)) {
    ; // nanosleep returns 0 if and only if the requested time has elapsed
  }
}

int openConnection(const char* sockname, int msec, const struct timespec abstime)
{
  if (!sockname || !strlen(sockname) || msec < 0) {
    errno = EINVAL;
    goto end;
  }

  struct sockaddr_un address;
  memset(&address, '0', sizeof(address));
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, sockname, sizeof(address.sun_path) - 1);

  if ((fss_client_socket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    goto end;
  }
  time_t current_time;
  while (connect(fss_client_socket, (struct sockaddr*)&address, sizeof(address)) == -1) {
    if (errno == ENOENT) {
      current_time = time(NULL); // man 2 time: "when tloc is NULL, the call cannot fail"
      if (current_time >= abstime.tv_sec) {
	errno = ETIMEDOUT;
	goto end;
      }
      if (fss_verbose) {
        fprintf(stderr, "[%d]: (%s) '%s': ", getpid(), "openConnection", sockname);
	fprintf(stderr, "error: could not connect to socket, retrying in %d msec...\n", msec);
      }
      // wait before retrying to connect
      sleep_for(msec);
    } else {
      goto end;
    }
  }
  // The POSIX specification does not define the length of the sun_path array
  // and it specifically warns that applications should not assume a particular length
  if ((fss_socket_name = calloc(1, sizeof(char) * (strlen(address.sun_path) + 1))) == NULL) {
    goto end;
  }
  strncpy(fss_socket_name, address.sun_path, strlen(address.sun_path));
  if (fss_verbose) {
    fprintf(stdout, "[%d]: (%s) '%s': ", getpid(), "openConnection", sockname);
    fprintf(stdout, "successfully connected to socket\n");
  }
  return 0;

  end:
  free_item((void**)&fss_socket_name);
  if (fss_verbose) {
    fprintf(stderr, "[%d]: (%s) '%s': ", getpid(), "openConnection", sockname);
    fprintf(stderr, "error: could not connect to socket\n");
  }
  return -1;
}

int closeConnection(const char* sockname)
{
  if (!sockname || !fss_socket_name || strncmp(sockname, fss_socket_name, strlen(fss_socket_name))) {
    errno = EINVAL;
    goto end;
  }

  if (close(fss_client_socket) == -1) {
    goto end;
  }
  free_item((void**)&fss_socket_name);
  if (fss_verbose) {
    fprintf(stdout, "[%d]: (%s) '%s': ", getpid(), "closeConnection", sockname);
    fprintf(stdout, "successfully disconnected from socket\n");
  }
  return 0;

  end:
  if (fss_verbose) {
    fprintf(stderr, "[%d]: (%s) '%s': ", getpid(), "closeConnection", sockname);
    fprintf(stderr, "error: could not disconnect from socket\n");
  }
  return -1;
}

int openFile(const char* pathname, int flags)
{
  // variables initialization
  char* abs_pathname = NULL;
  char* request = NULL;
  long response_code = RESPONSE_CODE_INIT;

  if (!pathname || !strlen(pathname)) {
    errno = EINVAL;
    goto end;
  }
  // get absolute pathname
  if ((abs_pathname = realpath(pathname, NULL)) == NULL) {
    goto end;
  }

  const size_t pathname_length = strlen(abs_pathname);
  const size_t request_length = REQUEST_CODE_LENGTH + METADATA_LENGTH + pathname_length + OPEN_FLAGS_LENGTH + 1;
  if ((request = calloc(1, sizeof(char) * request_length)) == NULL) {
    goto end;
  }
  // assemble the request
  snprintf(request, request_length, "%d%010ld%s%d", OPEN_FILE, pathname_length, abs_pathname, flags);
  free_item((void**)&abs_pathname);
  // send the request
  if (writen(fss_client_socket, request, request_length - 1) == -1) {
    goto end;
  }
  free_item((void**)&request);

  WAIT_FOR_RESPONSE();

  if (fss_verbose) {
    fprintf(stdout, "[%d]: (%s) '%s': ", getpid(), "openFile", pathname);
    fprintf(stdout, "file successfully opened\n");
  }
  return 0;

  end:
  free_item((void**)&abs_pathname);
  free_item((void**)&request);
  if (fss_verbose) {
    fprintf(stderr, "[%d]: (%s) '%s': ", getpid(), "openFile", pathname);
    fprintf(stderr, "error: could not open file\n");
    print_error(response_code);
  }
  return -1;
}

int readFile(const char* pathname, void** buf, size_t* size)
{
  // variables initialization
  char* abs_pathname = NULL;
  char* request = NULL;
  char* file_buffer = NULL;
  long response_code = RESPONSE_CODE_INIT;

  if (!pathname || !strlen(pathname) || !buf || !size) {
    errno = EINVAL;
    goto end;
  }
  // get absolute pathname
  if ((abs_pathname = realpath(pathname, NULL)) == NULL) {
    goto end;
  }

  const size_t pathname_length = strlen(abs_pathname);
  const size_t request_length = REQUEST_CODE_LENGTH + METADATA_LENGTH + pathname_length + 1;
  if ((request = calloc(1, sizeof(char) * request_length)) == NULL) {
    goto end;
  }
  // assemble the request
  snprintf(request, request_length, "%d%010ld%s", READ_FILE, pathname_length, abs_pathname);
  free_item((void**)&abs_pathname);
  // send the request
  if (writen(fss_client_socket, request, request_length - 1) == -1) {
    goto end;
  }
  free_item((void**)&request);

  WAIT_FOR_RESPONSE();

  // get file size
  char file_size_buffer[METADATA_LENGTH + 1] = {0};
  if (readn(fss_client_socket, file_size_buffer, METADATA_LENGTH) == -1) {
    response_code = INVALID_RESPONSE;
    goto end;
  }
  long file_size;
  if (str2num(file_size_buffer, &file_size) != 0) {
    response_code = INVALID_RESPONSE;
    errno = EINVAL;
    goto end;
  }
  // get file content
  if ((file_buffer = calloc(1, sizeof(char) * (file_size + 1))) == NULL) {
    response_code = RESPONSE_CODE_INIT;
    goto end;
  }
  if (readn(fss_client_socket, file_buffer, file_size) == -1) {
    response_code = INVALID_RESPONSE;
    goto end;
  }
  if (fss_verbose) {
    fprintf(stdout, "[%d]: (%s) '%s': ", getpid(), "readFile", pathname);
    fprintf(stdout, "%ld bytes read\n", file_size);
  }
  *size = file_size;
  *buf = file_buffer;
  return 0;

  end:
  free_item((void**)&abs_pathname);
  free_item((void**)&request);
  free_item((void**)&file_buffer);
  if (fss_verbose) {
    fprintf(stderr, "[%d]: (%s) '%s': ", getpid(), "readFile", pathname);
    fprintf(stderr, "error: could not read file\n");
    print_error(response_code);
  }
  return -1;
}

int readNFiles(int N, const char* dirname)
{
  // variables initialization
  long response_code = RESPONSE_CODE_INIT;

  const size_t request_length = REQUEST_CODE_LENGTH + METADATA_LENGTH + 1;
  char request[request_length];
  // assemble the request
  snprintf(request, request_length, "%d%010d", READ_N_FILES, N);
  // send the request
  if (writen(fss_client_socket, request, request_length - 1) == -1) {
    goto end;
  }

  WAIT_FOR_RESPONSE();

  // receive files
  int files_read;
  if ((files_read = receive_files(dirname)) == -1) {
    response_code = INVALID_RESPONSE;
    goto end;
  }
  if (fss_verbose) {
    fprintf(stdout, "[%d]: (%s) '%s': ", getpid(), "readNFiles", dirname);
    fprintf(stdout, "%d files read", files_read);
    if (dirname) {
      fprintf(stdout, " (and stored)");
    }
    fprintf(stdout, "\n");
  }
  return files_read;

  end:
  if (fss_verbose) {
    fprintf(stderr, "[%d]: (%s) '%s': ", getpid(), "readNFiles", dirname);
    fprintf(stderr, "error: could not read files\n");
    print_error(response_code);
  }
  return -1;
}

int writeFile(const char* pathname, const char* dirname)
{
  // variables initialization
  char* abs_pathname = NULL;
  FILE* file = NULL;
  char* file_buffer = NULL;
  char* request = NULL;
  long response_code = RESPONSE_CODE_INIT;
  int removed_files = 0;

  if (!pathname || !strlen(pathname)) {
    errno = EINVAL;
    goto end;
  }
  // get absolute pathname
  if ((abs_pathname = realpath(pathname, NULL)) == NULL) {
    goto end;
  }

  // open file
  const size_t pathname_length = strlen(abs_pathname);
  if ((file = fopen(abs_pathname, "r")) == NULL) {
    goto end;
  }
  // calculate file size
  if (fseek(file, 0L, SEEK_END) == -1) {
    goto end;
  }
  ssize_t file_size;
  if ((file_size = ftell(file)) == -1) {
    goto end;
  }
  // rewind file position indicator
  errno = 0;
  rewind(file);
  if (errno) {
    goto end;
  }
  // read file content
  if ((file_buffer = calloc(1, sizeof(char) * file_size + 1)) == NULL) {
    goto end;
  }
  if (fread(file_buffer, sizeof(char), file_size, file) < (size_t)file_size) {
    if (ferror(file)) {
      goto end;
    }
  }
  // close file
  if (file && fclose(file) != 0) {
    goto end;
  }
  file = NULL;

  const size_t request_length = REQUEST_CODE_LENGTH + METADATA_LENGTH + pathname_length + METADATA_LENGTH + file_size + 1;
  if ((request = calloc(1, sizeof(char) * request_length)) == NULL) {
    goto end;
  }
  // assemble the request
  snprintf(request, request_length, "%d%010ld%s%010ld", WRITE_FILE, pathname_length, abs_pathname, file_size);
  free_item((void**)&abs_pathname);
  // append file content
  memcpy((request + strlen(request)), file_buffer, file_size);
  free_item((void**)&file_buffer);
  // send the request
  if (writen(fss_client_socket, request, request_length - 1) == -1) {
    goto end;
  }
  free_item((void**)&request);

  WAIT_FOR_RESPONSE();

  if (fss_verbose) {
    fprintf(stdout, "[%d]: (%s) '%s': ", getpid(), "writeFile", pathname);
    fprintf(stdout, "%ld bytes written\n", file_size);
  }
  // receive any removed files
  if ((removed_files = receive_files(dirname)) == -1) {
    response_code = INVALID_RESPONSE;
    goto end;
  }
  if (fss_verbose && removed_files) {
    fprintf(stdout, "[%d]: (%s) '%s': ", getpid(), "writeFile", pathname);
    fprintf(stdout, "%d file(s) removed from server\n", removed_files);
  }
  return 0;

  end:
  ;
  int myerrno = errno;
  free_item((void**)&abs_pathname);
  if (file != NULL) EXIT_ON_NZ(fclose(file));
  free_item((void**)&file_buffer);
  free_item((void**)&request);
  if (fss_verbose) {
    fprintf(stderr, "[%d]: (%s) '%s': ", getpid(), "writeFile", pathname);
    if (removed_files == -1) {
      fprintf(stderr, "error: could not receive removed file(s)\n");
    } else {
      fprintf(stderr, "error: could not write file\n");
    }
    print_error(response_code);
  }
  errno = myerrno;
  return -1;
}

int appendToFile(const char* pathname, void* buf, size_t size, const char* dirname)
{
  // variables initialization
  char* abs_pathname = NULL;
  char* request = NULL;
  long response_code = RESPONSE_CODE_INIT;
  int removed_files = 0;

  if (!pathname || !strlen(pathname) || !buf || !size) {
    errno = EINVAL;
    goto end;
  }
  // get absolute pathname
  if ((abs_pathname = realpath(pathname, NULL)) == NULL) {
    goto end;
  }

  const size_t pathname_length = strlen(abs_pathname);
  const size_t request_length = REQUEST_CODE_LENGTH + METADATA_LENGTH + pathname_length + METADATA_LENGTH + size + 1;
  if ((request = calloc(1, sizeof(char) * request_length)) == NULL) {
    goto end;
  }
  // assemble the request
  snprintf(request, request_length, "%d%010ld%s%010ld", APPEND_TO_FILE, pathname_length, abs_pathname, size);
  free_item((void**)&abs_pathname);
  // append content to request
  memcpy((request + strlen(request)), buf, size);
  // send the request
  if (writen(fss_client_socket, request, request_length - 1) == -1) {
    goto end;
  }
  free_item((void**)&request);

  WAIT_FOR_RESPONSE();

  if (fss_verbose) {
    fprintf(stdout, "[%d]: (%s) '%s': ", getpid(), "appendToFile", pathname);
    fprintf(stdout, "%ld bytes appended\n", size);
  }

  // receive any removed files
  if ((removed_files = receive_files(dirname)) == -1) {
    response_code = INVALID_RESPONSE;
    goto end;
  }
  if (fss_verbose && removed_files) {
    fprintf(stdout, "[%d]: (%s) '%s': ", getpid(), "appendToFile", pathname);
    fprintf(stdout, "%d file(s) removed from server\n", removed_files);
  }
  return 0;

  end:
  free_item((void**)&abs_pathname);
  free_item((void**)&request);
  if (fss_verbose) {
    fprintf(stderr, "[%d]: (%s) '%s': ", getpid(), "appendToFile", pathname);
    if (removed_files == -1) {
      fprintf(stderr, "error: could not receive removed file(s)\n");
    } else {
      fprintf(stderr, "error: could not append to file\n");
    }
    print_error(response_code);
  }
  return -1;
}

int lockFile(const char* pathname)
{
  // variables initialization
  char* abs_pathname = NULL;
  char* request = NULL;
  long response_code = RESPONSE_CODE_INIT;

  if (!pathname || !strlen(pathname)) {
    errno = EINVAL;
    goto end;
  }
  // get absolute pathname
  if ((abs_pathname = realpath(pathname, NULL)) == NULL) {
    goto end;
  }

  const size_t pathname_length = strlen(abs_pathname);
  const size_t request_length = REQUEST_CODE_LENGTH + METADATA_LENGTH + pathname_length + 1;
  if ((request = calloc(1, sizeof(char) * request_length)) == NULL) {
    goto end;
  }
  // assemble the request
  snprintf(request, request_length, "%d%010ld%s", LOCK_FILE, pathname_length, abs_pathname);
  free_item((void**)&abs_pathname);
  // send the request
  if (writen(fss_client_socket, request, request_length - 1) == -1) {
    goto end;
  }
  free_item((void**)&request);

  WAIT_FOR_RESPONSE();

  if (fss_verbose) {
    fprintf(stdout, "[%d]: (%s) '%s': ", getpid(), "lockFile", pathname);
    fprintf(stdout, "file successfully locked\n");
  }
  return 0;

  end:
  free_item((void**)&abs_pathname);
  free_item((void**)&request);
  if (fss_verbose) {
    fprintf(stderr, "[%d]: (%s) '%s': ", getpid(), "lockFile", pathname);
    fprintf(stderr, "error: could not lock file\n");
    print_error(response_code);
  }
  return -1;
}

int unlockFile(const char* pathname)
{
  // variables initialization
  char* abs_pathname = NULL;
  char* request = NULL;
  long response_code = RESPONSE_CODE_INIT;

  if (!pathname || !strlen(pathname)) {
    errno = EINVAL;
    goto end;
  }
  // get absolute pathname
  if ((abs_pathname = realpath(pathname, NULL)) == NULL) {
    goto end;
  }

  const size_t pathname_length = strlen(abs_pathname);
  const size_t request_length = REQUEST_CODE_LENGTH + METADATA_LENGTH + pathname_length + 1;
  if ((request = calloc(1, sizeof(char) * request_length)) == NULL) {
    goto end;
  }
  // assemble the request
  snprintf(request, request_length, "%d%010ld%s", UNLOCK_FILE, pathname_length, abs_pathname);
  free_item((void**)&abs_pathname);
  // send the request
  if (writen(fss_client_socket, request, request_length - 1) == -1) {
    goto end;
  }
  free_item((void**)&request);

  WAIT_FOR_RESPONSE();

  if (fss_verbose) {
    fprintf(stdout, "[%d]: (%s) '%s': ", getpid(), "unlockFile", pathname);
    fprintf(stdout, "file successfully unlocked\n");
  }
  return 0;

  end:
  free_item((void**)&abs_pathname);
  free_item((void**)&request);
  if (fss_verbose) {
    fprintf(stderr, "[%d]: (%s) '%s': ", getpid(), "unlockFile", pathname);
    fprintf(stderr, "error: could not unlock file\n");
    print_error(response_code);
  }
  return -1;
}

int closeFile(const char* pathname)
{
  // variables initialization
  char* abs_pathname = NULL;
  char* request = NULL;
  long response_code = RESPONSE_CODE_INIT;

  if (!pathname || !strlen(pathname)) {
    errno = EINVAL;
    goto end;
  }
  // get absolute pathname
  if ((abs_pathname = realpath(pathname, NULL)) == NULL) {
    goto end;
  }

  const size_t pathname_length = strlen(abs_pathname);
  const size_t request_length = REQUEST_CODE_LENGTH + METADATA_LENGTH + pathname_length + 1;
  if ((request = calloc(1, sizeof(char) * request_length)) == NULL) {
    goto end;
  }
  // assemble the request
  snprintf(request, request_length, "%d%010ld%s", CLOSE_FILE, pathname_length, abs_pathname);
  free_item((void*)&abs_pathname);
  // send the request
  if (writen(fss_client_socket, request, request_length - 1) == -1) {
    goto end;
  }
  free_item((void**)&request);

  WAIT_FOR_RESPONSE();

  if (fss_verbose) {
    fprintf(stdout, "[%d]: (%s) '%s': ", getpid(), "closeFile", pathname);
    fprintf(stdout, "file successfully closed\n");
  }
  return 0;

  end:
  free_item((void**)&abs_pathname);
  free_item((void**)&request);
  if (fss_verbose) {
    fprintf(stderr, "[%d]: (%s) '%s': ", getpid(), "closeFile", pathname);
    fprintf(stderr, "error: could not close file\n");
    print_error(response_code);
  }
  return -1;
}

int removeFile(const char* pathname)
{
  // variables initialization
  char* abs_pathname = NULL;
  char* request = NULL;
  long response_code = RESPONSE_CODE_INIT;

  if (!pathname || !strlen(pathname)) {
    errno = EINVAL;
    goto end;
  }
  // get absolute pathname
  if ((abs_pathname = realpath(pathname, NULL)) == NULL) {
    goto end;
  }

  const size_t pathname_length = strlen(abs_pathname);
  const size_t request_length = REQUEST_CODE_LENGTH + METADATA_LENGTH + pathname_length + 1;
  if ((request = calloc(1, sizeof(char) * request_length)) == NULL) {
    goto end;
  }
  // assemble the request
  snprintf(request, request_length, "%d%010ld%s", REMOVE_FILE, pathname_length, abs_pathname);
  free_item((void**)&abs_pathname);
  // send the request
  if (writen(fss_client_socket, request, request_length - 1) == -1) {
    goto end;
  }
  free_item((void**)&request);

  WAIT_FOR_RESPONSE();

  if (fss_verbose) {
    fprintf(stdout, "[%d]: (%s) '%s': ", getpid(), "removeFile", pathname);
    fprintf(stdout, "file successfully removed\n");
  }
  return 0;

  end:
  free_item((void**)&abs_pathname);
  free_item((void**)&request);
  if (fss_verbose) {
    fprintf(stderr, "[%d]: (%s) '%s': ", getpid(), "removeFile", pathname);
    fprintf(stderr, "error: could not remove file\n");
    print_error(response_code);
  }
  return -1;
}
