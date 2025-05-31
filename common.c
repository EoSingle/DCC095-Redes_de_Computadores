#include "common.h"

void log_error(const char *msg) {
    perror(msg); // perror imprime a mensagem de erro do sistema
}

void log_info(const char *msg) {
    fprintf(stdout, "[INFO] %s\n", msg);
    fflush(stdout);
}