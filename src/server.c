#include <posixver.h>

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/un.h>

#include <communication_protocol.h>
#include <error_handling.h>
#include <free_item.h>
#include <readnwrite.h>
#include <str2num.h>
#include <config_parser.h>
#include <storage.h>
#include <ubuffer.h>

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX 104
#endif

#define PIPE_BUFFER_LENGTH 4
#define END_OF_CONTENT "0000000000"
#define NO_CLIENT "0000"

#define SEND_RESPONSE(fd, code) \
  do { \
    snprintf(response_code_buffer, RESPONSE_CODE_LENGTH + 1, "%d", code); \
    EXIT_ON_NEG_ONE(writen(fd, response_code_buffer, RESPONSE_CODE_LENGTH)); \
  } while (0)

#define SEND_ERROR(fd) \
  do { \
    switch (errno) { \
      case ENOENT: \
        SEND_RESPONSE(fd, FILE_NOT_FOUND); \
	break; \
      case EEXIST: \
        SEND_RESPONSE(fd, ALREADY_EXISTS); \
	break; \
      case ENODATA: \
        SEND_RESPONSE(fd, NO_CONTENT); \
	break; \
      case EACCES: \
        SEND_RESPONSE(fd, FORBIDDEN); \
	break; \
      case ENOMEM: \
        SEND_RESPONSE(fd, OUT_OF_MEMORY); \
	break; \
      case EINVAL: \
        SEND_RESPONSE(fd, BAD_REQUEST); \
	break; \
      default: \
        SEND_RESPONSE(fd, INTERNAL_SERVER_ERROR); \
	break; \
    } \
  } while (0)

#define NOTIFY_PENDING_CLIENTS(pending_clients, response_code, pipe_buffer, pipe) \
  do { \
    while (pending_clients) { \
      SEND_RESPONSE(pending_clients->user, response_code); \
      snprintf(pipe_buffer, PIPE_BUFFER_LENGTH + 1, "%04d", pending_clients->user); \
      EXIT_ON_NEG_ONE(writen(pipe, pipe_buffer, PIPE_BUFFER_LENGTH)); \
      user_node_t* client = pending_clients; \
      pending_clients = pending_clients->next; \
      free_item((void**)&client); \
    } \
  } while (0)

#define SEND_FILE(fd, file, file_content) \
  do { \
    char* file_buffer; \
    EXIT_ON_NULL((file_buffer = calloc(1, sizeof(char) * (METADATA_LENGTH + strlen(file->pathname) + METADATA_LENGTH + file->size + 1)))); \
    snprintf(file_buffer, METADATA_LENGTH + strlen(file->pathname) + METADATA_LENGTH + 1, "%010ld%s%010ld", strlen(file->pathname), file->pathname, file->size); \
    memcpy(file_buffer + strlen(file_buffer), file_content, file->size); \
    EXIT_ON_NEG_ONE(writen(fd, file_buffer, (METADATA_LENGTH + strlen(file->pathname) + METADATA_LENGTH + file->size))); \
    free_item((void**)&file_buffer); \
  } while (0)

typedef struct {
  storage_t* storage;
  ubuffer_t* buffer;
  int pipe;
} worker_args_t;

volatile sig_atomic_t soft_exit = 0;
volatile sig_atomic_t hard_exit = 0;
// socket_name is global because of the cleanup function
static char* socket_name = NULL;

static int remove_socket(const char* socket);
static void cleanup(void);
static int signal_setup(void);
static void signal_handler(const int signal);
static int connection_setup(const char* socket_name, const int backlog);
static int max(const int a, const int b);
static int update_max(const fd_set set, const int max);
static char* request_payload(const int client, size_t* size);
static void* worker(void* args);

