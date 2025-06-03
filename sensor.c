// sensor.c
#include "common.h" // Inclui MAX_MSG_SIZE e outras definições/funções

// IDs recebidos dos servidores
char my_id_from_ss[MAX_PIDS_LENGTH] = ""; // Ou um tamanho adequado para IdC
char my_id_from_sl[MAX_PIDS_LENGTH] = "";
int initial_loc_id = -1;

// Função auxiliar para conectar a um servidor e fazer o handshake REQ_CONNSEN/RES_CONNSEN
// Retorna o socket fd em sucesso, -1 em falha. Armazena o ID recebido.
int connect_and_get_id(const char *server_type_name, const char *server_ip, int server_port, int loc_id, char *id_storage) {
    int sockfd;
    struct sockaddr_in serv_addr;
    char buffer[MAX_MSG_SIZE + 1];
    char log_msg[150];

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        sprintf(log_msg, "Falha ao criar socket para %s", server_type_name);
        log_error(log_msg);
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {
        sprintf(log_msg, "Endereço IP inválido para %s", server_type_name);
        log_error(log_msg);
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        sprintf(log_msg, "Falha ao conectar ao servidor %s (%s:%d)", server_type_name, server_ip, server_port);
        log_error(log_msg);
        close(sockfd);
        return -1;
    }
    sprintf(log_msg, "Conectado ao servidor %s (%s:%d).", server_type_name, server_ip, server_port);
    log_info(log_msg);

    // Enviar REQ_CONNSEN(LocId)
    char payload_locid[10];
    sprintf(payload_locid, "%d", loc_id);
    build_control_message(buffer, sizeof(buffer), REQ_CONNSEN, payload_locid);
    sprintf(log_msg, "Enviando REQ_CONNSEN(%s) para %s", payload_locid, server_type_name);
    log_info(log_msg);
    if (write(sockfd, buffer, strlen(buffer)) < 0) {
        sprintf(log_msg, "Falha ao enviar REQ_CONNSEN para %s", server_type_name);
        log_error(log_msg);
        close(sockfd);
        return -1;
    }

    // Aguardar RES_CONNSEN(IdC)
    ssize_t bytes_read = read(sockfd, buffer, MAX_MSG_SIZE);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        int code;
        char received_id[MAX_PIDS_LENGTH]; // Ajustar tamanho se necessário
        if (parse_message(buffer, &code, received_id, sizeof(received_id))) {
            if (code == RES_CONNSEN) {
                strncpy(id_storage, received_id, MAX_PIDS_LENGTH -1);
                id_storage[MAX_PIDS_LENGTH-1] = '\0';
                // Impressão conforme PDF: "SS New ID: IdC" ou "SL New ID: IdC" [cite: 76]
                sprintf(log_msg, "%s New ID: %s", server_type_name, id_storage);
                log_info(log_msg);
                return sockfd;
            } else if (code == ERROR_MSG) {
                // Imprimir descrição do erro [cite: 72]
                int error_code_payload = atoi(received_id); // 'payload' aqui é o código do erro
                if (error_code_payload == SENSOR_LIMIT_EXCEEDED_ERROR) {
                     sprintf(log_msg, "%s respondeu ERROR(%02d): Sensor limit exceeded", server_type_name, error_code_payload);
                } else {
                     sprintf(log_msg, "%s respondeu ERROR(%02d): Código de erro desconhecido %s", server_type_name, error_code_payload, received_id);
                }
                log_info(log_msg);
            } else {
                sprintf(log_msg, "%s respondeu com mensagem inesperada: Code=%d", server_type_name, code);
                log_info(log_msg);
            }
        } else { /* Erro de parse */ }
    } else { /* Erro de read ou desconexão */ }
    
    close(sockfd);
    return -1;
}


int main(int argc, char *argv[]) {
    if (argc < 6) { // Ex: ./sensor <ip_ss> <porta_ss> <ip_sl> <porta_sl> <loc_id>
        fprintf(stderr, "Uso: %s <ip_servidor_ss> <porta_ss> <ip_servidor_sl> <porta_sl> <loc_id_inicial>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *ss_ip = argv[1];
    int ss_port = atoi(argv[2]);
    char *sl_ip = argv[3];
    int sl_port = atoi(argv[4]);
    initial_loc_id = atoi(argv[5]);

    char log_msg[150];

    // Validar LocId [cite: 67]
    if (initial_loc_id < 1 || initial_loc_id > 10) {
        log_info("Invalid argument (LocId deve ser entre 1 e 10). Encerrando."); // [cite: 68]
        exit(EXIT_FAILURE);
    }
    sprintf(log_msg, "Sensor iniciado para LocId: %d", initial_loc_id);
    log_info(log_msg);

    int ss_fd = -1;
    int sl_fd = -1;

    // Conectar e obter ID do Servidor de Status (SS)
    ss_fd = connect_and_get_id("SS", ss_ip, ss_port, initial_loc_id, my_id_from_ss);
    if (ss_fd < 0) {
        log_info("Não foi possível obter ID do Servidor de Status. Encerrando.");
        exit(EXIT_FAILURE);
    }

    // Conectar e obter ID do Servidor de Localização (SL)
    sl_fd = connect_and_get_id("SL", sl_ip, sl_port, initial_loc_id, my_id_from_sl);
    if (sl_fd < 0) {
        log_info("Não foi possível obter ID do Servidor de Localização. Encerrando.");
        if (ss_fd > 0) close(ss_fd); // Fechar conexão com SS se SL falhar
        exit(EXIT_FAILURE);
    }

    log_info("Handshake inicial com SS e SL concluído.");
    sprintf(log_msg, "IDs recebidos: SS_ID=%s, SL_ID=%s", my_id_from_ss, my_id_from_sl);
    log_info(log_msg);

    // Loop principal do cliente para enviar comandos (Check failure, locate, diagnose, kill)
    // Este loop precisará usar select() para monitorar STDIN e os sockets ss_fd, sl_fd
    // para respostas assíncronas ou notificações (embora o protocolo seja maioritariamente req/res).
    // Por enquanto, apenas um placeholder:
    log_info("Digite comandos ('check failure', 'locate <SensID>', 'diagnose <LocId>', 'kill' para sair):");
    char command_line[MAX_MSG_SIZE];
    while (fgets(command_line, sizeof(command_line), stdin) != NULL) {
        command_line[strcspn(command_line, "\n")] = 0; // Remover newline

        if (strcmp(command_line, "kill") == 0) {
            log_info("Comando 'kill' recebido. Iniciando desconexão...");
            // Enviar REQ_DISCSEN para SS e SL
            // ... (lógica a ser implementada) ...
            break; 
        } else if (strncmp(command_line, "check failure", strlen("check failure")) == 0) {
            // Enviar REQ_SENSSTATUS para SS
            // ... (lógica a ser implementada) ...
        }
        // Adicionar outros comandos
        log_info("Digite comandos ('check failure', 'locate <SensID>', 'diagnose <LocId>', 'kill' para sair):");
    }

    if (ss_fd > 0) close(ss_fd);
    if (sl_fd > 0) close(sl_fd);
    log_info("Sensor encerrado.");
    return 0;
}