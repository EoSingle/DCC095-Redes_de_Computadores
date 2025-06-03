#include "common.h"

void log_error(const char *msg) {
    perror(msg); // perror imprime a mensagem de erro do sistema
}

void log_info(const char *msg) {
    fprintf(stdout, "[INFO] %s\n", msg);
    fflush(stdout);
}

void build_control_message(char *buffer, size_t buffer_size, int code, const char *payload) {
    if (payload != NULL && strlen(payload) > 0) {
        snprintf(buffer, buffer_size, "%d %s", code, payload);
    } else {
        snprintf(buffer, buffer_size, "%d ", code); // Adiciona espaço para consistência no parsing
    }
}

// Retorna 1 se o parsing for bem-sucedido, 0 caso contrário.
// Assume que o payload é uma string simples até o final da linha.
int parse_message(const char *buffer, int *code, char *payload_buffer, size_t payload_buffer_size) {
    int n_scanned = sscanf(buffer, "%d %[^\n]", code, payload_buffer); // Lê até o final da linha no payload
    if (n_scanned >= 1) { // Pelo menos o código deve ser lido
        if (n_scanned == 1) { // Sem payload
            payload_buffer[0] = '\0'; // Garante payload vazio
        }
        return 1;
    }
    return 0; // Falha no parsing
}