int main(int argc, char* argv[])
{
  // check args
  config_t server_config;
  switch (argc) {
    case 1:
      fprintf(stderr, "server: error: no input file, proceeding with the default server configuration...\n");
      // set default server configuration
      EXIT_ON_NEG_ONE(parser(NULL, &socket_name, &server_config));
      break;
    case 2:
      // parse config file
      EXIT_ON_NEG_ONE(parser(argv[1], &socket_name, &server_config));
      break;
    default:
      fprintf(stderr, "server: fatal error: too many arguments\n");
      fprintf(stderr, "Usage: %s config_file_path\n", argv[0]);
      return EXIT_FAILURE;
  }

  // remove socket file (if existing)
  EXIT_ON_NEG_ONE(remove_socket(socket_name));
  // register cleanup function
  EXIT_ON_NZ(atexit(cleanup));

  // signal setup
  EXIT_ON_NEG_ONE(signal_setup());

  // connection setup
  int server_socket;
  EXIT_ON_NEG_ONE(server_socket = connection_setup(socket_name, server_config.backlog));

  // create storage
  storage_t* storage;
  EXIT_ON_NULL(storage = storage_create(server_config.storage_max_file_number, server_config.storage_max_size));

  // create master-to-workers shared buffer
  ubuffer_t* m2w_buffer;
  EXIT_ON_NULL(m2w_buffer = ubuffer_create());
  // create workers-to-master shared pipe
  int w2m_pipe[2];
  EXIT_ON_NEG_ONE(pipe(w2m_pipe));

  // create worker thread pool
  worker_args_t worker_args = {.storage = storage, .buffer = m2w_buffer, .pipe = w2m_pipe[1]};
  pthread_t* workers;
  EXIT_ON_NULL(workers = malloc(sizeof(pthread_t) * server_config.worker_pool_size));
  for (long i = 0; i < server_config.worker_pool_size; i++) {
    EXIT_ON_NEG_ONE(pthread_create(&workers[i], NULL, worker, (void*)&worker_args));
  }

  // select fd set initialization
  fd_set current_fds, ready_fds;
  FD_ZERO(&current_fds);
  FD_SET(server_socket, &current_fds);
  FD_SET(w2m_pipe[0], &current_fds);
  int max_fd = max(server_socket, w2m_pipe[0]);
  int new_fd = 0;
  // keep track of connected clients
  ssize_t connected_clients = 0;
  // buffer to read fd from the pipe
  char pipe_buffer[PIPE_BUFFER_LENGTH + 1] = {0};

  while (!hard_exit) {
    // initialize ready fd set
    ready_fds = current_fds;

    // wait for a fd to be ready
    if (select(max_fd + 1, &ready_fds, NULL, NULL, NULL) == -1) {
      if (errno == EINTR) {
        if (soft_exit && !connected_clients) {
	  // no more pending jobs, exit
	  goto end;
	}
        continue;
      } else {
        perror("select");
        return EXIT_FAILURE;
      }
    }

    // check which fd is ready (not checking stdin (0), stdout (1), stderr (2))
    for (int i = 3; i < max_fd + 1; i++) {
      if (FD_ISSET(i, &ready_fds)) {
        // fd is ready

        if (i == server_socket) {
          // new connection
          EXIT_ON_NEG_ONE(new_fd = accept(server_socket, NULL, NULL));
          if (soft_exit) {
	    // reject connection immediately
	    EXIT_ON_NEG_ONE(close(new_fd));
          } else {
	    // add fd to listening set
            FD_SET(new_fd, &current_fds);
	    // update client count
	    connected_clients++;
            max_fd = max(new_fd, max_fd);
          }

        } else if (i == w2m_pipe[0]) {
	  // a worker has finished handling a request
	  // read client fd
	  EXIT_ON_NEG_ONE(readn(i, pipe_buffer, PIPE_BUFFER_LENGTH));
	  if ((new_fd = atol(pipe_buffer))) {
	    // add fd to listening set
	    FD_SET(new_fd, &current_fds);
	    max_fd = max(new_fd, max_fd);
	  } else {
	    // client left
	    connected_clients--;
	    if (!connected_clients && soft_exit) {
	      goto end;
	    }
	  }

        } else {
          // new request from connected client
          FD_CLR(i, &current_fds);
	  if (i == max_fd) {
	    // need to update max fd, but fds are not guaranteed to be generated in increasing order...
	    max_fd = update_max(current_fds, max_fd);
	  }
	  // enqueue ready fd into shared buffer
	  int* tmp;
	  EXIT_ON_NULL((tmp = malloc(sizeof(int))));
	  *tmp = i;
	  EXIT_ON_NEG_ONE(ubuffer_enqueue(m2w_buffer, (void*)tmp));
        }
      }
    }
  }

  end:
  // send termination message to workers
  for (long i = 0; i < server_config.worker_pool_size; i++) {
    int* term;
    EXIT_ON_NULL((term = calloc(1, sizeof(int))));
    EXIT_ON_NEG_ONE(ubuffer_enqueue(m2w_buffer, (void*)term));
  }
  // join worker threads
  for (long i = 0; i < server_config.worker_pool_size; i++) {
    EXIT_ON_NEG_ONE(pthread_join(workers[i], NULL));
  }
  free_item((void**)&workers);

  // close server socket
  EXIT_ON_NEG_ONE(close(server_socket));

  // destroy shared buffer
  EXIT_ON_NEG_ONE(ubuffer_destroy(m2w_buffer));
  // close shared pipe
  EXIT_ON_NEG_ONE(close(w2m_pipe[0]));
  EXIT_ON_NEG_ONE(close(w2m_pipe[1]));
  // destroy storage
  EXIT_ON_NEG_ONE(storage_destroy(storage));

  return 0;
}

