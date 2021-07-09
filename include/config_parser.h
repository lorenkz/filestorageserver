#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

typedef struct {
  long worker_pool_size;
  long storage_max_file_number;
  long storage_max_size;
  long backlog;
} config_t;

/**
 * Parse a configuration file and set parameter values accordingly,
 * setting default values to unparsed options
 *
 * Return 0 on success, -1 on error (set errno)
 */
int parser(const char* config_file_path, char** server_socket, config_t* server_config);

#endif
