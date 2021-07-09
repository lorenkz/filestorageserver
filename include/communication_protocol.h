#ifndef COMMUNICATION_PROTOCOL_H
#define COMMUNICATION_PROTOCOL_H

/**
 * This header describes the client/server communication protocol specifications.
 */

/**
 * Standard lengths for making requests and responses
 */
#define METADATA_LENGTH 10
#define REQUEST_CODE_LENGTH 1
#define RESPONSE_CODE_LENGTH 1
#define OPEN_FLAGS_LENGTH 1

/**
 * Request codes used to make a request to the server
 */
#define OPEN_FILE 1
#define READ_FILE 2
#define READ_N_FILES 3
#define WRITE_FILE 4
#define APPEND_TO_FILE 5
#define LOCK_FILE 6
#define UNLOCK_FILE 7
#define CLOSE_FILE 8
#define REMOVE_FILE 9

/**
 * Response codes used to send a response to the client
 */
#define RESPONSE_CODE_INIT 0
#define OK 1
#define FILE_NOT_FOUND 2
#define ALREADY_EXISTS 3
#define NO_CONTENT 4
#define FORBIDDEN 5
#define OUT_OF_MEMORY 6
#define INTERNAL_SERVER_ERROR 7
#define BAD_REQUEST 8
#define INVALID_RESPONSE 9

/**
 * Flags used to make an openFile request
 */
#define O_CREATE (1<<0)
#define O_LOCK (1<<1)
#define O_NOFLAG (0)
#define IS_SET(X, Y) ((X) & (Y))

#endif