/**
 * Remove the server socket if it exists
 *
 * Return 0 on success, -1 on error (set errno)
 */
static int remove_socket(const char* socket)
{
  if (unlink(socket) == -1 && errno != ENOENT) {
    // if errno == ENOENT, the file does not exist
    return -1;
  }
  return 0;
}

/**
 * Perform a cleanup
 */
static void cleanup(void)
{
  // remove the server socket
  EXIT_ON_NEG_ONE(remove_socket(socket_name));
  // free the socket name string
  free_item((void**)&socket_name);
}

/**
 * Performs the signal set up for the server
 *
 * Return 0 on success, -1 on error (set errno)
 */
static int signal_setup(void)
{
  // ignore SIGPIPE
  struct sigaction act;
  if (sigaction(SIGPIPE, NULL, &act) == -1) {
    // failed to get old handler for SIGPIPE
    return -1;
  }
  act.sa_handler = SIG_IGN;
  if (sigaction(SIGPIPE, &act, NULL) == -1) {
    // failed to ignore SIGPIPE
    return -1;
  }
  // set signal handler mask
  sigset_t handler_mask;
  sigemptyset(&handler_mask);
  sigaddset(&handler_mask, SIGINT);
  sigaddset(&handler_mask, SIGQUIT);
  sigaddset(&handler_mask, SIGHUP);

  // register signal handler function
  memset(&act, 0, sizeof(act));
  act = (struct sigaction){.sa_handler = signal_handler, .sa_mask = handler_mask};
  if (sigaction(SIGINT, &act, NULL) == -1) {
    return -1;
  }
  if (sigaction(SIGQUIT, &act, NULL) == -1) {
    return -1;
  }
  if (sigaction(SIGHUP, &act, NULL) == -1) {
    return -1;
  }
  return 0;
}

/**
 * Signal handler function, set the exit flags according to the received signal
 */
static void signal_handler(const int signal)
{
  switch (signal) {
    case SIGHUP:
      soft_exit = 1;
      break;
    case SIGINT:
      // fall through
    case SIGQUIT:
      // fall through
    default:
      hard_exit = 1;
      break;
  }
}

/**
 * Set up the server connection
 *
 * Return server socket (>=0) on success, -1 on error (set errno)
 */
