#include <posixver.h>

#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <fss_api.h>
#include <fss_defaults.h>
#include <communication_protocol.h>
#include <error_handling.h>
#include <free_item.h>
#include <str2num.h>

#define HELP_MESSAGE "- Client for File Storage Server -\n\nUsage: %s [options] ...\n\nOptions:\n   -h                     Print a list of all options and exit.\n   -f filename            Specify the socket name to connect to.\n   -w dirname[,n]         Send recursively up to n files in 'dirname'\n                          (no limits if n=0 or unspecified).\n   -W file1[,file2] ...   List of file names to be written to the server.\n   -D dirname             Folder where the evicted files are written.\n   -r file1[,file2] ...   List of file names to be read from the server.\n   -R [n]                 Read 'n' random files currently stored on the server\n                          (no limits if n=0 or unspecified).\n   -d dirname             Folder where to write files read by the server\n                          with the -r and -R options.\n   -t time                Time in milliseconds between sending\n                          two consecutive requests to the server.\n   -l file1[,file2] ...   List of file names on which to acquire the mutual exclusion.\n   -u file1[,file2] ...   List of file names on which to release the mutual exclusion.\n   -c file1[,file2] ...   List of files to be removed from the server if any.\n   -p                     Enables standard output printouts for each operation.\n"
#define RETRY_DELAY 200
#define TIMEOUT 5

static ssize_t w_command(char* w_arg, const char* D_directory, const long open_flags);
static ssize_t visit_n_write(const char* visit_dir, const char* save_dir, const long up_to, const long open_flags);
static int W_command(char* W_files, const char* D_directory, const int open_flags);
static int r_command(char* r_arg, const char* d_directory);
static int R_command(const char* R_arg, const char* d_directory);
static int l_command(char* l_files);
static int u_command(char* u_files);
static int c_command(char* c_files);

