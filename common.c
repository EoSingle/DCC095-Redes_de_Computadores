#include "common.h"

// Imprime uma mensagem de erro
void log_error(const char *msg) {
    perror(msg); // perror exibe a mensagem de erro do sistema
}

// Imprime uma mensagem de informação
void log_info(const char *msg) {
    fprintf(stdout, "[INFO] %s\n", msg);
    fflush(stdout);
}

// Constrói uma mensagem de controle no formato "código payload"
void build_control_message(char *buffer, size_t buffer_size, int code, const char *payload) {
    if (payload != NULL && strlen(payload) > 0) {
        snprintf(buffer, buffer_size, "%d %s", code, payload);
    } else {
        snprintf(buffer, buffer_size, "%d ", code); 
    }
}

// Retorna 1 se o parsing for bem-sucedido, 0 caso contrário.
// Assume que o payload (se existir) é uma string simples até o final da linha.
int parse_message(const char *buffer, int *code, char *payload_buffer, size_t payload_buffer_size) {
    // Limpar o payload_buffer antes para evitar lixo se não houver payload
    if (payload_buffer_size > 0) {
        payload_buffer[0] = '\0';
    }

    int n_scanned = sscanf(buffer, "%d %[^\n]", code, payload_buffer);

    if (n_scanned >= 1) { // Pelo menos o código deve ser lido com sucesso.
        if (n_scanned == 1) {
            if (payload_buffer_size > 0) {
                 payload_buffer[0] = '\0';
            }
        }
        return 1;
    }
    
    n_scanned = sscanf(buffer, "%d", code);
    if (n_scanned == 1) {
        if (payload_buffer_size > 0) {
            payload_buffer[0] = '\0'; // Garante payload vazio
        }
        return 1;
    }

    return 0; // Falha no parsing
}