static int connection_setup(const char* socket_name, const int backlog)
{
  if (!socket_name || !strlen(socket_name) || backlog < 0) {
    errno = EINVAL;
    return -1;
  }
  // create communication endpoint
  int server_socket;
  if ((server_socket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    return -1;
  }
  // initialize the address struct
  struct sockaddr_un address;
  memset(&address, '0', sizeof(address));
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, socket_name, sizeof(address.sun_path) - 1);
  // bind the address to a socket
  if (bind(server_socket, (struct sockaddr *)&address, sizeof(address)) == -1) {
    return -1;
  }
  // listen for connection on the created socket
  if (listen(server_socket, backlog) == -1) {
    return -1;
  }

  return server_socket;
}

/**
 * Return the maximum of two integers
 */
static int max(const int a, const int b)
{
  if (a > b) return a;
  return b;
}

/**
 * Determine the highest set fd less than max
 *
 * Return the new max (>0) on success, -1 on error
 */
static int update_max(const fd_set set, const int max)
{
  for (int i = (max-1); i >= 0; i--) {
    if (FD_ISSET(i, &set)) {
      return i;
    }
  }
  errno = ENOENT;
  return -1;
}

/**
 * Read the data contained in a request made by a client and return it in a newly allocated buffer
 *
 * Return a pointer to the data on sucess, NULL on error (set errno)
 */
static char* request_payload(const int client, size_t* size)
{
  // variables initialization
  char* content_buffer = NULL;

  if (client < 0) {
    errno = EINVAL;
    goto end;
  }
  // read the content length
  char content_length_buffer[METADATA_LENGTH + 1] = {0};
  if (readn(client, content_length_buffer, METADATA_LENGTH) == -1) {
    return NULL;
  }
  size_t content_length = atol(content_length_buffer);
  // give back the size to the caller
  if (size) {
    *size = content_length;
  }
  // read the content
  if ((content_buffer = calloc(1, sizeof(char) * (content_length + 1))) == NULL) {
    return NULL;
  }
  if (readn(client, content_buffer, content_length) == -1) {
    return NULL;
  }
  return content_buffer;

  end:
  free_item((void**)&content_buffer);
  return NULL;
}

/**
 * Function executed by worker threads in the threadpool
 */