int main(int argc, char* argv[])
{
  // check args
  if (argc == 1) {
    fprintf(stderr, "client: fatal error: missing operand\n");
    fprintf(stderr, "client: Try '%s -h' for more information.\n", argv[0]);
    return EXIT_FAILURE;
  }

  // flags to keep track of the parsed options
  char h_flag = 0,
       w_flag = 0,
       W_flag = 0,
       D_flag = 0,
       r_flag = 0,
       R_flag = 0,
       d_flag = 0,
       l_flag = 0,
       u_flag = 0,
       c_flag = 0;
  // counters to check the number of times certain options are specified
  int f_flag = 0,
      p_flag = 0;

  // optarg is a pointer to the original argv array
  // so there's no need to allocate lots of strings,
  // we just keep pointers to option arguments
  char* f_arg = NULL,
      * w_arg = NULL,
      * W_arg = NULL,
      * D_arg = NULL,
      * r_arg = NULL,
      * R_arg = NULL,
      * d_arg = NULL,
      * t_arg = NULL,
      * l_arg = NULL,
      * u_arg = NULL,
      * c_arg = NULL;

  int opt;
  // disable getopt error messages
  opterr = 0;
  while ((opt = getopt(argc, argv, ":hf:w:W:D:r:R:d:t:l:u:c:p")) != -1) {

    // in case of missing optional argument, continue parsing
    // (arguments cannot start with a hyphen '-')
    if (optarg && optarg[0] == '-') {
      optind--;
      continue;
    }

    switch (opt) {
      case 'h':
        h_flag = 1;
	break;
      case 'f':
        if (!f_flag) {
	  // if the option is specified more than once, only the first occurrence is counted
	  // (for every other option, each time the option is specified its argument is overwritten)
	  f_arg = optarg;
	}
        f_flag++;
        break;
      case 'w':
        w_arg = optarg;
	w_flag = 1;
        break;
      case 'W':
        W_arg = optarg;
        W_flag = 1;
        break;
      case 'D':
        D_arg = optarg;
        D_flag = 1;
        break;
      case 'r':
        r_arg = optarg;
        r_flag = 1;
        break;
      case 'R':
        R_arg = optarg;
	R_flag = 1;
        break;
      case 'd':
        d_arg = optarg;
        d_flag = 1;
        break;
      case 't':
        t_arg = optarg;
        break;
      case 'l':
        l_arg = optarg;
	l_flag = 1;
        break;
      case 'u':
        u_arg = optarg;
        u_flag = 1;
        break;
      case 'c':
        c_arg = optarg;
        c_flag = 1;
        break;
      case 'p':
	fss_verbose = 1;
        p_flag++;
        break;
      case ':':
        // missing option argument
        switch (optopt) {
	  case 'R':
	    // R option does not have the optional argument
	    R_arg = NULL;
	    R_flag = 1;
	    break;
	  default:
	    fprintf(stderr, "[%d]: ", getpid());
            fprintf(stderr, "error: option '-%c' is missing a required argument\n", optopt);
	}
	break;
      default: /* '?' */
	fprintf(stderr, "[%d]: ", getpid());
        fprintf(stderr, "error: unrecognized command-line option '-%c'\n", optopt);
    }
  }

  // print help
  if (h_flag) {
    printf(HELP_MESSAGE, argv[0]);
    return 0;
  }

  // f (socket name) option check
  switch (f_flag) {
    case 0:
      // the socket name was not specified
      f_arg = DEF_SOCKET_NAME;
      fprintf(stderr, "[%d]: ", getpid());
      fprintf(stderr, "error: socket name not specified with '-f' option, using default (%s)\n", DEF_SOCKET_NAME);
      break;
    case 1:
      // the socket name was specified once
      break;
    default:
      // the socket name was specified more than once
      fprintf(stderr, "[%d]: ", getpid());
      fprintf(stderr, "error: option '-f' cannot be repeated, using first socket name specified\n");
  }
  // t (time) option check
  long time_between_requests = 0;
  if (t_arg && str2num(t_arg, &time_between_requests) != 0) {
    fprintf(stderr, "[%d]: ", getpid());
    fprintf(stderr, "error: unable to parse time value of '-t' option, setting default value (0)\n");
    time_between_requests = 0;
  }
  // p (verbose) option check
  if (p_flag > 1) {
    fprintf(stderr, "[%d]: ", getpid());
    fprintf(stderr, "error: option '-p' cannot be repeated\n");
  }
  // D option check
  if (D_flag && !w_flag && !W_flag) {
    fprintf(stderr, "[%d]: ", getpid());
    fprintf(stderr, "error: cannot use '-D' option without '-w' or '-W' options\n");
  }
  // d option check
  if (d_flag && !r_flag && !R_flag) {
    fprintf(stderr, "[%d]: ", getpid());
    fprintf(stderr, "error: cannot use '-d' option without '-r' or '-R' options\n");
  }

  // commands execution
  time_t current_time = time(NULL);
  int msec = RETRY_DELAY;
  struct timespec abstime = {.tv_sec = current_time + TIMEOUT, .tv_nsec = 0};

  if (openConnection(f_arg, msec, abstime) == -1) {
    fprintf(stderr, "[%d]: ", getpid());
    perror("openConnection");
    return EXIT_FAILURE;
  }
  if (w_flag) {
    w_command(w_arg, D_arg, (O_CREATE|O_LOCK));
    sleep_for(msec);
  }
  if (W_flag) {
    W_command(W_arg, D_arg, (O_CREATE|O_LOCK));
    sleep_for(msec);
  }
  if (r_flag) {
    r_command(r_arg, d_arg);
    sleep_for(msec);
  }
  if (R_flag) {
    R_command(R_arg, d_arg);
    sleep_for(msec);
  }
  if (l_flag) {
    l_command(l_arg);
    sleep_for(msec);
  }
  if (u_flag) {
    u_command(u_arg);
    sleep_for(msec);
  }
  if (c_flag) {
    c_command(c_arg);
    sleep_for(msec);
  }
  if (closeConnection(f_arg) == -1) {
    fprintf(stderr, "[%d]: ", getpid());
    perror("closeConnection");
    return EXIT_FAILURE;
  }

  return 0;
}

