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

// Funções utilitárias (exemplo, podem ser adicionadas depois)
void log_error(const char *msg);
void log_info(const char *msg);

#endif // COMMON_H