static void* worker(void* args)
{
  // get worker arguments
  storage_t* storage = ((worker_args_t*)args)->storage;
  ubuffer_t* shared_buffer = ((worker_args_t*)args)->buffer;
  int master_pipe = ((worker_args_t*)args)->pipe;

  int* client_socket = NULL;

  while (1) {
    // variables initialization
    char* pathname = NULL;
    char request_code_buffer[REQUEST_CODE_LENGTH + 1] = {0};
    char response_code_buffer[RESPONSE_CODE_LENGTH + 1] = {0};
    char pipe_buffer[PIPE_BUFFER_LENGTH + 1] = {0};
    char pending_request = 0;
    user_node_t* pending_clients = NULL;
    file_t* removed_files = NULL;

    // get ready client from shared buffer
    EXIT_ON_NULL((client_socket = (int*)ubuffer_dequeue(shared_buffer)));
    if (!(*client_socket)) {
      // server termination message
      break;
    }

    // read the request code
    ssize_t bytes_read;
    if ((bytes_read = readn((*client_socket), request_code_buffer, REQUEST_CODE_LENGTH)) == -1) {
      if (errno == ECONNRESET) {
        bytes_read = 0;
      } else {
        perror("readn");
        exit(EXIT_FAILURE);
      }
    }

    if (bytes_read) {
      // successful read
      long request_code = atol(request_code_buffer);

      if (request_code != READ_N_FILES) {
        // read the file pathname
	EXIT_ON_NULL((pathname = request_payload(*client_socket, NULL)));
      }

      switch (request_code) {

        case OPEN_FILE:
	  {
	    // read flags
	    char flags_buffer[OPEN_FLAGS_LENGTH + 1] = {0};
	    EXIT_ON_NEG_ONE(readn(*client_socket, flags_buffer, OPEN_FLAGS_LENGTH));
	    long flags;
	    if (str2num(flags_buffer, &flags) != 0) {
	      // invalid flags
	      SEND_RESPONSE(*client_socket, BAD_REQUEST);
	    } else if (storage_open(storage, pathname, flags, &pending_clients, *client_socket) == -1) {
	      SEND_ERROR(*client_socket);
	    } else {
	      SEND_RESPONSE(*client_socket, OK);
	      if (pending_clients) {
	        // if there are clients waiting to lock removed files,
		// notify them that these files no longer exist
	        NOTIFY_PENDING_CLIENTS(pending_clients, FILE_NOT_FOUND, pipe_buffer, master_pipe);
	      }
	    }
	  }
	  break;

	case READ_FILE:
	  {
	    char* file_buffer;
	    size_t file_size;
	    // read file
	    if (storage_read(storage, pathname, (void**)&file_buffer, &file_size, *client_socket) == -1) {
	      SEND_ERROR(*client_socket);
	    } else {
	      SEND_RESPONSE(*client_socket, OK);
	      // send file
	      char* send_buffer;
	      EXIT_ON_NULL(send_buffer = calloc(1, sizeof(char) * (METADATA_LENGTH + file_size + 1)));
	      snprintf(send_buffer, METADATA_LENGTH + 1, "%010ld", file_size);
	      memcpy(send_buffer + METADATA_LENGTH, file_buffer, file_size);
	      free_item((void**)&file_buffer);
	      EXIT_ON_NEG_ONE(writen(*client_socket, send_buffer, METADATA_LENGTH + file_size));
	      free_item((void**)&send_buffer);
	    }
	  }
	  break;

	case READ_N_FILES:
	  {
	    // read N
	    char N_buffer[METADATA_LENGTH + 1] = {0};
	    EXIT_ON_NEG_ONE(readn(*client_socket, N_buffer, METADATA_LENGTH));
	    long N;
	    if (str2num(N_buffer, &N) != 0) {
	      // invalid N
	      SEND_RESPONSE(*client_socket, BAD_REQUEST);
	    } else {
	      char* files_buffer;
	      size_t files_size;
	      if (storage_read_many(storage, N, (void**)&files_buffer, &files_size, *client_socket) == -1) {
	        SEND_ERROR(*client_socket);
	      } else {
	        SEND_RESPONSE(*client_socket, OK);
		// send the files
		EXIT_ON_NEG_ONE(writen(*client_socket, files_buffer, files_size));
		free_item((void**)&files_buffer);
		EXIT_ON_NEG_ONE(writen(*client_socket, END_OF_CONTENT, METADATA_LENGTH));
	      }
	    }
	  }
	  break;

	case WRITE_FILE:
	  {
	    if (!storage_can_write(storage, pathname, *client_socket)) {
	      SEND_RESPONSE(*client_socket, FORBIDDEN);
	      // discard the rest of the request
	      char* content_buffer;
	      EXIT_ON_NULL((content_buffer = request_payload(*client_socket, NULL)));
	      free_item((void**)&content_buffer);
	      break;
	    }
	  }
	  // fall through
	case APPEND_TO_FILE:
	  {
	    // get the new content to append
	    char* new_content;
	    size_t new_content_size;
	    EXIT_ON_NULL((new_content = request_payload(*client_socket, &new_content_size)));
	    // append the new content
	    if (storage_append(storage, pathname, new_content, new_content_size, &pending_clients, &removed_files, *client_socket) == -1) {
	      SEND_ERROR(*client_socket);
	    } else {
	      SEND_RESPONSE(*client_socket, OK);
	      if (pending_clients) {
	        // if there are clients waiting to lock removed files,
		// notify them that these files no longer exist
	        NOTIFY_PENDING_CLIENTS(pending_clients, FILE_NOT_FOUND, pipe_buffer, master_pipe);
	      }
	      // send the removed files to the client
	      file_t* current_file;
	      while ((current_file = removed_files)) {
		// copy the file content
		char* content_buffer;
		EXIT_ON_NULL((content_buffer = storage_copy(current_file, 0)));
		SEND_FILE(*client_socket, current_file, content_buffer);
		free_item((void**)&content_buffer);
		removed_files = removed_files->next;
		file_dealloc(current_file);
	      }
	      // tell the client there are no more removed files to read
	      EXIT_ON_NEG_ONE(writen(*client_socket, END_OF_CONTENT, METADATA_LENGTH));
	    }
	    free_item((void**)&new_content);
	  }
	  break;

	case LOCK_FILE:
	  {
	    switch (storage_lock(storage, pathname, *client_socket)) {
	      case -1:
	        SEND_ERROR(*client_socket);
		break;
	      case -2:
	        // the client will not be sent back to the master
	        pending_request = 1;
		break;
	      default:
	        SEND_RESPONSE(*client_socket, OK);
	    }
	  }
	  break;

	case UNLOCK_FILE:
	  {
	    int pending_client;
	    if (storage_unlock(storage, pathname, &pending_client, *client_socket) == -1) {
	      SEND_ERROR(*client_socket);
	    } else {
	      SEND_RESPONSE(*client_socket, OK);
	      if (pending_client) {
	        // a client waiting to lock the file has finally acquired the lock
		SEND_RESPONSE(pending_client, OK);
		// send the pending client back to the master
		snprintf(pipe_buffer, PIPE_BUFFER_LENGTH + 1, "%04d", pending_client);
		EXIT_ON_NEG_ONE(writen(master_pipe, pipe_buffer, PIPE_BUFFER_LENGTH));
	      }
	    }
	  }
	  break;

	case CLOSE_FILE:
	  {
	    if (storage_close(storage, pathname, *client_socket) == -1) {
	      SEND_ERROR(*client_socket);
	    } else {
	      SEND_RESPONSE(*client_socket, OK);
	    }
	  }
	  break;

	case REMOVE_FILE:
	  {
	    if (storage_remove(storage, pathname, &pending_clients, *client_socket) == -1) {
	      SEND_ERROR(*client_socket);
	    } else {
	      SEND_RESPONSE(*client_socket, OK);
	      if (pending_clients) {
	        // if there are clients waiting to lock removed files,
		// notify them that these files no longer exist
	        NOTIFY_PENDING_CLIENTS(pending_clients, FILE_NOT_FOUND, pipe_buffer, master_pipe);
	      }
	    }
	  }
	  break;

	default:
	  {
	    SEND_RESPONSE(*client_socket, BAD_REQUEST);
	  }
      }

      // free the resources allocated to handle the request
      free_item((void**)&pathname);

      if (!pending_request) {
        // send the client back to master
        snprintf(pipe_buffer, PIPE_BUFFER_LENGTH + 1, "%04d", *client_socket);
	EXIT_ON_NEG_ONE(writen(master_pipe, pipe_buffer, PIPE_BUFFER_LENGTH));
      }

    } else {
      // unsuccessful read, the client left

      // release the lock on all files locked by the client
      // and get a list of the first clients waiting to lock these files
      EXIT_ON_NEG_ONE(storage_user_exit(storage, &pending_clients, *client_socket));

      // close the connection
      EXIT_ON_NEG_ONE(close(*client_socket));

      if (pending_clients) {
        // notify the "first in line" clients, waiting to lock the files locked by the client that just exited,
	// that they have finally acquired the lock
        NOTIFY_PENDING_CLIENTS(pending_clients, OK, pipe_buffer, master_pipe);
      }

      // tell the master that the client left
      EXIT_ON_NEG_ONE(writen(master_pipe, NO_CLIENT, PIPE_BUFFER_LENGTH));
    }
    free_item((void**)&client_socket);

  }
  free_item((void**)&client_socket);

  return NULL;
}
