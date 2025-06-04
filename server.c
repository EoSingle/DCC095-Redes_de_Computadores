// server.c
#include "common.h"
#include <sys/select.h> // Para select() e fd_set
#include <errno.h>      // Para errno

#define MAX_CLIENTS 15 // Conforme especificado, cada servidor trata até 15 equipamentos

int peer_socket_fd = -1;

// Estado da conexão P2P
typedef enum {
    P2P_DISCONNECTED,
    P2P_ACTIVE_CONNECTING,      // S_i tentou connect(), aguardando resultado ou enviando REQ
    P2P_PASSIVE_LISTENING,      // S_i falhou em conectar, agora escuta
    P2P_REQ_SENT,               // S_i (ativo) enviou REQ_CONNPEER, aguarda RES
    P2P_RES_SENT_AWAITING_RES,  // S_j (passivo) recebeu REQ, enviou RES, aguarda RES de S_i
    P2P_RES_RECEIVED_AWAITING_FINAL_RES, // S_i (ativo) recebeu RES, enviou RES, aguarda RES final (não no fluxo)
                                        // O fluxo do PDF é S_i envia REQ, S_j envia RES, S_i envia RES, S_j recebe RES.
                                        // Então, S_i recebe RES, envia RES. S_j recebe RES.
    P2P_FULLY_ESTABLISHED,
    P2P_DISCONNECT_REQ_SENT // S_i enviou REQ_DISCPEER, aguardando confirmação de desconexão
} P2PState;

P2PState p2p_current_state = P2P_DISCONNECTED;
char my_pids_for_peer[MAX_PIDS_LENGTH] = ""; // ID que este servidor atribui ao peer
char peer_pids_for_me[MAX_PIDS_LENGTH] = ""; // ID que o peer atribuiu a este servidor

typedef struct {
    int socket_fd;
    char id_cliente[MAX_PIDS_LENGTH]; // IdC gerado pelo servidor
    int loc_id; // Se este for o SL
    int status_risco; // Se este for o SS (0 ou 1)
    // Adicionar mais campos conforme necessário
} ClientInfo;

ClientInfo connected_clients[MAX_CLIENTS]; // Array de clientes conectados
int num_connected_clients = 0; // Contador

