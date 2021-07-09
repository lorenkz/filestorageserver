#include <config_parser.h>

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>

#include <fss_defaults.h>
#include <error_handling.h>
#include <free_item.h>
#include <str2num.h>

#define BUFSIZE 1024

int parser(const char* config_file_path, char** server_socket, config_t* server_config)
{
  // variables initialization
  FILE* config_file = NULL;
  char* line = NULL;

  if (!server_socket || !server_config) {
    errno = EINVAL;
    goto end;
  }

  char SOCKET_NAME_flag = 0,
       WORKER_POOL_SIZE_flag = 0,
       STORAGE_MAX_FILE_NUMBER_flag = 0,
       STORAGE_MAX_SIZE_flag = 0,
       BACKLOG_flag = 0;

  // used to verify that the socket pathname fits into the array
  struct sockaddr_un sizecheck;

  if (config_file_path) {
    // open file
    if ((config_file = fopen(config_file_path, "r")) == NULL) {
      goto end;
    }
    // allocate line buffer
    if ((line = calloc(1, sizeof(char) * BUFSIZE)) == NULL) {
      goto end;
    }

    // read each line
    while (fgets(line, BUFSIZE, config_file) != NULL) {

      // check if buffer contains the whole line
      char* newline = NULL;
      if (!(newline = strchr(line, '\n')) && !(newline = strchr(line, EOF))) {
        // increase line size
	char* realloc_line = NULL;
        if ((realloc_line = realloc(line, sizeof(char) * (2 * BUFSIZE))) == NULL) {
	  goto end;
	}
	line = realloc_line;
	rewind(config_file);
	continue;
      }
      // "cut" the rest of the string
      *newline = '\0';

      // first validity check of the line
      size_t line_length = newline - line + 1;
      if (line_length < 4) {
        // the line is empty or is not enough long for a valid command
        continue;
      }

      // strip the line, act similiarly to insertion sort...
      size_t stripped_line_length = 0;
      for (size_t i = 0; i < line_length; i++) {
        if (line[i] == ' ' || line[i] == '\t' || line[i] == '\r') {
	  // skip whitespaces
	  continue;
	}
        if (line[i] == '#') {
	  // skip comments
          line[stripped_line_length++] = '\0';
          break;
        }
        // insert in stripped line every other character including '\0'
        line[stripped_line_length++] = line[i];
      }

      // second validity check of the line
      char* equalsign = NULL;
      if ((equalsign = strchr(line, '=')) == NULL || line[0] == '=' || *(equalsign+1) == '\n') {
        // line is not valid
        continue;
      }
      // stripped line now respect pattern key=value
      *equalsign = '\0';
      equalsign++;
      // 'line' now points to key, 'equalsign' points to value

      // switch for key value
      long value;
      // socket name insertion has been deprecated for security reasons:
      // (the server unlink it at launch and exit, it can be any file)
      /*
      if (strncmp(line, "SOCKET_NAME", 11) == 0) {
        if (strlen(line) <= sizeof(sizecheck.sun_path)) {
          if ((*server_socket = calloc(1, sizeof(char) * (strlen(line) + 1))) == NULL) {
            goto end;
          }
        } else {
          if ((*server_socket = calloc(1, sizeof(char) * sizeof(sizecheck.sun_path))) == NULL) {
            goto end;
          }
        }
        strncpy(server_socket, equalsign, sizeof(sizecheck.sun_path) - 1);
	SOCKET_NAME_flag = 1;
      }
      */
      if (strncmp(line, "WORKER_POOL_SIZE", 16) == 0) {
        if (str2num(equalsign, &value) != 0 || value < 1) {
          fprintf(stderr, "error: %s: bad config file format\n", "WORKER_POOL_SIZE");
          continue;
        }
	server_config->worker_pool_size = value;
	WORKER_POOL_SIZE_flag = 1;
      }
      if (strncmp(line, "STORAGE_MAX_FILE_NUMBER", 23) == 0) {
        if (str2num(equalsign, &value) != 0 || value < 1) {
          fprintf(stderr, "error: %s: bad config file format\n", "STORAGE_MAX_FILE_NUMBER");
          continue;
        }
	server_config->storage_max_file_number = value;
	STORAGE_MAX_FILE_NUMBER_flag = 1;
      }
      if (strncmp(line, "STORAGE_MAX_SIZE", 16) == 0) {
        if (str2num(equalsign, &value) != 0 || value < 1) {
          fprintf(stderr, "error: %s: bad config file format\n", "STORAGE_MAX_SIZE");
          continue;
        }
	server_config->storage_max_size = value;
	STORAGE_MAX_SIZE_flag = 1;
      }
      if (strncmp(line, "BACKLOG", 7) == 0) {
        if (str2num(equalsign, &value) != 0 || value < 1) {
          fprintf(stderr, "error: %s: bad config file format\n", "BACKLOG");
          continue;
        }
	server_config->backlog = value;
	BACKLOG_flag = 1;
      }
    } // while

    free_item((void**)&line);
    // close file
    if (fclose(config_file) != 0) {
      goto end;
    }
    config_file = NULL;
  }

  // if not parsed, set server defaults
  if (!SOCKET_NAME_flag) {
    if (strlen(DEF_SOCKET_NAME) <= sizeof(sizecheck.sun_path)) {
      if ((*server_socket = calloc(1, sizeof(char) * (strlen(DEF_SOCKET_NAME) + 1))) == NULL) {
        goto end;
      }
      memcpy(*server_socket, DEF_SOCKET_NAME, strlen(DEF_SOCKET_NAME));
    } else {
      if ((*server_socket = calloc(1, sizeof(char) * sizeof(sizecheck.sun_path))) == NULL) {
        goto end;
      }
      memcpy(*server_socket, DEF_SOCKET_NAME, sizeof(sizecheck.sun_path) - 1);
    }
  }
  if (!WORKER_POOL_SIZE_flag) {
    server_config->worker_pool_size = DEF_WORKER_POOL_SIZE;
  }
  if (!STORAGE_MAX_FILE_NUMBER_flag) {
    server_config->storage_max_file_number = DEF_STORAGE_MAX_FILE_NUMBER;
  }
  if (!STORAGE_MAX_SIZE_flag) {
    server_config->storage_max_size = DEF_STORAGE_MAX_SIZE;
  }
  if (!BACKLOG_flag) {
    server_config->backlog = DEF_BACKLOG;
  }

  return 0;

  end:
  if (config_file) EXIT_ON_NZ(fclose(config_file));
  free_item((void**)&line);
  free_item((void**)server_socket);
  return -1;
}
