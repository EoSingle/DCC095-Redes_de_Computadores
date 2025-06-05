// sensor.c
#include "common.h" // Inclui MAX_MSG_SIZE e outras definições/funções
#include <ctype.h> // Para isdigit

// ID recebidos dos servidores
char my_sensor_id[MAX_PIDS_LENGTH] = "";
int initial_loc_id = -1;

// IDs confirmados pelos servidores
int connect_and_get_id(const char *server_type_name, const char *server_ip, int server_port, 
                       int loc_id, // loc_id é o initial_loc_id
                       char *id_storage, // Onde armazenaremos o ID confirmado pelo servidor
                       const char *sensor_id_to_send) { // Passando o ID global como parâmetro
    int sockfd;
    struct sockaddr_in serv_addr;
    char buffer[MAX_MSG_SIZE + 1]; // Buffer para construir e receber mensagens
    char log_msg[150];             // Buffer para logs
    char payload_for_req_connsen[MAX_MSG_SIZE]; // Buffer para o payload composto de REQ_CONNSEN

    // 1. Criação e conexão do socket 
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

    // 2. Enviar REQ_CONNSEN com payload "ID_Sensor,LocId"
    char loc_id_as_string[12]; // Suficiente para um int
    sprintf(loc_id_as_string, "%d", loc_id);

    // Construir o payload composto: "ID_SENSOR,LOCID"
    snprintf(payload_for_req_connsen, sizeof(payload_for_req_connsen), "%s,%s", 
             sensor_id_to_send, // Usar o ID passado como parâmetro
             loc_id_as_string);
    
    build_control_message(buffer, sizeof(buffer), REQ_CONNSEN, payload_for_req_connsen); // REQ_CONNSEN é código 23 
    
    sprintf(log_msg, "Enviando REQ_CONNSEN (Payload: %s) para %s", payload_for_req_connsen, server_type_name);
    log_info(log_msg);
    
    if (write(sockfd, buffer, strlen(buffer)) < 0) {
        sprintf(log_msg, "Falha ao enviar REQ_CONNSEN para %s", server_type_name);
        log_error(log_msg);
        close(sockfd);
        return -1;
    }

    // 3. Aguardar e Processar RES_CONNSEN(IdSen), onde IdSen deve ser o ID_Global_Sensor
    ssize_t bytes_read = read(sockfd, buffer, MAX_MSG_SIZE);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        int code;
        char received_payload[MAX_PIDS_LENGTH]; // Para armazenar o IdSen recebido

        if (parse_message(buffer, &code, received_payload, sizeof(received_payload))) {
            if (code == RES_CONNSEN) { // RES_CONNSEN é código 24 
                // O payload 'received_payload' deve ser o ID Global que o servidor está confirmando.
                sprintf(log_msg, "%s respondeu com RES_CONNSEN: Code=%d, Payload='%s'", server_type_name, code, received_payload);
                strncpy(id_storage, received_payload, MAX_PIDS_LENGTH - 1);
                id_storage[MAX_PIDS_LENGTH - 1] = '\0';
                    
                // "SS New ID: IdC" ou "SL New ID: IdC" 
                sprintf(log_msg, "%s New ID: %s", server_type_name, id_storage);
                log_info(log_msg);
                return sockfd; // Sucesso, retorna o socket descriptor
            } else if (code == ERROR_MSG) {
                // Imprimir descrição do erro 
                int error_code_payload = atoi(received_payload); // 'payload' aqui é o código do erro
                // A tabela de mensagens de erro está na página 6.
                // O erro "Sensor limit exceeded" é código 09. 
                if (error_code_payload == SENSOR_LIMIT_EXCEEDED) { 
                    sprintf(log_msg, "%s respondeu ERROR(%02d): Sensor limit exceeded", server_type_name, error_code_payload);
                } else {
                    sprintf(log_msg, "%s respondeu ERROR(%02d): Código de erro %s", server_type_name, error_code_payload, received_payload);
                }
                log_info(log_msg);
            } else if (code == INVALID_PAYLOAD_ERROR) {
                sprintf(log_msg, "%s respondeu ERROR(%02d): Payload inválido", server_type_name, INVALID_PAYLOAD_ERROR);
                log_info(log_msg);
            } else if (code == SENSOR_ID_ALREADY_EXISTS_ERROR) {
                sprintf(log_msg, "%s respondeu ERROR(%02d): Sensor ID já existe", server_type_name, SENSOR_ID_ALREADY_EXISTS_ERROR);
                log_info(log_msg);
            } else {
                sprintf(log_msg, "%s respondeu com mensagem inesperada: Code=%d, Payload='%s'", server_type_name, code, received_payload);
                log_info(log_msg);
            }
        } else {
            sprintf(log_msg, "Falha ao parsear resposta do servidor %s.", server_type_name);
            log_error(log_msg);
        }
    } else if (bytes_read == 0) {
        sprintf(log_msg, "Servidor %s desconectou antes de enviar RES_CONNSEN.", server_type_name);
        log_info(log_msg);
    } else { // bytes_read < 0
        sprintf(log_msg, "Falha ao ler resposta de RES_CONNSEN do servidor %s", server_type_name);
        log_error(log_msg);
    }
    
    // Se chegou aqui, algo deu errado (não retornou sockfd com sucesso)
    close(sockfd);
    return -1; // Falha
}