static ssize_t w_command(char* w_arg, const char* D_directory, const long open_flags)
{
  char* token,
      * save_ptr,
      * w_dirname;
  w_dirname = strtok_r(w_arg, ",", &save_ptr);

  token = strtok_r(NULL, ",", &save_ptr);
  long w_n = 0;
  if (token && str2num(token, &w_n) != 0) {
    fprintf(stderr, "[%d]: ", getpid());
    fprintf(stderr, "error: unable to parse n value of '-w' option, setting default value (0)\n");
    w_n = 0;
  }
  return visit_n_write(w_dirname, D_directory, w_n, open_flags);
}

static ssize_t visit_n_write(const char* visit_dir, const char* save_dir, const long up_to, const long open_flags)
{
  // variables initialization
  DIR* dir = NULL;
  char* pathname = NULL;
  long processed_files = 0;

  if ((dir = opendir(visit_dir)) == NULL) {
    goto end;
  }
  struct dirent* entry;
  while (((errno = 0, entry = readdir(dir)) != NULL) && (up_to <= 0 || processed_files < up_to)) {

    // skip current and parent directories
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    // get entry pathname
    if ((pathname = calloc(1, sizeof(char) * (strlen(visit_dir) + 1 + strlen(entry->d_name) + 1))) == NULL) {
      goto end;
    }
    strncpy(pathname, visit_dir, strlen(visit_dir));
    if (visit_dir[strlen(visit_dir)] != '/') {
      strncat(pathname, "/", strlen("/"));
    }
    strncat(pathname, entry->d_name, strlen(entry->d_name));

    struct stat statbuf;
    if(stat(pathname, &statbuf) == -1) {
      fprintf(stderr, "[%d]: ", getpid());
      perror("stat");
      goto end;
    }
    if (S_ISDIR(statbuf.st_mode)) {
      // entry is a directory

      // recursively visit and write files in subdirectories
      ssize_t subdir_count;
      if ((subdir_count = visit_n_write(pathname, save_dir, up_to - processed_files, open_flags)) == -1) {
        goto end;
      }
      processed_files += subdir_count;

    } else {
      // entry is a file

      if (openFile(pathname, open_flags) == -1) {
        fprintf(stderr, "[%d]: ", getpid());
        perror("openFile");
        if (errno != ECANCELED) {
	  goto end;
	}
      } else {
        if (writeFile(pathname, save_dir) == -1) {
          fprintf(stderr, "[%d]: ", getpid());
          perror("writeFile");
	  if (errno != ECANCELED) {
	    goto end;
	  }
	  // the file is empty, it must be removed
	  if (removeFile(pathname) == -1) {
            fprintf(stderr, "[%d]: ", getpid());
            perror("removeFile");
	    // could not remove the file, at least try to close it
	    if (closeFile(pathname) == -1) {
              fprintf(stderr, "[%d]: ", getpid());
              perror("closeFile");
	      if (errno != ECANCELED) {
	        goto end;
	      }
	    }
	  }
        } else if (closeFile(pathname) == -1) {
          fprintf(stderr, "[%d]: ", getpid());
          perror("closeFile");
	  if (errno != ECANCELED) {
	    goto end;
	  }
        }
      }
      processed_files++;
    }
    free_item((void**)&pathname);
  }
  if (errno != 0) {
    fprintf(stderr, "[%d]: ", getpid());
    perror("readdir");
    goto end;
  }
  if (closedir(dir) == -1) {
    fprintf(stderr, "[%d]: ", getpid());
    perror("closedir");
    goto end;
  }
  dir = NULL;
  return processed_files;

  end:
  if (dir != NULL) EXIT_ON_NEG_ONE(closedir(dir));
  free_item((void**)&pathname);
  return -1;
}

static int W_command(char* W_files, const char* D_directory, const int open_flags)
{
  char* current_file,
      * save_ptr;
  current_file = strtok_r(W_files, ",", &save_ptr);
  while (current_file) {
    if (openFile(current_file, open_flags) == -1) {
      fprintf(stderr, "[%d]: ", getpid());
      perror("openFile");
      if (errno != ECANCELED) {
        return -1;
      }
    } else {
      if (writeFile(current_file, D_directory) == -1) {
        fprintf(stderr, "[%d]: ", getpid());
        perror("writeFile");
        if (errno != ECANCELED) {
          return -1;
        }
        // the file is empty, it must be removed
        if (removeFile(current_file) == -1) {
          fprintf(stderr, "[%d]: ", getpid());
          perror("removeFile");
	  // could not remove the file, at least try to close it
	  if (closeFile(current_file) == -1) {
            fprintf(stderr, "[%d]: ", getpid());
            perror("closeFile");
	    if (errno != ECANCELED) {
              return -1;
	    }
	  }
	}
      } else if (closeFile(current_file) == -1) {
        fprintf(stderr, "[%d]: ", getpid());
        perror("closeFile");
        if (errno != ECANCELED) {
          return -1;
	}
      }
    }
    current_file = strtok_r(NULL, ",", &save_ptr);
  }
  return 0;
}

