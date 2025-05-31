// sensor.c
#include "common.h" // Inclui MAX_MSG_SIZE e outras definições/funções

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <ip_servidor> <porta_servidor>\n", argv[0]);
        // O projeto final também espera um LocId como argumento inicial, mas não para a conexão em si [cite: 67]
        exit(EXIT_FAILURE);
    }

    char *server_ip = argv[1];
    int server_port = atoi(argv[2]);

    int client_fd;
    struct sockaddr_in server_addr;
    char buffer[MAX_MSG_SIZE] = {0};
    char log_msg[100];

    // 1. Criar o socket do cliente
    // AF_INET para IPv4, SOCK_STREAM para TCP, 0 para protocolo padrão (TCP) [cite: 105, 109]
    if ((client_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        log_error("Falha ao criar socket do cliente");
        exit(EXIT_FAILURE);
    }
    log_info("Socket do cliente criado.");

    // Configurar a estrutura de endereço do servidor para conexão
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    // Converter o endereço IP de string para formato de rede
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        log_error("Endereço IP inválido ou não suportado");
        close(client_fd);
        exit(EXIT_FAILURE);
    }

    // 2. Conectar ao servidor
    if (connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        sprintf(log_msg, "Falha ao conectar ao servidor %s:%d", server_ip, server_port);
        log_error(log_msg);
        close(client_fd);
        exit(EXIT_FAILURE);
    }
    sprintf(log_msg, "Conectado ao servidor %s:%d.", server_ip, server_port);
    log_info(log_msg);

    // 3. Trocar dados (exemplo: enviar uma mensagem e receber uma resposta)
    const char *message_to_server = "Ola Servidor, sou um sensor!";
    if (write(client_fd, message_to_server, strlen(message_to_server)) < 0) {
        log_error("Falha ao enviar mensagem para o servidor");
    } else {
        log_info("Mensagem enviada ao servidor.");
    }

    ssize_t bytes_read = read(client_fd, buffer, MAX_MSG_SIZE - 1);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0'; // Garantir terminação nula
        sprintf(log_msg, "Mensagem recebida do servidor: %s", buffer);
        log_info(log_msg);
    } else if (bytes_read == 0) {
        log_info("Servidor desconectou (socket fechado).");
    } else {
        log_error("Erro ao ler do servidor.");
    }

    // 4. Fechar o socket
    log_info("Fechando socket do cliente.");
    close(client_fd); // O cliente encerra a comunicação e sua execução [cite: 81] (após comando 'kill')

    return 0;
}