// Função para gerar IdC único (exemplo simples)
void generate_unique_client_id(char *buffer, size_t buffer_len, int client_socket_fd) {
    snprintf(buffer, buffer_len, "Sensor%d", client_socket_fd); // Simples, mas não globalmente único entre reinícios
                                                               // O PDF diz que o servidor define[cite: 110].
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Uso: %s <ip_peer_destino> <porta_p2p_peer_destino> <porta_escuta_cliente_propria>\n", argv[0]);
        fprintf(stderr, "Exemplo: ./server 127.0.0.1 64000 60000\n");
        exit(EXIT_FAILURE);
    }

    char *peer_target_ip = argv[1];
    int peer_target_port = atoi(argv[2]); // Porta P2P onde o outro peer escuta (ou este tentará conectar)
                                          // Esta também será a porta onde este servidor escutará por P2P se a conexão ativa falhar.
    int client_listen_port = atoi(argv[3]); // Porta onde este servidor escuta por clientes

    int client_master_socket_fd, new_socket_fd; // Renomeado master_socket_fd para clareza
    int peer_listen_socket_fd = -1; // Socket para escutar P2P se a conexão ativa falhar
    
    struct sockaddr_in server_addr_clients, server_addr_peer_listen, peer_target_addr;
    socklen_t client_addr_len = sizeof(struct sockaddr_in); // client_addr não está mais no main, mas sim dentro dos loops
    char buffer[MAX_MSG_SIZE + 1];
    char log_msg[150]; // Aumentado para logs mais longos

    int client_sockets[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_sockets[i] = 0;
    }

    fd_set read_fds;
    int max_sd;

    // --- INICIALIZAÇÃO DO SOCKET DE ESCUTA DE CLIENTES ---
    if ((client_master_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) { /* ... erro ... */ exit(EXIT_FAILURE); }
    log_info("Socket mestre de clientes criado.");
    int opt = 1;
    if (setsockopt(client_master_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) { /* ... erro ... */ }
    server_addr_clients.sin_family = AF_INET;
    server_addr_clients.sin_addr.s_addr = INADDR_ANY;
    server_addr_clients.sin_port = htons(client_listen_port);
    if (bind(client_master_socket_fd, (struct sockaddr *)&server_addr_clients, sizeof(server_addr_clients)) < 0) { /* ... erro ... */ close(client_master_socket_fd); exit(EXIT_FAILURE); }
    sprintf(log_msg, "Socket mestre de clientes fez bind na porta %d.", client_listen_port); log_info(log_msg);
    if (listen(client_master_socket_fd, SERVER_BACKLOG) < 0) { /* ... erro ... */ close(client_master_socket_fd); exit(EXIT_FAILURE); }
    sprintf(log_msg, "Servidor ouvindo por clientes na porta %d...", client_listen_port); log_info(log_msg);

    // --- TENTATIVA DE CONEXÃO P2P ATIVA ---
    log_info("Tentando conectar ao peer...");
    // Criar socket para tentar conectar ao peer
    if ((peer_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        log_error("Falha ao criar socket para conexão P2P ativa.");
        // Poderia tentar escutar P2P aqui ou sair, dependendo da robustez desejada
    } else {
        peer_target_addr.sin_family = AF_INET;
        peer_target_addr.sin_port = htons(peer_target_port);
        if (inet_pton(AF_INET, peer_target_ip, &peer_target_addr.sin_addr) <= 0) {
            log_error("Endereço IP do peer inválido para conexão P2P ativa.");
            close(peer_socket_fd);
            peer_socket_fd = -1;
        } else {
            if (connect(peer_socket_fd, (struct sockaddr *)&peer_target_addr, sizeof(peer_target_addr)) < 0) {
                sprintf(log_msg, "Falha ao conectar ao peer %s:%d. %s.", peer_target_ip, peer_target_port, strerror(errno));
                log_info(log_msg);
                close(peer_socket_fd);
                peer_socket_fd = -1; // Marcar que a conexão ativa falhou

                // Se falhou, prepara para escutar P2P passivamente
                log_info("No peer found, starting to listen for P2P connections..."); // [cite: 54]
                if ((peer_listen_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) { /* ... erro ... */ }
                else {
                     if (setsockopt(peer_listen_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) { /* ... erro ... */ }
                    server_addr_peer_listen.sin_family = AF_INET;
                    server_addr_peer_listen.sin_addr.s_addr = INADDR_ANY;
                    server_addr_peer_listen.sin_port = htons(peer_target_port); // Escuta na mesma porta P2P
                    if (bind(peer_listen_socket_fd, (struct sockaddr *)&server_addr_peer_listen, sizeof(server_addr_peer_listen)) < 0) { /* ... erro ... */ close(peer_listen_socket_fd); peer_listen_socket_fd = -1;}
                    else if (listen(peer_listen_socket_fd, 1) < 0) { /* ... erro ... */ close(peer_listen_socket_fd); peer_listen_socket_fd = -1; } // Só escuta por 1 peer
                    else {
                        sprintf(log_msg, "Servidor ouvindo por conexão P2P na porta %d...", peer_target_port);
                        log_info(log_msg);
                    }
                }

            } else {
                sprintf(log_msg, "Conectado ao peer %s:%d no socket P2P %d. Enviando REQ_CONNPEER...",
                        peer_target_ip, peer_target_port, peer_socket_fd);
                log_info(log_msg);
                p2p_current_state = P2P_ACTIVE_CONNECTING; // Ou um estado que indique que está prestes a enviar REQ

                char msg_buffer[MAX_MSG_SIZE];
                build_control_message(msg_buffer, sizeof(msg_buffer), REQ_CONNPEER, NULL);
                if (write(peer_socket_fd, msg_buffer, strlen(msg_buffer)) < 0) {
                    log_error("Falha ao enviar REQ_CONNPEER");
                    close(peer_socket_fd);
                    peer_socket_fd = -1;
                    p2p_current_state = P2P_DISCONNECTED; // Voltar ao estado desconectado
                    // Tentar escutar passivamente P2P (lógica já existente)
                } else {
                    log_info("REQ_CONNPEER enviado.");
                    p2p_current_state = P2P_REQ_SENT; // Agora aguardando RES_CONNPEER
                }
            }
        }
    }

    log_info("Aguardando conexões (clientes/P2P) ou entrada do teclado...");

    int running = 1; // Variável para controlar o loop principal

    while (running) { // Loop principal do servidor
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        max_sd = STDIN_FILENO;

        FD_SET(client_master_socket_fd, &read_fds);
        if (client_master_socket_fd > max_sd) max_sd = client_master_socket_fd;

        if (peer_listen_socket_fd > 0) { // Se estiver escutando por P2P
            FD_SET(peer_listen_socket_fd, &read_fds);
            if (peer_listen_socket_fd > max_sd) max_sd = peer_listen_socket_fd;
        }
        if (peer_socket_fd > 0) { // Se tiver uma conexão P2P ativa/estabelecida
            FD_SET(peer_socket_fd, &read_fds);
            if (peer_socket_fd > max_sd) max_sd = peer_socket_fd;
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_sockets[i] > 0) {
                FD_SET(client_sockets[i], &read_fds);
                if (client_sockets[i] > max_sd) {
                    max_sd = client_sockets[i];
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int current_client_socket = client_sockets[i]; // Ou o nome da sua variável para o socket do cliente atual

            if (current_client_socket > 0 && FD_ISSET(current_client_socket, &read_fds)) {
                // Há dados chegando de um cliente
                ssize_t bytes_read = read(current_client_socket, buffer, MAX_MSG_SIZE); // 'buffer' deve estar definido no escopo

                if (bytes_read > 0) {
                    buffer[bytes_read] = '\0'; // Garantir terminação nula
                    
                    // Obter informações do cliente para log (opcional, mas útil)
                    struct sockaddr_in addr_cli_log;
                    socklen_t len_cli_log = sizeof(addr_cli_log);
                    getpeername(current_client_socket, (struct sockaddr*)&addr_cli_log, &len_cli_log);
                    sprintf(log_msg, "Dados recebidos do cliente %s:%d (socket %d)",
                            inet_ntoa(addr_cli_log.sin_addr), ntohs(addr_cli_log.sin_port), current_client_socket);
                    log_info(log_msg);

                    // AGORA, PROCESSAR A MENSAGEM RECEBIDA DO CLIENTE
                    int code;
                    char payload[MAX_MSG_SIZE]; // Deve ser suficiente para os payloads esperados
                    
                    if (parse_message(buffer, &code, payload, sizeof(payload))) {
                        sprintf(log_msg, "Msg do cliente (socket %d): Code=%d, Payload='%s'", current_client_socket, code, payload);
                        log_info(log_msg);
                        
                        // Lógica para tratar REQ_CONNSEN e outras mensagens do cliente
                        if (code == REQ_CONNSEN) {
                            int client_loc_id = atoi(payload);

                            // Verificar se o socket realmente corresponde, como uma sanidade.
                            if (connected_clients[i].socket_fd != current_client_socket) {
                                // Esta situação indica um erro grave na lógica de gerenciamento de arrays.
                                sprintf(log_msg, "Erro crítico: Inconsistência entre client_sockets[%d] (%d) e connected_clients[%d].socket_fd (%d).",
                                        i, current_client_socket, i, connected_clients[i].socket_fd);
                                log_error(log_msg);
                                close(current_client_socket);
                                client_sockets[i] = 0;
                                if (connected_clients[i].socket_fd != 0) { // Limpar completamente
                                     if (connected_clients[i].id_cliente[0] != '\0' && num_connected_clients > 0) num_connected_clients--;
                                     connected_clients[i].socket_fd = 0;
                                     connected_clients[i].id_cliente[0] = '\0';
                                }
                                continue; // Pular para o próximo cliente
                            }

                            // Verificar se o cliente já está registrado (já tem um IdC)
                            if (connected_clients[i].id_cliente[0] == '\0') { // Cliente ainda não registrado
                                if (num_connected_clients >= MAX_CLIENTS) {
                                    log_info("Limite de sensores (num_connected_clients) excedido. Enviando ERROR(09)...");
                                    char error_payload_str[10];
                                    sprintf(error_payload_str, "%02d", SENSOR_LIMIT_EXCEEDED_ERROR);
                                    char msg_error[MAX_MSG_SIZE];
                                    build_control_message(msg_error, sizeof(msg_error), ERROR_MSG, error_payload_str);
                                    if (write(current_client_socket, msg_error, strlen(msg_error)) < 0) { /* log erro no write */ }
                                } else {
                                    char new_client_id_str[MAX_PIDS_LENGTH];
                                    generate_unique_client_id(new_client_id_str, sizeof(new_client_id_str), current_client_socket);

                                    strncpy(connected_clients[i].id_cliente, new_client_id_str, MAX_PIDS_LENGTH - 1);
                                    connected_clients[i].id_cliente[MAX_PIDS_LENGTH - 1] = '\0';
                                    
                                    connected_clients[i].loc_id = client_loc_id; 

                                    num_connected_clients++;

                                    sprintf(log_msg, "Client %s added (Loc %d)", connected_clients[i].id_cliente, client_loc_id);
                                    log_info(log_msg);

                                    char msg_res_connsen[MAX_MSG_SIZE];
                                    build_control_message(msg_res_connsen, sizeof(msg_res_connsen), RES_CONNSEN, connected_clients[i].id_cliente);
                                    if (write(current_client_socket, msg_res_connsen, strlen(msg_res_connsen)) < 0) { /* log erro no write */ }
                                    else { log_info("RES_CONNSEN enviado ao cliente."); }
                                }
                            } else {
                                // Cliente já registrado (tem um IdC), mas enviou REQ_CONNSEN novamente.
                                sprintf(log_msg, "Cliente %s (socket %d) já registrado enviou REQ_CONNSEN novamente. Ignorando.",
                                        connected_clients[i].id_cliente, current_client_socket);
                                log_info(log_msg);
                                // Opcionalmente, poderia reenviar o RES_CONNSEN existente.
                            }
                        }
                        // else if (code == REQ_DISCSEN) { ... }
                        // else if (code == REQ_SENSSTATUS && (sou_servidor_status)) { ... }
                        // else if (code == REQ_SENSLOC && (sou_servidor_localizacao)) { ... }
                        // ... outros tratadores de mensagens do cliente ...
                        else {
                            sprintf(log_msg, "Código de mensagem de cliente desconhecido ou não esperado: %d", code);
                            log_info(log_msg);
                            // Enviar mensagem de erro? O protocolo não especifica um erro genérico para isso.
                        }
                        // ##################################################################
                        // ## FIM DO TRECHO DE CÓDIGO COMENTADO QUE VOCÊ PRECISA INSERIR ##
                        // ##################################################################

                    } else {
                        log_error("Falha ao parsear mensagem do cliente.");
                        // Lidar com erro de parsing, talvez fechar a conexão
                        // close(current_client_socket); client_sockets[i] = 0; connected_clients[i].socket_fd = 0; num_connected_clients--;
                    }
                } else if (bytes_read == 0) {
                    // Cliente desconectou (EOF)
                    // ... (lógica para remover o cliente de connected_clients, decrementar num_connected_clients) ...
                    sprintf(log_msg, "Cliente (socket %d) desconectou.", current_client_socket);
                    log_info(log_msg);
                    close(current_client_socket);
                    client_sockets[i] = 0; // Marcar slot como livre em client_sockets
                    if (connected_clients[i].socket_fd != 0) { // Se estava na lista de clientes ativos
                        connected_clients[i].socket_fd = 0; // Marcar slot como livre em connected_clients
                        connected_clients[i].id_cliente[0] = '\0';
                        if (num_connected_clients > 0) num_connected_clients--;
                    }
                } else { // Erro na leitura (bytes_read < 0)
                    log_error("Erro ao ler do cliente.");
                    // ... (lógica para remover o cliente) ...
                    close(current_client_socket);
                    client_sockets[i] = 0;
                     if (connected_clients[i].socket_fd != 0) {
                        connected_clients[i].socket_fd = 0;
                        connected_clients[i].id_cliente[0] = '\0';
                        if (num_connected_clients > 0) num_connected_clients--;
                    }
                }
            } // Fim do if (FD_ISSET(current_client_socket, &read_fds))
        } // Fim do for (int i = 0; i < MAX_CLIENTS; i++)


        int activity = select(max_sd + 1, &read_fds, NULL, NULL, NULL);
        if ((activity < 0) && (errno != EINTR)) { /* ... erro ... */ }


        // Verificar se há entrada do teclado (STDIN_FILENO)
        if (FD_ISSET(STDIN_FILENO, &read_fds))
        {
            // Ler o comando do teclado
            // Usar fgets para ler a linha inteira de forma mais segura
            char command_buffer[MAX_MSG_SIZE];
            if (fgets(command_buffer, sizeof(command_buffer), stdin) != NULL)
            {
                // Remover o caractere de nova linha, se presente
                command_buffer[strcspn(command_buffer, "\n")] = 0;

                sprintf(log_msg, "Comando do teclado recebido: '%s'", command_buffer);
                log_info(log_msg);

                if (strcmp(command_buffer, "kill") == 0)
                {
                    if (p2p_current_state == P2P_FULLY_ESTABLISHED && peer_socket_fd > 0) {
                        sprintf(log_msg, "Comando 'kill' recebido. Enviando REQ_DISCPEER para o peer %s...", my_pids_for_peer);
                        log_info(log_msg);

                        char msg_req_disc[MAX_MSG_SIZE];
                        // O payload de REQ_DISCPEER é PidS, o ID que ESTE servidor (S_i) deu ao OUTRO servidor (S_j)
                        // que está armazenado em my_pids_for_peer.
                        build_control_message(msg_req_disc, sizeof(msg_req_disc), REQ_DISCPEER, my_pids_for_peer);
                        
                        if (write(peer_socket_fd, msg_req_disc, strlen(msg_req_disc)) < 0) {
                            log_error("Falha ao enviar REQ_DISCPEER");
                            // Poderia tentar fechar o socket localmente de qualquer forma
                            close(peer_socket_fd);
                            peer_socket_fd = -1;
                            p2p_current_state = P2P_DISCONNECTED;
                        } else {
                            log_info("REQ_DISCPEER enviado.");
                            p2p_current_state = P2P_DISCONNECT_REQ_SENT;
                        }
                    } else {
                        log_info("No peer connected or P2P not fully established to issue REQ_DISCPEER.");
                        // Conforme especificação: "Caso não haja conexão, o programa deverá imprimir No peer connected to close connection" [cite: 115]
                        // No nosso caso, é mais preciso: "No peer connected or P2P not fully established..."
                     }
                }
                else if (strcmp(command_buffer, "exit") == 0)
                {
                    log_info("Comando 'exit' recebido. Encerrando o servidor...");
                    // Adicionar lógica para fechar todos os sockets e sair gracefulmente.
                    // Por enquanto, vamos apenas quebrar o loop para terminar.
                    // Este comando 'exit' é para nossa conveniência de teste, não está no PDF.
                    // O fechamento P2P correto, segundo o PDF, também encerra o servidor S_i após confirmação. [cite: 65]
                    // E S_j volta a escutar. [cite: 66]
                    // TODO: Implementar fechamento gracioso de todos os sockets.
                    break; // Sai do while(1)
                }
                else
                {
                    sprintf(log_msg, "Comando desconhecido: '%s'", command_buffer);
                    log_info(log_msg);
                }
            }
            else
            {
                // Se fgets retornar NULL, pode ser EOF (Ctrl+D) ou um erro.
                log_info("Entrada do teclado finalizada (EOF) ou erro de leitura.");
                // Poderia tratar como um comando de saída também.
                break; // Sai do while(1)
            }
        }

        // Aceitar nova conexão de cliente
        if (FD_ISSET(client_master_socket_fd, &read_fds)) {
            struct sockaddr_in new_client_addr; // Definir struct para o novo cliente
            socklen_t new_client_addr_len = sizeof(new_client_addr);
            if ((new_socket_fd = accept(client_master_socket_fd, (struct sockaddr *)&new_client_addr, &new_client_addr_len)) < 0) {
                log_error("Falha ao aceitar nova conexão de cliente");
            } else {
                char client_ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &new_client_addr.sin_addr, client_ip_str, INET_ADDRSTRLEN);
                
                int added_to_slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (client_sockets[i] == 0) { // Encontrou um slot livre no array de sockets
                        client_sockets[i] = new_socket_fd;
                        
                        // **CRUCIAL: Inicialize a estrutura connected_clients correspondente**
                        connected_clients[i].socket_fd = new_socket_fd;
                        connected_clients[i].id_cliente[0] = '\0'; // Indica que ainda não tem IdC
                        connected_clients[i].loc_id = -1;         // Inicializar outros campos
                        connected_clients[i].status_risco = 0;   // Exemplo de inicialização

                        sprintf(log_msg, "Novo cliente conectado %s:%d no socket %d, alocado no slot %d.",
                                client_ip_str, ntohs(new_client_addr.sin_port), new_socket_fd, i);
                        log_info(log_msg);
                        added_to_slot = i;
                        break;
                    }
                }
                if (added_to_slot == -1) {
                    log_info("Limite de clientes (slots de socket) atingido. Rejeitando nova conexão.");
                    // A mensagem ERROR(09) é enviada ao receber REQ_CONNSEN se num_connected_clients for alto.
                    // Aqui, estamos apenas rejeitando a conexão TCP se não houver slot no array client_sockets.
                    close(new_socket_fd);
                }
            }
        }
        
        // Aceitar nova conexão P2P (se estiver escutando e ainda não conectado)
        if (peer_listen_socket_fd > 0 && FD_ISSET(peer_listen_socket_fd, &read_fds)) {
            struct sockaddr_in peer_addr;
            socklen_t peer_addr_len = sizeof(peer_addr);
            int new_peer_fd = accept(peer_listen_socket_fd, (struct sockaddr *)&peer_addr, &peer_addr_len);
            if (new_peer_fd < 0) {
                log_error("Falha ao aceitar nova conexão P2P");
            } else {
                peer_socket_fd = new_peer_fd;
                close(peer_listen_socket_fd); // Só aceita um peer, fecha o socket de escuta
                peer_listen_socket_fd = -1;
                p2p_current_state = P2P_PASSIVE_LISTENING;
                sprintf(log_msg, "Nova conexão P2P aceita no socket %d. Estado: PASSIVE_LISTENING.", peer_socket_fd);
                log_info(log_msg);
            }
        }

        // Ler dados do peer conectado
        if (peer_socket_fd > 0 && FD_ISSET(peer_socket_fd, &read_fds)) {
            ssize_t bytes_read = read(peer_socket_fd, buffer, MAX_MSG_SIZE);
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';

                sprintf(log_msg, "S_i: Dados brutos recebidos do peer: [%s]", buffer);
                log_info(log_msg);
                
                int code;
                char payload[MAX_MSG_SIZE]; // Aumentar se MAX_PIDS_LENGTH for maior
                if (parse_message(buffer, &code, payload, sizeof(payload))) {
                    sprintf(log_msg, "Mensagem P2P recebida: Code=%d, Payload='%s'", code, payload);
                    log_info(log_msg);

                    char msg_to_send[MAX_MSG_SIZE];
                    char response_payload_str[10];

                    if (p2p_current_state == P2P_PASSIVE_LISTENING && code == REQ_CONNPEER) { // S_j recebe REQ_CONNPEER
                        // Este é S_j (servidor passivo) recebendo REQ_CONNPEER de S_i
                        // Gerar PidS para S_i, registrar, imprimir, enviar RES_CONNPEER(PidS_para_Si)
                        snprintf(my_pids_for_peer, sizeof(my_pids_for_peer), "Peer%d_Active", peer_socket_fd); // Exemplo de PidS
                        
                        sprintf(log_msg, "Peer %s, connected", my_pids_for_peer); // [cite: 58]
                        log_info(log_msg);
                        
                        build_control_message(msg_to_send, sizeof(msg_to_send), RES_CONNPEER, my_pids_for_peer);
                        if (write(peer_socket_fd, msg_to_send, strlen(msg_to_send)) < 0) { /* ... erro ... */ }
                        else {
                            log_info("RES_CONNPEER (para S_i) enviado.");
                            p2p_current_state = P2P_RES_SENT_AWAITING_RES; // S_j agora aguarda RES_CONNPEER de S_i
                        }

                    } else if (p2p_current_state == P2P_REQ_SENT && code == RES_CONNPEER) { // S_i recebe RES_CONNPEER
                        // Este é S_i (servidor ativo) recebendo RES_CONNPEER de S_j
                        strncpy(peer_pids_for_me, payload, sizeof(peer_pids_for_me) - 1);
                        peer_pids_for_me[sizeof(peer_pids_for_me) - 1] = '\0'; // Garantir terminação nula
                        
                        sprintf(log_msg, "New Peer ID: %s", peer_pids_for_me); // Onde peer_pids_for_me conteria "Peer4_Passive"
                        log_info(log_msg);
                        p2p_current_state = P2P_FULLY_ESTABLISHED; // Atualize o estado
                        sprintf(log_msg, "Conexão P2P com %s (ID: %s) totalmente estabelecida.", my_pids_for_peer, peer_pids_for_me);
                        log_info(log_msg);

                        // Definir um identificador PidS para S_j, imprimir, enviar RES_CONNPEER(PidS_para_Sj)
                        snprintf(my_pids_for_peer, sizeof(my_pids_for_peer), "Peer%d_Passive", peer_socket_fd); // Exemplo
                        sprintf(log_msg, "Peer %s, connected", my_pids_for_peer); // [cite: 59]
                        log_info(log_msg);

                        build_control_message(msg_to_send, sizeof(msg_to_send), RES_CONNPEER, my_pids_for_peer);
                        if (write(peer_socket_fd, msg_to_send, strlen(msg_to_send)) < 0) { /* ... erro ... */ }
                        else {
                            log_info("RES_CONNPEER (para S_j) enviado.");
                            p2p_current_state = P2P_FULLY_ESTABLISHED; // S_i considera estabelecido aqui
                            sprintf(log_msg, "Conexão P2P com %s (ID: %s) totalmente estabelecida.", my_pids_for_peer, peer_pids_for_me);
                            log_info(log_msg);
                        }
                    } else if (p2p_current_state == P2P_RES_SENT_AWAITING_RES && code == RES_CONNPEER) { // S_j recebe RES_CONNPEER final
                        // Este é S_j (servidor passivo) recebendo o segundo RES_CONNPEER de S_i
                        strncpy(peer_pids_for_me, payload, sizeof(peer_pids_for_me) - 1);
                        peer_pids_for_me[sizeof(peer_pids_for_me) - 1] = '\0';

                        sprintf(log_msg, "New Peer ID: %s", peer_pids_for_me); // [cite: 59]
                        log_info(log_msg);
                        p2p_current_state = P2P_FULLY_ESTABLISHED;
                        sprintf(log_msg, "Conexão P2P com %s (ID: %s) totalmente estabelecida.", my_pids_for_peer, peer_pids_for_me);
                        log_info(log_msg);
                        // Conexão P2P totalmente estabelecida para S_j
                    } else if (code == REQ_DISCPEER) {
                        // O payload recebido é o PidS que S_i atribuiu a S_j (este servidor)
                        // Devemos verificar se o payload (PidS_i_para_Sj) corresponde ao que S_j (este servidor)
                        // conhece como o ID que S_i lhe deu (armazenado em 'peer_pids_for_me' neste servidor S_j).
                        // O PDF diz "verifica se PidS, é o identificador do peer conectado." [cite: 61]
                        // No REQ_DISCPEER(PidS), PidS é o ID que S_i deu para S_j.
                        // Então, S_j (este servidor) verifica se o 'payload' recebido é o 'peer_pids_for_me'.
                        if (strcmp(payload, peer_pids_for_me) == 0) {
                            // Identificador do peer S_i (que S_j deu para S_i) está em 'my_pids_for_peer' de S_j
                            sprintf(log_msg, "REQ_DISCPEER recebido do peer %s (ID: %s). Confirmado.", my_pids_for_peer, peer_pids_for_me);
                            log_info(log_msg);

                            // Remover S_i da base de dados (limpar PIDs), responder OK(01)
                            sprintf(response_payload_str, "%02d", OK_SUCCESSFUL_DISCONNECT); // Formato de 2 dígitos
                            build_control_message(msg_to_send, sizeof(msg_to_send), OK_MSG, response_payload_str);
                            if (write(peer_socket_fd, msg_to_send, strlen(msg_to_send)) < 0) { /* erro */ }
                            else {
                                log_info("OK(01) de desconexão enviado para o peer.");
                            }
                            
                            // Imprimir "Peer PidS disconnected" (PidS é o ID que S_j deu para S_i)
                            sprintf(log_msg, "Peer %s disconnected.", my_pids_for_peer);
                            log_info(log_msg);
                            
                            close(peer_socket_fd);
                            peer_socket_fd = -1;
                            p2p_current_state = P2P_DISCONNECTED;
                            my_pids_for_peer[0] = '\0';
                            peer_pids_for_me[0] = '\0';

                            // S_j começa a ouvir por novas conexões P2P [cite: 66]
                            log_info("No peer found, starting to listen for P2P connections...");
                            // (A lógica para iniciar peer_listen_socket_fd já existe na inicialização se peer_socket_fd = -1,
                            // mas pode precisar ser reativada aqui se o servidor não for encerrar)
                            // Para simplificar, podemos apenas voltar a permitir que o select monitore o peer_listen_socket_fd
                            // que foi preparado na inicialização (se a conexão P2P ativa inicial falhou).
                            // Se a conexão P2P inicial foi ativa por S_j, ele não teria peer_listen_socket_fd.
                            // O fluxo é mais simples se S_j sempre tentar escutar após uma desconexão.
                            // Vamos re-inicializar a escuta P2P aqui:
                            if (peer_listen_socket_fd <= 0) { // Se não já estiver escutando ou preparando
                                if ((peer_listen_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) != -1) {
                                    int opt = 1;
                                    setsockopt(peer_listen_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                                    server_addr_peer_listen.sin_family = AF_INET; // Supondo que server_addr_peer_listen foi configurado antes
                                    server_addr_peer_listen.sin_addr.s_addr = INADDR_ANY;
                                    server_addr_peer_listen.sin_port = htons(peer_target_port); // Usar a porta P2P original
                                    if (bind(peer_listen_socket_fd, (struct sockaddr *)&server_addr_peer_listen, sizeof(server_addr_peer_listen)) == 0 &&
                                        listen(peer_listen_socket_fd, 1) == 0) {
                                        sprintf(log_msg, "Servidor (anteriormente S_j) ouvindo por nova conexão P2P na porta %d...", peer_target_port);
                                        log_info(log_msg);
                                    } else {
                                        log_error("Falha ao reiniciar escuta P2P após desconexão.");
                                        close(peer_listen_socket_fd);
                                        peer_listen_socket_fd = -1;
                                    }
                                }
                            }


                        } else { // PidS não confere
                            sprintf(log_msg, "REQ_DISCPEER recebido com PidS ('%s') incorreto. Esperado: '%s'. Enviando ERROR(02)...", payload, peer_pids_for_me);
                            log_info(log_msg);
                            sprintf(response_payload_str, "%02d", PEER_NOT_FOUND_ERROR);
                            build_control_message(msg_to_send, sizeof(msg_to_send), ERROR_MSG, response_payload_str);
                            if (write(peer_socket_fd, msg_to_send, strlen(msg_to_send)) < 0) { /* erro */ }
                        }
                    }
                    // S_i recebe OK(01) de S_j
                    else if (code == OK_MSG && atoi(payload) == OK_SUCCESSFUL_DISCONNECT) {
                        log_info("OK(01) 'Successful disconnect' recebido do peer.");
                        // Imprimir descrição da mensagem (já feito pelo log acima)
                        // Remover S_j da base de dados (limpar PIDs)
                        // Imprimir "Peer PidS disconnected" (PidS é o ID que S_i deu para S_j)
                        sprintf(log_msg, "Peer %s disconnected.", my_pids_for_peer);
                        log_info(log_msg);
                        
                        close(peer_socket_fd);
                        peer_socket_fd = -1;
                        p2p_current_state = P2P_DISCONNECTED;
                        my_pids_for_peer[0] = '\0';
                        peer_pids_for_me[0] = '\0';

                        log_info("Encerrando execução do servidor S_i conforme protocolo.");
                        close(client_master_socket_fd); // Fechar outros sockets importantes
                        running = 0; 
                    }
                    // S_i recebe ERROR(02) de S_j
                    else if (code == ERROR_MSG && atoi(payload) == PEER_NOT_FOUND_ERROR) {
                        log_info("ERROR(02) 'Peer not found' recebido do peer.");
                        // Imprimir descrição e não fazer mais nada com o peer, mas não encerra S_i necessariamente.
                        // O PDF diz que S_i imprime a descrição[cite: 63]. Não diz que S_i encerra ou tenta novamente.
                        // Manter a conexão P2P ou fechar? O erro implica que S_j não reconheceu S_i.
                        // Por segurança, S_i pode fechar sua ponta.
                        close(peer_socket_fd);
                        peer_socket_fd = -1;
                        p2p_current_state = P2P_DISCONNECTED;
                    } else if (code == REQ_CHECKALERT) { // Payload é o SensID (IdC do sensor no SS)
                        char sens_id_from_ss[MAX_PIDS_LENGTH];
                        strncpy(sens_id_from_ss, payload, sizeof(sens_id_from_ss) - 1);
                        sens_id_from_ss[sizeof(sens_id_from_ss) - 1] = '\0';

                        sprintf(log_msg, "SL: REQ_CHECKALERT %s recebido do SS", sens_id_from_ss);
                        log_info(log_msg);

                        // !!! INÍCIO DO HACK PARA TESTE DE FLUXO !!!
                        // O correto seria buscar 'sens_id_from_ss' na base de dados do SL,
                        // o que requer que os IDs sejam consistentes ou mapeados entre SS e SL.
                        // Por agora, vamos pegar a localização do primeiro cliente registrado no SL, ou um valor fixo.
                        int loc_id_to_send = -1;
                        char found_client_for_hack[MAX_PIDS_LENGTH] = "N/A_HACK";

                        for (int k = 0; k < MAX_CLIENTS; k++) {
                            if (connected_clients[k].socket_fd > 0 && connected_clients[k].id_cliente[0] != '\0') {
                                loc_id_to_send = connected_clients[k].loc_id;
                                strncpy(found_client_for_hack, connected_clients[k].id_cliente, MAX_PIDS_LENGTH -1);
                                break; // Usar o primeiro encontrado para o hack
                            }
                        }
                        if (loc_id_to_send == -1) { // Se nenhum cliente no SL, usar um valor fixo para teste
                            loc_id_to_send = 3; // Exemplo fixo
                            log_info("SL: HACK - Nenhum cliente no SL, usando LocID de teste fixo.");
                        } else {
                            sprintf(log_msg, "SL: HACK - Usando LocID do cliente %s do SL para responder ao REQ_CHECKALERT sobre %s.", found_client_for_hack, sens_id_from_ss);
                            log_info(log_msg);
                        }
                        // !!! FIM DO HACK PARA TESTE DE FLUXO !!!

                        char msg_to_ss[MAX_MSG_SIZE];
                        char payload_str[10];
                        // Com o hack, loc_id_to_send sempre terá um valor (ou do primeiro cliente do SL ou o fixo)
                        // Para simular "Sensor not found" no SL, você teria que setar loc_id_to_send = -1 propositalmente.
                        // Para este teste, vamos assumir que sempre encontramos uma localização (devido ao hack).

                        if (loc_id_to_send != -1) { // Sempre verdadeiro com o hack atual, a menos que queira testar o erro do SL
                            sprintf(payload_str, "%d", loc_id_to_send);
                            sprintf(log_msg, "SL: Sending RES_CHECKALERT %s to SS", payload_str);
                            log_info(log_msg);
                            build_control_message(msg_to_ss, sizeof(msg_to_ss), RES_CHECKALERT, payload_str);
                        } else {
                            // Este bloco seria para o caso real de não encontrar o sensor.
                            // sprintf(log_msg, "SL: ERROR(10) - Sensor %s not found (real logic)", sens_id_from_ss);
                            // log_info(log_msg);
                            // sprintf(payload_str, "%02d", SENSOR_NOT_FOUND_ERROR);
                            // build_control_message(msg_to_ss, sizeof(msg_to_ss), ERROR_MSG, payload_str);
                        }
                        if (write(peer_socket_fd, msg_to_ss, strlen(msg_to_ss)) < 0) { /* log erro */ 
                        }
                    } else {
                        sprintf(log_msg, "Mensagem P2P inesperada (Code=%d) ou estado P2P incorreto (%d).", code, p2p_current_state);
                        log_info(log_msg);
                    }
                } else {
                    log_error("Falha ao parsear mensagem P2P.");
                    // Poderia fechar a conexão P2P aqui por erro de protocolo
                }
            } else if (bytes_read == 0) { // Peer desconectou
                log_info("Peer desconectado.");
                close(peer_socket_fd);
                peer_socket_fd = -1;
                p2p_current_state = P2P_DISCONNECTED;
                // Limpar PidS
                my_pids_for_peer[0] = '\0';
                peer_pids_for_me[0] = '\0';
                // Tentar escutar P2P novamente? O PDF diz que S_j volta a escutar[cite: 66].
                // S_i encerra sua execução após confirmação OK(01) do REQ_DISCPEER[cite: 65].
                // Por ora, apenas desconectamos. A lógica de escuta P2P já está fora deste if.
            } else { // Erro na leitura
                log_error("Erro ao ler do peer.");
                close(peer_socket_fd);
                peer_socket_fd = -1;
                p2p_current_state = P2P_DISCONNECTED;
            }
        }

        // Ler dados dos clientes conectados
        for (int i = 0; i < MAX_CLIENTS; i++) { 
            int current_client_socket = client_sockets[i];
            if (current_client_socket > 0 && FD_ISSET(current_client_socket, &read_fds)) {
                ssize_t bytes_read = read(current_client_socket, buffer, MAX_MSG_SIZE);
                if (bytes_read > 0) {
                    buffer[bytes_read] = '\0'; // Garantir terminação nula
                    int code;
                    char payload[MAX_MSG_SIZE]; 
                    sprintf(log_msg, "Mensagem recebida do cliente %d: %s", current_client_socket, buffer);
                    log_info(log_msg);
                    
                    if(parse_message(buffer, &code, payload, sizeof(payload))) {
                        sprintf(log_msg, "Código: %d, Payload: '%s'", code, payload);
                        log_info(log_msg);
                        if (code == REQ_CONNSEN) {
                            // ... (sua lógica existente para REQ_CONNSEN) ...
                        } else if (code == REQ_DISCSEN) { // Cliente solicitando desconexão 
                            char client_id_from_payload[MAX_PIDS_LENGTH];
                            strncpy(client_id_from_payload, payload, sizeof(client_id_from_payload) - 1);
                            client_id_from_payload[sizeof(client_id_from_payload) - 1] = '\0';

                            char msg_response_to_client[MAX_MSG_SIZE];
                            char response_code_payload_str[10]; // Para "01" ou "10"

                            // Verificar se o IdC do payload corresponde ao IdC armazenado para este socket (connected_clients[i])
                            // Assumindo que 'i' é o índice correto para connected_clients
                            if (connected_clients[i].socket_fd == current_client_socket && 
                                strcmp(connected_clients[i].id_cliente, client_id_from_payload) == 0 &&
                                connected_clients[i].id_cliente[0] != '\0') { // Cliente encontrado e ID confere

                                sprintf(log_msg, "Client %s removed (Loc %d)", 
                                        connected_clients[i].id_cliente, 
                                        connected_clients[i].loc_id); // 
                                log_info(log_msg);

                                // Responder com OK(01) 
                                sprintf(response_code_payload_str, "%02d", OK_SUCCESSFUL_DISCONNECT);
                                build_control_message(msg_response_to_client, sizeof(msg_response_to_client), OK_MSG, response_code_payload_str);
                                if (write(current_client_socket, msg_response_to_client, strlen(msg_response_to_client)) < 0) {
                                    log_error("Falha ao enviar OK(01) para REQ_DISCSEN");
                                } else {
                                    log_info("OK(01) para REQ_DISCSEN enviado ao cliente.");
                                }

                                // Desconectar cliente: fechar socket, limpar estruturas
                                close(current_client_socket);
                                client_sockets[i] = 0; // Liberar slot no array de sockets monitorados pelo select

                                // Limpar informações do cliente
                                if (connected_clients[i].id_cliente[0] != '\0') { // Apenas se estava realmente registrado
                                    if (num_connected_clients > 0) {
                                        num_connected_clients--;
                                    }
                                }
                                connected_clients[i].socket_fd = 0;
                                connected_clients[i].id_cliente[0] = '\0';
                                connected_clients[i].loc_id = -1;
                                // Resetar outros campos se houver (ex: status_risco)
                                
                                sprintf(log_msg, "Clientes ativos restantes: %d", num_connected_clients);
                                log_info(log_msg);

                            } else { // IdC não existe ou não corresponde ao socket 
                                sprintf(log_msg, "REQ_DISCSEN para IdC '%s' (socket %d) não encontrado ou IdC não corresponde. Esperado '%s'. Enviando ERROR(10).",
                                        client_id_from_payload, current_client_socket, connected_clients[i].id_cliente);
                                log_info(log_msg);
                                
                                sprintf(response_code_payload_str, "%02d", SENSOR_NOT_FOUND_ERROR);
                                build_control_message(msg_response_to_client, sizeof(msg_response_to_client), ERROR_MSG, response_code_payload_str);
                                if (write(current_client_socket, msg_response_to_client, strlen(msg_response_to_client)) < 0) {
                                    log_error("Falha ao enviar ERROR(10) para REQ_DISCSEN");
                                }
                                // O PDF não especifica explicitamente fechar a conexão aqui, mas é uma opção.
                                // Se o cliente enviou um ID inválido, pode ser um erro de protocolo.
                            }
                        } else if (code == REQ_SENSSTATUS) { // Payload é o IdC do sensor (SensID)
                            char sens_id_from_client[MAX_PIDS_LENGTH];
                            strncpy(sens_id_from_client, payload, sizeof(sens_id_from_client) - 1);
                            sens_id_from_client[sizeof(sens_id_from_client) - 1] = '\0';

                            // Verificar se o sens_id_from_client corresponde ao connected_clients[i].id_cliente
                            if (connected_clients[i].socket_fd == current_client_socket &&
                                strcmp(connected_clients[i].id_cliente, sens_id_from_client) == 0) {
                                
                                sprintf(log_msg, "REQ_SENSSTATUS %s", sens_id_from_client); // Conforme PDF p.10
                                log_info(log_msg);

                                // Simular ou verificar o status do sensor na base de dados do SS
                                // Para teste, podemos alternar o status ou ter um valor fixo.
                                // Suponha que connected_clients[i].status_risco armazena 0 (normal) ou 1 (falha)
                                // Vamos simular uma falha para testar o fluxo completo:
                                // connected_clients[i].status_risco = 1; // Para teste. Em um sistema real, seria atualizado por outros meios.

                                if (connected_clients[i].status_risco == 1) { // Falha detectada
                                    sprintf(log_msg, "Sensor %s status = 1 (failure detected)", sens_id_from_client); // Conforme PDF p.11
                                    log_info(log_msg);

                                    if (peer_socket_fd > 0 && p2p_current_state == P2P_FULLY_ESTABLISHED) {
                                        sprintf(log_msg, "Sending REQ_CHECKALERT %s to SL", sens_id_from_client); // Conforme PDF p.11
                                        log_info(log_msg);
                                        
                                        char msg_to_sl[MAX_MSG_SIZE];
                                        build_control_message(msg_to_sl, sizeof(msg_to_sl), REQ_CHECKALERT, sens_id_from_client);
                                        if (write(peer_socket_fd, msg_to_sl, strlen(msg_to_sl)) < 0) {
                                            log_error("SS: Falha ao enviar REQ_CHECKALERT para SL");
                                            // O que fazer se não conseguir contatar SL? Enviar erro ao cliente?
                                            // O PDF não cobre este caso de falha de comunicação SS-SL.
                                            // Por ora, o cliente não receberá resposta se isso falhar.
                                        } else {
                                            // SS agora aguarda RES_CHECKALERT ou ERROR do SL.
                                            // Esta é uma interação bloqueante no fluxo do PDF.
                                            // Em uma implementação real com select, o SS não bloquearia aqui,
                                            // mas guardaria o estado e esperaria a resposta do SL via select.
                                            // Para simplificar e seguir o fluxo linear do PDF para esta função:
                                            char sl_response_buffer[MAX_MSG_SIZE];
                                            ssize_t sl_bytes = read(peer_socket_fd, sl_response_buffer, sizeof(sl_response_buffer)-1);
                                            if (sl_bytes > 0) {
                                                sl_response_buffer[sl_bytes] = '\0';
                                                int sl_code; char sl_payload[MAX_MSG_SIZE];
                                                if(parse_message(sl_response_buffer, &sl_code, sl_payload, sizeof(sl_payload))){
                                                    char msg_to_client[MAX_MSG_SIZE];
                                                    if(sl_code == RES_CHECKALERT){
                                                        sprintf(log_msg, "SS: RES_CHECKALERT %s recebido do SL", sl_payload); // Conforme PDF p.12 [SL] Sending RES_CHECKALERT 3 to SS
                                                        log_info(log_msg);                                                       // E [SS] RES_CHECKALERT 3
                                                        // Enviar RES_SENSSTATUS (LocID) para o cliente
                                                        sprintf(log_msg, "SS: Sending RES_SENSSTATUS %s to CLIENT", sl_payload);
                                                        log_info(log_msg);
                                                        build_control_message(msg_to_client, sizeof(msg_to_client), RES_SENSSTATUS, sl_payload); // sl_payload é LocID
                                                        write(current_client_socket, msg_to_client, strlen(msg_to_client));
                                                    } else if (sl_code == ERROR_MSG && atoi(sl_payload) == SENSOR_NOT_FOUND_ERROR) {
                                                        log_info("SS: ERROR(10) received from SL"); // Conforme PDF p.12
                                                        sprintf(log_msg, "SS: Sending ERROR(10) to CLIENT");
                                                        log_info(log_msg);
                                                        // Enviar ERROR(10) para o cliente
                                                        build_control_message(msg_to_client, sizeof(msg_to_client), ERROR_MSG, sl_payload); // sl_payload é o código de erro "10"
                                                        write(current_client_socket, msg_to_client, strlen(msg_to_client));
                                                    }
                                                } else { log_error("SS: Falha ao parsear resposta do SL para REQ_CHECKALERT");}
                                            } else {log_error("SS: Falha ao ler resposta do SL ou SL desconectou");}
                                        }
                                    } else {
                                        log_error("SS: Sem conexão P2P com SL para enviar REQ_CHECKALERT.");
                                        // Enviar erro para o cliente? Ex: ERROR_SERVICE_UNAVAILABLE? Protocolo não define.
                                        // Por ora, o cliente não receberá resposta.
                                    }
                                } else { // Status do sensor é 0 (normal)
                                    sprintf(log_msg, "Sensor %s status = 0 (normal). Nenhuma ação de alerta.", sens_id_from_client);
                                    log_info(log_msg);
                                    // O PDF diz "nenhuma ação adicional é tomada." (p.10, 2.a)
                                    // Isso é problemático para o cliente que espera uma resposta.
                                    // Vamos enviar RES_SENSSTATUS com LocID = 0 para indicar normalidade.
                                    char msg_to_client[MAX_MSG_SIZE];
                                    char loc_id_normal[] = "0";
                                    build_control_message(msg_to_client, sizeof(msg_to_client), RES_SENSSTATUS, loc_id_normal);
                                    if(write(current_client_socket, msg_to_client, strlen(msg_to_client)) < 0){
                                        log_error("Falha ao enviar RES_SENSSTATUS (normal) para cliente.");
                                    } else {
                                        log_info("RES_SENSSTATUS (normal, LocID=0) enviado ao cliente.");
                                    }
                                }
                            } else {
                                log_error("REQ_SENSSTATUS de um IdC desconhecido ou não correspondente ao socket.");
                                // Enviar ERROR SENSOR_NOT_FOUND?
                            }
                        }
                        // else if (code == ...) { // Outros códigos de mensagem do cliente
                        //
                        // }
                        else {
                            sprintf(log_msg, "Servidor: Código de mensagem de cliente desconhecido ou não esperado: %d, Payload: '%s'", code, payload);
                            log_info(log_msg);
                        }

                    } else {
                        log_error("Falha ao parsear mensagem do cliente.");
                    }

                } else if (bytes_read == 0) {
                    // Cliente desconectou
                    log_info("Cliente desconectado.");
                    close(current_client_socket);
                    client_sockets[i] = 0; // Marcar como slot livre
                } else {
                    log_error("Erro ao ler do cliente.");
                }
            }
        }
        

        if (strcmp(buffer, "exit") == 0 && FD_ISSET(STDIN_FILENO, &read_fds)) break; // Condição de saída do loop principal

    }

    // --- LIMPEZA FINAL ---
    log_info("Encerrando e limpando sockets...");
    close(client_master_socket_fd);
    if (peer_socket_fd > 0) close(peer_socket_fd);
    
    if (peer_listen_socket_fd > 0) close(peer_listen_socket_fd);
        for (int i = 0; i < MAX_CLIENTS; i++) {
        connected_clients[i].socket_fd = 0; // 0 indica slot livre
        connected_clients[i].id_cliente[0] = '\0';
        connected_clients[i].loc_id = -1;
        connected_clients[i].status_risco = -1; // ou 0
    }

    log_info("Servidor encerrado.");
    return 0;
}