static int r_command(char* r_files, const char* d_directory)
{
  // variables initialization
  char* file_content = NULL;

  char* current_file,
      * save_ptr;
  size_t file_size;

  current_file = strtok_r(r_files, ",", &save_ptr);
  while (current_file) {
    if (openFile(current_file, O_NOFLAG) == -1) {
      fprintf(stderr, "[%d]: ", getpid());
      perror("openFile");
    } else if (readFile(current_file, (void**)&file_content, &file_size) == -1) {
      fprintf(stderr, "[%d]: ", getpid());
      perror("readFile");
      if (errno != ECANCELED) {
        goto end;
      }
      // could not read the file, it must be closed
      if (closeFile(current_file) == -1) {
        fprintf(stderr, "[%d]: ", getpid());
        perror("closeFile");
        if (errno != ECANCELED) {
	  goto end;
	}
      }
    } else {
      if (d_directory) {
        if (store_file(current_file, file_content, file_size, d_directory) == -1) {
          fprintf(stderr, "[%d]: ", getpid());
          perror("store_file");
	  goto end;
        }
      } else if (fss_verbose) {
        fprintf(stdout, "[%d]: ", getpid());
        fprintf(stdout, "the read file (%s) was thrown away\n", current_file);
      }
      free_item((void**)&file_content);
      if (closeFile(current_file) == -1) {
        fprintf(stderr, "[%d]: ", getpid());
        perror("closeFile");
        if (errno != ECANCELED) {
	  goto end;
	}
      }
    }
    current_file = strtok_r(NULL, ",", &save_ptr);
  }
  return 0;

  end:
  free_item((void**)&file_content);
  return -1;
}

static int R_command(const char* R_arg, const char* d_directory)
{
  long R_n = 0;
  if (R_arg && str2num(R_arg, &R_n) != 0) {
    fprintf(stderr, "[%d]: ", getpid());
    fprintf(stderr, "error: unable to parse n value of '-R' option, setting default value (0)\n");
    R_n = 0;
  }
  if (readNFiles(R_n, d_directory) == -1) {
    fprintf(stderr, "[%d]: ", getpid());
    perror("readNFiles");
    return -1;
  }
  return 0;
}

static int l_command(char* l_files)
{
  char* current_file,
      * save_ptr;
  current_file = strtok_r(l_files, ",", &save_ptr);
  while (current_file) {
    if (lockFile(current_file) == -1) {
      fprintf(stderr, "[%d]: ", getpid());
      perror("lockFile");
      if (errno != ECANCELED) {
        return -1;
      }
    }
    current_file = strtok_r(NULL, ",", &save_ptr);
  }
  return 0;
}

static int u_command(char* u_files)
{
  char* current_file,
      * save_ptr;
  current_file = strtok_r(u_files, ",", &save_ptr);
  while (current_file) {
    if (unlockFile(current_file) == -1) {
      fprintf(stderr, "[%d]: ", getpid());
      perror("unlockFile");
      if (errno != ECANCELED) {
        return -1;
      }
    }
    current_file = strtok_r(NULL, ",", &save_ptr);
  }
  return 0;
}

static int c_command(char* c_files)
{
  char* current_file,
      * save_ptr;
  current_file = strtok_r(c_files, ",", &save_ptr);
  while (current_file) {
    if (removeFile(current_file) == -1) {
      fprintf(stderr, "[%d]: ", getpid());
      perror("removeFile");
      if (errno != ECANCELED) {
        return -1;
      }
    }
    current_file = strtok_r(NULL, ",", &save_ptr);
  }
  return 0;
}
