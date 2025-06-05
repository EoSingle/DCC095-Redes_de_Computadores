#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // Para read, write, close
#include <arpa/inet.h> // Para inet_addr, htons, etc.
#include <sys/socket.h> // Para socket, bind, listen, accept, connect
#include <netinet/in.h> // Para sockaddr_in

#define MAX_MSG_SIZE 500 // Tamanho máximo das mensagens
#define SERVER_BACKLOG 5 // Número de conexões pendentes que o listen pode enfileirar
#define MAX_PIDS_LENGTH 50 // Tamanho máximo para um PidS

// Mensagens de controle
#define REQ_CONNPEER 20
#define RES_CONNPEER 21
#define REQ_DISCPEER 22
#define REQ_CONNSEN 23
#define RES_CONNSEN 24
#define REQ_DISCSEN 25

// Mensagens de dados
#define REQ_CHECKALERT 36
#define RES_CHECKALERT 37
#define REQ_SENSLOC 38
#define RES_SENSLOC 39
#define REQ_SENSSTATUS 40
#define RES_SENSSTATUS 41
#define REQ_LOCLIST 42
#define RES_LOCLIST 43

// Mensagens de erro ou Confirmação
#define OK_MSG 0
#define OK_SUCCESSFUL_DISCONNECT 1
#define OK_SUCCESSFUL_CREATE 2
#define OK_SUCCESSFUL_UPDATE 3

#define INVALID_PAYLOAD_ERROR 4
#define SENSOR_ID_ALREADY_EXISTS_ERROR 5
#define INVALID_MSG_CODE_ERROR 6

#define ERROR_MSG 255
#define PEER_LIMIT_EXCEEDED 1
#define PEER_NOT_FOUND 2
#define SENSOR_LIMIT_EXCEEDED 9
#define SENSOR_NOT_FOUND 10

// Funções utilitárias
void log_error(const char *msg);
void log_info(const char *msg);

void build_control_message(char *buffer, size_t buffer_size, int code, const char *payload);
int parse_message(const char *buffer, int *code, char *payload_buffer, size_t payload_buffer_size);

#endif // COMMON_H