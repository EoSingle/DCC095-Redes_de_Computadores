#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     // For read, write, close
#include <arpa/inet.h>  // For inet_addr, htons, etc.
#include <sys/socket.h> // For socket, bind, listen, accept, connect
#include <netinet/in.h> // For sockaddr_in

#define MAX_MSG_SIZE 500    // Maximum message size
#define SERVER_BACKLOG 5    // Number of pending connections the listen call can queue
#define MAX_PIDS_LENGTH 50  // Maximum length for a Peer ID (PidS)

// --- Control Messages ---
#define REQ_CONNPEER 20
#define RES_CONNPEER 21
#define REQ_DISCPEER 22
#define REQ_CONNSEN 23
#define RES_CONNSEN 24
#define REQ_DISCSEN 25

// --- Data Messages ---
#define REQ_CHECKALERT 36
#define RES_CHECKALERT 37
#define REQ_SENSLOC 38
#define RES_SENSLOC 39
#define REQ_SENSSTATUS 40
#define RES_SENSSTATUS 41
#define REQ_LOCLIST 42
#define RES_LOCLIST 43

// --- Confirmation and Error Messages ---
#define OK_MSG 0
#define OK_SUCCESSFUL_DISCONNECT 1
#define OK_SUCCESSFUL_CREATE 2
#define OK_SUCCESSFUL_UPDATE 3

#define ERROR_MSG 255
#define PEER_LIMIT_EXCEEDED 1
#define PEER_NOT_FOUND 2

#define INVALID_PAYLOAD_ERROR 3
#define SENSOR_ID_ALREADY_EXISTS_ERROR 4
#define INVALID_MSG_CODE_ERROR 5

#define SENSOR_LIMIT_EXCEEDED 9
#define SENSOR_NOT_FOUND 10

// --- Utility Functions ---
void log_error(const char *msg);
void log_info(const char *msg);

void build_control_message(char *buffer, size_t buffer_size, int code, const char *payload);
int parse_message(const char *buffer, int *code, char *payload_buffer, size_t payload_buffer_size);

#endif // COMMON_H