int main(int argc, char *argv[]) {
    if (argc < 7) { // Ex: ./sensor <ip_ss> <porta_ss> <ip_sl> <porta_sl> <ID_SENSOR> <loc_id>
        fprintf(stderr, "Uso: %s <ip_servidor_ss> <porta_ss> <ip_servidor_sl> <porta_sl> <ID_SENSOR> <loc_id_inicial>\n", argv[0]);
        fprintf(stderr, "Exemplo: ./sensor 127.0.0.1 60000 127.0.0.1 61000 1234567890 5\n");
        exit(EXIT_FAILURE);
    }

    char *ss_ip = argv[1];
    int ss_port = atoi(argv[2]);
    char *sl_ip = argv[3];
    int sl_port = atoi(argv[4]);
    // ID Global do Sensor
    strncpy(my_sensor_id, argv[5], sizeof(my_sensor_id) - 1);
    my_sensor_id[sizeof(my_sensor_id) - 1] = '\0'; // Garantir terminação nula
    initial_loc_id = atoi(argv[6]);

    char log_msg[150];

    // Validar o ID do Sensor
    if (strlen(my_sensor_id) == 10) { // Exemplo de validação de tamanho
        for (size_t i = 0; i < 10; ++i) {
            if (!isdigit((unsigned char)my_sensor_id[i])) {
                fprintf(stderr, "Erro: ID_SENSOR deve conter apenas dígitos numéricos.\n");
                exit(EXIT_FAILURE);
            }
        }
    } else {
        fprintf(stderr, "Erro: ID_SENSOR deve ter exatamente 10 caracteres numéricos.\n");
        exit(EXIT_FAILURE);
    }

    sprintf(log_msg, "Sensor iniciado com ID: %s, LocId: %d", my_sensor_id, initial_loc_id);
    log_info(log_msg);

    int ss_fd = -1;
    int sl_fd = -1;
    char id_confirmado_ss[MAX_PIDS_LENGTH]; // Para my_id_from_ss
    char id_confirmado_sl[MAX_PIDS_LENGTH]; // Para my_id_from_sl

    // Conectar e obter ID do Servidor de Status (SS)
    ss_fd = connect_and_get_id("SS", ss_ip, ss_port, initial_loc_id, id_confirmado_ss, my_sensor_id);
    if (ss_fd < 0) {
        log_info("Não foi possível obter ID do Servidor de Status. Encerrando.");
        exit(EXIT_FAILURE);
    }

    // Conectar e obter ID do Servidor de Localização (SL)
    sl_fd = connect_and_get_id("SL", sl_ip, sl_port, initial_loc_id, id_confirmado_sl, my_sensor_id);
    if (sl_fd < 0) {
        log_info("Não foi possível obter ID do Servidor de Localização. Encerrando.");
        if (ss_fd > 0) close(ss_fd);
        exit(EXIT_FAILURE);
    }

    char my_global_sensor_id[MAX_PIDS_LENGTH] = ""; // Limpar antes de usar
    // Checar se os IDs confirmados são iguais
    if (strcmp(id_confirmado_ss, id_confirmado_sl) != 0) {
        log_error("IDs confirmados pelos servidores SS e SL não coincidem. Encerrando.");
        close(ss_fd);
        close(sl_fd);
        exit(EXIT_FAILURE);
    } else {
        // Armazenar os IDs confirmados
        strncpy(my_global_sensor_id, id_confirmado_ss, sizeof(my_global_sensor_id) - 1);
        my_global_sensor_id[sizeof(my_global_sensor_id) - 1] = '\0'; // Garantir terminação nula
    }

    log_info("Handshake inicial com SS e SL concluído.");
    sprintf(log_msg, "ID Global do Sensor %s confirmado por SS e SL.", my_sensor_id);
    log_info(log_msg);

    // Loop principal do cliente para enviar comandos (Check failure, locate, diagnose, kill)
    // Este loop precisará usar select() para monitorar STDIN e os sockets ss_fd, sl_fd
    // para respostas assíncronas ou notificações (embora o protocolo seja maioritariamente req/res).
    // Por enquanto, apenas um placeholder:
    log_info("Digite comandos ('check failure', 'locate <SensID>', 'diagnose <LocId>', 'kill' para sair):");
    char command_line[MAX_MSG_SIZE];
    while (fgets(command_line, sizeof(command_line), stdin) != NULL) {
        command_line[strcspn(command_line, "\n")] = 0; // Remover newline
        char log_msg_sensor[150]; // Buffer para logs específicos do sensor

        if (strcmp(command_line, "kill") == 0) {
            log_info("Comando 'kill' recebido. Iniciando desconexão dos servidores SS e SL...");
            char msg_buffer[MAX_MSG_SIZE];
            char response_buffer[MAX_MSG_SIZE];

            // 1. Desconectar do Servidor SS
            if (ss_fd > 0 && strlen(id_confirmado_ss) > 0) { // id_confirmado_ss é o ID de Slot do SS
                sprintf(log_msg_sensor, "Enviando REQ_DISCSEN (Slot ID: %s) para SS...", id_confirmado_ss);
                log_info(log_msg_sensor);
                build_control_message(msg_buffer, sizeof(msg_buffer), REQ_DISCSEN, id_confirmado_ss);

                if (write(ss_fd, msg_buffer, strlen(msg_buffer)) < 0) {
                    log_error("Falha ao enviar REQ_DISCSEN para SS");
                } else {
                    // Aguardar resposta do SS
                    ssize_t bytes_read = read(ss_fd, response_buffer, sizeof(response_buffer) - 1);
                    if (bytes_read > 0) {
                        response_buffer[bytes_read] = '\0';
                        int code;
                        char payload[MAX_MSG_SIZE];
                        if (parse_message(response_buffer, &code, payload, sizeof(payload))) {
                            sprintf(log_msg_sensor, "SS respondeu: Code=%d, Payload='%s'", code, payload);
                            log_info(log_msg_sensor);

                            if (code == OK_MSG && atoi(payload) == OK_SUCCESSFUL_DISCONNECT) {
                                log_info("SS Successful disconnect"); // Conforme PDF
                            } else if (code == ERROR_MSG && atoi(payload) == SENSOR_NOT_FOUND) {
                                log_info("SS respondeu ERROR(10): Sensor not found"); // Conforme PDF
                            } else if (code == INVALID_MSG_CODE_ERROR && atoi(payload) == INVALID_MSG_CODE_ERROR) {
                                log_info("SS respondeu ERROR(11): Invalid message code"); // Conforme PDF
                            } else {
                                sprintf(log_msg_sensor, "SS respondeu com msg inesperada para REQ_DISCSEN: Code=%d, Payload='%s'", code, payload);
                                log_info(log_msg_sensor);
                            }
                        } else {
                            log_error("Falha ao parsear resposta do SS para REQ_DISCSEN");
                        }
                    } else if (bytes_read == 0) {
                        log_info("SS desconectou antes de responder ao REQ_DISCSEN.");
                    } else {
                        log_error("Falha ao ler resposta do SS para REQ_DISCSEN");
                    }
                }
                close(ss_fd); // Fechar socket com SS independentemente da resposta
                ss_fd = -1;   // Marcar como fechado
                
            } else {
                log_info("Não conectado ao SS ou sem ID do SS para enviar REQ_DISCSEN.");
            }

            // 2. Desconectar do Servidor SL
            if (sl_fd > 0 && strlen(id_confirmado_sl) > 0) { // id_confirmado_sl é o ID de Slot do SL
                sprintf(log_msg_sensor, "Enviando REQ_DISCSEN (Slot ID: %s) para SL...", id_confirmado_sl);
                log_info(log_msg_sensor);
                build_control_message(msg_buffer, sizeof(msg_buffer), REQ_DISCSEN, id_confirmado_sl);

                if (write(sl_fd, msg_buffer, strlen(msg_buffer)) < 0) {
                    log_error("Falha ao enviar REQ_DISCSEN para SL");
                } else {
                    // Aguardar resposta do SL
                    ssize_t bytes_read = read(sl_fd, response_buffer, sizeof(response_buffer) - 1);
                    if (bytes_read > 0) {
                        response_buffer[bytes_read] = '\0';
                        int code;
                        char payload[MAX_MSG_SIZE];
                        if (parse_message(response_buffer, &code, payload, sizeof(payload))) {
                            if (code == OK_MSG && atoi(payload) == OK_SUCCESSFUL_DISCONNECT) {
                                log_info("SL Successful disconnect"); // Conforme PDF
                            } else if (code == ERROR_MSG && atoi(payload) == SENSOR_NOT_FOUND) {
                                log_info("SL respondeu ERROR(10): Sensor not found"); // Conforme PDF
                            } else {
                                sprintf(log_msg_sensor, "SL respondeu com msg inesperada para REQ_DISCSEN: Code=%d, Payload='%s'", code, payload);
                                log_info(log_msg_sensor);
                            }
                        } else {
                            log_error("Falha ao parsear resposta do SL para REQ_DISCSEN");
                        }
                    } else if (bytes_read == 0) {
                        log_info("SL desconectou antes de responder ao REQ_DISCSEN.");
                    } else {
                        log_error("Falha ao ler resposta do SL para REQ_DISCSEN");
                    }
                }
                close(sl_fd); // Fechar socket com SL independentemente da resposta
                sl_fd = -1;   // Marcar como fechado
            } else {
                log_info("Não conectado ao SL ou sem ID do SL para enviar REQ_DISCSEN.");
            }

            log_info("Desconexão dos servidores solicitada. Encerrando sensor.");
            break; // Sai do loop de comandos e permite que o programa sensor termine
        } else if (strcmp(command_line, "check failure") == 0) {
            if (ss_fd > 0 && strlen(my_global_sensor_id) > 0) {
                sprintf(log_msg_sensor, "Comando 'check failure' recebido. Enviando REQ_SENSSTATUS (ID: %s) para SS...", my_global_sensor_id);
                log_info(log_msg_sensor);

                char msg_buffer[MAX_MSG_SIZE];
                char response_buffer[MAX_MSG_SIZE];

                build_control_message(msg_buffer, sizeof(msg_buffer), REQ_SENSSTATUS, my_global_sensor_id);
                if (write(ss_fd, msg_buffer, strlen(msg_buffer)) < 0) {
                    log_error("Falha ao enviar REQ_SENSSTATUS para SS");
                } else {
                    // Aguardar resposta RES_SENSSTATUS ou ERROR do SS
                    ssize_t bytes_read = read(ss_fd, response_buffer, sizeof(response_buffer) - 1);
                    if (bytes_read > 0) {
                        response_buffer[bytes_read] = '\0';
                        int code; char payload_loc_id_str[MAX_MSG_SIZE];
                        if (parse_message(response_buffer, &code, payload_loc_id_str, sizeof(payload_loc_id_str))) {
                            if (code == RES_SENSSTATUS) {
                                int loc_id = atoi(payload_loc_id_str);
                                // Imprimir conforme formato do PDF (página 10)
                                if (loc_id == 1) sprintf(log_msg_sensor, "Alert received from location: %d (Norte)", loc_id);
                                else if (loc_id == 2) sprintf(log_msg_sensor, "Alert received from location: %d (Norte)", loc_id); // PDF p.10 tem Norte para 1 e Sul para 2. Ajuste aqui.
                                else if (loc_id == 3) sprintf(log_msg_sensor, "Alert received from location: %d (Norte)", loc_id); // E Leste para 3
                                else if (loc_id == 4) sprintf(log_msg_sensor, "Alert received from location: %d (Norte)", loc_id); // E Oeste para 4
                                // CORREÇÃO DA LÓGICA DE IMPRESSÃO DAS ÁREAS CONFORME PÁGINA 4:
                                // Área Norte: localização 1, 2 e 3
                                // Área Sul: localização 4 e 5
                                // Área Leste: localização 6 e 7
                                // Área Oeste: localização 8, 9 e 10
                                // O exemplo da p.10 para o cliente tem uma numeração diferente da definição de áreas. Usaremos a definição da p.4.
                                else if (loc_id >=1 && loc_id <=3) sprintf(log_msg_sensor, "Alert received from location: %d (Norte)", loc_id);
                                else if (loc_id >=4 && loc_id <=5) sprintf(log_msg_sensor, "Alert received from location: %d (Sul)", loc_id);
                                else if (loc_id >=6 && loc_id <=7) sprintf(log_msg_sensor, "Alert received from location: %d (Leste)", loc_id);
                                else if (loc_id >=8 && loc_id <=10) sprintf(log_msg_sensor, "Alert received from location: %d (Oeste)", loc_id);
                                else if (loc_id == 0) sprintf(log_msg_sensor, "Status normal reportado para o sensor (LocID: %d).", loc_id); // Interpretação para normal
                                else sprintf(log_msg_sensor, "Alerta recebido de localização desconhecida ou inválida: %d", loc_id);
                                log_info(log_msg_sensor);

                            } else if (code == ERROR_MSG && atoi(payload_loc_id_str) == SENSOR_NOT_FOUND) {
                                log_info("Sensor not found"); // Conforme PDF
                            } else {
                                sprintf(log_msg_sensor, "SS respondeu com msg inesperada para REQ_SENSSTATUS: Code=%d, Payload='%s'", code, payload_loc_id_str);
                                log_info(log_msg_sensor);
                            }
                        } else { log_error("Falha ao parsear resposta do SS para REQ_SENSSTATUS"); }
                    } else { log_error("Falha ao ler resposta do SS para REQ_SENSSTATUS ou desconexão"); }
                }
            } else {
                log_info("Não conectado ao SS ou sem ID do SS para enviar REQ_SENSSTATUS.");
            }
        }
        // Adicionar outros comandos
        log_info("Digite comandos ('check failure', 'locate <SensID>', 'diagnose <LocId>', 'kill' para sair):");
    }

    if (ss_fd > 0) close(ss_fd);
    if (sl_fd > 0) close(sl_fd);
    log_info("Sensor encerrado.");
    return 0;
}
