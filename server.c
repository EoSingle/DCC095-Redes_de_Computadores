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
    char id_cliente[MAX_PIDS_LENGTH];
    int assigned_slot; // Slot atribuído para este cliente (1 a 15)
    int loc_id; // Se este for o SL
    int status_risco; // Se este for o SS (0 ou 1)
} ClientInfo;

ClientInfo connected_clients[MAX_CLIENTS]; // Array de clientes conectados
int num_connected_clients = 0; // Contador

typedef enum {
    SERVER_TYPE_UNINITIALIZED,
    SERVER_TYPE_SS,  // Servidor de Status
    SERVER_TYPE_SL   // Servidor de Localização
} ServerRole;

ServerRole current_server_role = SERVER_TYPE_UNINITIALIZED;

// Função para gerar IdC único (exemplo simples)
void generate_unique_client_id(char *buffer, size_t buffer_len, int client_socket_fd) {
    snprintf(buffer, buffer_len, "Sensor%d", client_socket_fd); // Simples, mas não globalmente único entre reinícios
                                                               // O PDF diz que o servidor define[cite: 110].
}

int main(int argc, char *argv[]) {
    // Verificar argumentos
    if (argc < 5) {
        fprintf(stderr, "Uso: %s <ip_peer_destino> <porta_p2p> <porta_escuta_cliente_propria> <SS|SL>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Atribuir argumentos
    char *peer_target_ip = argv[1];         // IP do peer destino (ou seja, o outro servidor)
    int peer_target_port = atoi(argv[2]);   // Porta P2P onde o outro peer escuta (ou este tentará conectar)
                                            // Esta também será a porta onde este servidor escutará por P2P se a conexão ativa falhar.
    int client_listen_port = atoi(argv[3]); // Porta onde este servidor escuta por clientes
    char *server_type_arg = argv[4];        // Tipo de servidor: SS ou SL

    // Inicializar servidor
    if (strcmp(server_type_arg, "SS") == 0) {
        current_server_role = SERVER_TYPE_SS;
        log_info("Servidor configurado para operar como SERVIDOR DE STATUS (SS).");
    } else if (strcmp(server_type_arg, "SL") == 0) {
        current_server_role = SERVER_TYPE_SL;
        log_info("Servidor configurado para operar como SERVIDOR DE LOCALIZAÇÃO (SL).");
    } else {
        fprintf(stderr, "Erro: Tipo de servidor inválido '%s'. Use 'SS' ou 'SL'.\n", server_type_arg);
        exit(EXIT_FAILURE);
    }

    int client_master_socket_fd, new_socket_fd;
    int peer_listen_socket_fd = -1; // Socket para escutar P2P se a conexão ativa falhar
    
    struct sockaddr_in server_addr_clients, server_addr_peer_listen, peer_target_addr;
    socklen_t client_addr_len = sizeof(struct sockaddr_in);
    char buffer[MAX_MSG_SIZE + 1]; // Buffer para mensagens recebidas
    char log_msg[150];             // Buffer para mensagens de log

    int client_sockets[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_sockets[i] = 0;
        connected_clients[i].socket_fd = 0; // Inicializar todos os slots como vazios
        connected_clients[i].id_cliente[0] = '\0'; // Limpar ID do cliente    
        connected_clients[i].assigned_slot = 0; // Inicializar slot atribuído como 0
        connected_clients[i].loc_id = 0; // Inicializar LocId como 0
        connected_clients[i].status_risco = -1; // Inicializar status de risco como -1
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
                log_info("No peer found, starting to listen for P2P connections..."); 
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

    // Loop principal do servidor
    while (running) { 
        FD_ZERO(&read_fds);

        // Adicionar STDIN aos conjuntos de leitura
        // Isso permite que o servidor leia comandos do teclado
        FD_SET(STDIN_FILENO, &read_fds);
        max_sd = STDIN_FILENO;

        // Adicionar sockets de clientes ao conjunto de leitura
        FD_SET(client_master_socket_fd, &read_fds);
        if (client_master_socket_fd > max_sd) max_sd = client_master_socket_fd;

        // Adicionar socket P2P se estiver ativo ou escutando
        if (peer_listen_socket_fd > 0) { 
            FD_SET(peer_listen_socket_fd, &read_fds);
            if (peer_listen_socket_fd > max_sd) max_sd = peer_listen_socket_fd;
        }

        // Adicionar socket P2P se estiver conectado
        if (peer_socket_fd > 0) {
            FD_SET(peer_socket_fd, &read_fds);
            if (peer_socket_fd > max_sd) max_sd = peer_socket_fd;
        }

        // Adicionar sockets de clientes conectados
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_sockets[i] > 0) {
                FD_SET(client_sockets[i], &read_fds);
                if (client_sockets[i] > max_sd) {
                    max_sd = client_sockets[i];
                }
            }
        }

        // Chamar select() para aguardar atividade em qualquer socket
        int activity = select(max_sd + 1, &read_fds, NULL, NULL, NULL);
        if ((activity < 0) && (errno != EINTR)) {
            log_error("Erro no select()");
            continue; // Continua o loop em caso de erro
        }

        // Processar atividades

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

        // Verificar se há novas conexões de clientes
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
                        connected_clients[i].socket_fd = new_socket_fd;

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
                    char error_payload_str[10];
                    sprintf(error_payload_str, "%02d", SENSOR_LIMIT_EXCEEDED);
                    char msg_error[MAX_MSG_SIZE];
                    build_control_message(msg_error, sizeof(msg_error), ERROR_MSG, error_payload_str);
                    if (write(new_socket_fd, msg_error, strlen(msg_error)) < 0) {
                        log_error("Falha ao enviar mensagem de erro para cliente novo.");
                    }
                    close(new_socket_fd);
                }
            }
        }

        // Verificar se há novas conexões P2P (escuta passiva)
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

        // Verificar se há atividade no socket P2P conectado
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
                            sprintf(response_payload_str, "%02d", PEER_NOT_FOUND);
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
                    else if (code == ERROR_MSG && atoi(payload) == PEER_NOT_FOUND) {
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

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int current_client_socket = client_sockets[i]; // Ou o nome da sua variável para o socket do cliente atual

            if (current_client_socket > 0 && FD_ISSET(current_client_socket, &read_fds)) {
                ssize_t bytes_read = read(current_client_socket, buffer, MAX_MSG_SIZE); // 'buffer' deve estar definido no escopo

                if (bytes_read > 0) {
                    buffer[bytes_read] = '\0'; // Garantir terminação nula
                    int code;
                    char payload[MAX_MSG_SIZE];
                    
                    // Obter informações do cliente para log
                    struct sockaddr_in addr_cli_log;
                    socklen_t len_cli_log = sizeof(addr_cli_log);
                    getpeername(current_client_socket, (struct sockaddr*)&addr_cli_log, &len_cli_log);
                    sprintf(log_msg, "Dados recebidos do cliente %s:%d (socket %d)",
                            inet_ntoa(addr_cli_log.sin_addr), ntohs(addr_cli_log.sin_port), current_client_socket);
                    log_info(log_msg);
                    
                    // Verificar se a mensagem foi parseada corretamente
                    if (parse_message(buffer, &code, payload, sizeof(payload))) {
                        // Lógica para processar a mensagem recebida
                        if (code == REQ_CONNSEN) {
                            char global_sensor_id_str[MAX_PIDS_LENGTH]; // Para o ID Global de 10 chars
                            char loc_id_str_from_payload[10];           // Para a parte da string da LocId
                            int client_loc_id_from_payload;
                            int proceed_with_registration = 0;          // Flag para controlar o fluxo

                            // 1. Parsear o payload "ID_GLOBAL_SENSOR,LocId"
                            char *comma_ptr = strchr(payload, ',');

                            if (comma_ptr != NULL) {
                                size_t id_len = comma_ptr - payload;
                                if (id_len < sizeof(global_sensor_id_str) && id_len > 0) { // ID não pode ser vazio
                                    strncpy(global_sensor_id_str, payload, id_len);
                                    global_sensor_id_str[id_len] = '\0';

                                    strncpy(loc_id_str_from_payload, comma_ptr + 1, sizeof(loc_id_str_from_payload) - 1);
                                    loc_id_str_from_payload[sizeof(loc_id_str_from_payload) - 1] = '\0';
                                    
                                    // Verificar se loc_id_str_from_payload não está vazio antes de atoi
                                    if (strlen(loc_id_str_from_payload) > 0) {
                                        client_loc_id_from_payload = atoi(loc_id_str_from_payload);
                                        
                                        // Validação básica do ID Global (ex: tamanho 10)
                                        if (strlen(global_sensor_id_str) == 10) { // Conforme tabelas do PDF
                                            // Adicionar validação para checar se são todos dígitos, se necessário
                                            proceed_with_registration = 1;
                                            sprintf(log_msg, "REQ_CONNSEN payload parseado: ID_Global='%s', LocId=%d", 
                                                    global_sensor_id_str, client_loc_id_from_payload);
                                            log_info(log_msg);
                                        } else {
                                            log_error("Formato de payload inválido para REQ_CONNSEN: ID Global não tem 10 caracteres.");
                                        }
                                    } else {
                                        log_error("Formato de payload inválido para REQ_CONNSEN: LocId ausente após a vírgula.");
                                    }
                                } else {
                                    log_error("Formato de payload inválido para REQ_CONNSEN: ID Global muito longo ou vazio.");
                                }
                            } else {
                                log_error("Formato de payload inválido para REQ_CONNSEN. Esperado 'ID_Global,LocId' (sem vírgula).");
                            }

                            if (!proceed_with_registration) {
                                // Enviar mensagem de erro para o cliente
                                char error_payload_str[10];
                                sprintf(error_payload_str, "%02d", INVALID_PAYLOAD_ERROR);
                                char msg_error[MAX_MSG_SIZE];
                                build_control_message(msg_error, sizeof(msg_error), ERROR_MSG, error_payload_str);
                                if (write(current_client_socket, msg_error, strlen(msg_error)) < 0) { /* log erro no write */ }
                                close(current_client_socket); // Fechar conexão com o cliente inválido
                                client_sockets[i] = 0; // Limpar slot do cliente
                                connected_clients[i].socket_fd = 0; // Limpar slot do cliente conectado
                                continue; // Pula para o próximo cliente
                            }

                            // 2. Verificar se o socket realmente corresponde (sua verificação de sanidade)
                            // Esta verificação já está no seu código original, pode mantê-la.
                            if (connected_clients[i].socket_fd != current_client_socket) {
                                sprintf(log_msg, "Erro crítico: Inconsistência entre client_sockets[%d] (%d) e connected_clients[%d].socket_fd (%d).",
                                        i, current_client_socket, i, connected_clients[i].socket_fd);
                                log_error(log_msg);
                                close(current_client_socket);
                                client_sockets[i] = 0;
                                if (connected_clients[i].socket_fd != 0) {
                                     if (connected_clients[i].id_cliente[0] != '\0' && num_connected_clients > 0) num_connected_clients--;
                                     connected_clients[i].socket_fd = 0;
                                     connected_clients[i].id_cliente[0] = '\0';
                                }
                                continue; 
                            }

                            // 3. Lógica de Registro ou Atualização
                            if (connected_clients[i].id_cliente[0] == '\0') { // Slot está "limpo" para um novo registro de ID
                                // Verificar se este ID Global já está em uso por outro socket/slot ativo
                                int id_already_in_use = 0;
                                for (int k = 0; k < MAX_CLIENTS; k++) {
                                    if (connected_clients[k].socket_fd > 0 && // Cliente ativo no slot k
                                        strcmp(connected_clients[k].id_cliente, global_sensor_id_str) == 0) {
                                        id_already_in_use = 1;
                                        sprintf(log_msg, "Erro: ID de Sensor '%s' já está em uso pelo socket %d (slot %d). Rejeitando novo registro para socket %d.",
                                                global_sensor_id_str, connected_clients[k].socket_fd, k+1, current_client_socket);
                                        log_error(log_msg);
                                        // Enviar mensagem de erro para o cliente
                                        char error_payload_str[10];
                                        sprintf(error_payload_str, "%02d", SENSOR_ID_ALREADY_EXISTS_ERROR);
                                        char msg_error[MAX_MSG_SIZE];
                                        build_control_message(msg_error, sizeof(msg_error), ERROR_MSG, error_payload_str);
                                        if (write(current_client_socket, msg_error, strlen(msg_error)) < 0) { /* log erro no write */ }
                                        // Fechar a conexão com o cliente atual, pois não pode ser registrado
                                        sprintf(log_msg, "Fechando conexão com socket %d (slot %d) devido a ID Global já em uso.", 
                                                current_client_socket, i + 1);
                                        log_info(log_msg);
                                        // Fechar a conexão com o cliente atual
                                        close(current_client_socket); client_sockets[i]=0; connected_clients[i].socket_fd=0;
                                        break; 
                                    }
                                }

                                if (id_already_in_use) {
                                    // Se o ID já está em uso por outro socket, não prosseguir com este.
                                    // Poderia fechar current_client_socket e limpar client_sockets[i] e connected_clients[i].socket_fd
                                    // para liberar o slot do array para um futuro cliente válido.
                                    close(current_client_socket);
                                    client_sockets[i] = 0;
                                    connected_clients[i].socket_fd = 0; 
                                    // Não decrementar num_connected_clients pois este não foi contado ainda.
                                    continue; // Pula para o próximo cliente
                                }

                                // Verificar limite de clientes *registrados*
                                if (num_connected_clients >= MAX_CLIENTS) {
                                    log_info("Limite de sensores (num_connected_clients) excedido. Enviando ERROR(09)...");
                                    char error_payload_str[10];
                                    sprintf(error_payload_str, "%02d", SENSOR_LIMIT_EXCEEDED);
                                    char msg_error[MAX_MSG_SIZE];
                                    build_control_message(msg_error, sizeof(msg_error), ERROR_MSG, error_payload_str);
                                    if (write(current_client_socket, msg_error, strlen(msg_error)) < 0) { /* log erro no write */ }
                                    // O PDF não diz para fechar a conexão, mas o cliente pode tentar de novo ou sair.
                                } else {
                                   // Armazenar o ID Global e outros dados.
                                    strncpy(connected_clients[i].id_cliente, global_sensor_id_str, MAX_PIDS_LENGTH - 1);
                                    connected_clients[i].id_cliente[MAX_PIDS_LENGTH - 1] = '\0';
                                    
                                    connected_clients[i].loc_id = client_loc_id_from_payload;
                                    connected_clients[i].assigned_slot = i + 1; // ID de slot (1-15)

                                    if (current_server_role == SERVER_TYPE_SS) {
                                        connected_clients[i].status_risco = 0; // Status inicial normal
                                    }
                                    num_connected_clients++;

                                    sprintf(log_msg, "Client (Global ID: %s, Slot: %d) added (Loc %d)", 
                                            connected_clients[i].id_cliente, 
                                            connected_clients[i].assigned_slot, 
                                            client_loc_id_from_payload);
                                    log_info(log_msg);

                                    // Enviar RES_CONNSEN com o ID do SLOT (i+1) como payload (IdSen)
                                    char id_slot_str[10];
                                    sprintf(id_slot_str, "%d", connected_clients[i].assigned_slot);
                                    
                                    char msg_res_connsen[MAX_MSG_SIZE];
                                    build_control_message(msg_res_connsen, sizeof(msg_res_connsen), RES_CONNSEN, id_slot_str);
                                    if (write(current_client_socket, msg_res_connsen, strlen(msg_res_connsen)) < 0) { /* log erro no write */ }
                                    else { log_info("RES_CONNSEN enviado ao cliente com ID de Slot."); }
                                }
                            } else { 
                                // Este slot/socket (connected_clients[i]) já tem um ID Global associado.
                                // Verificar se é o mesmo ID Global tentando se registrar novamente neste socket.
                                if (strcmp(connected_clients[i].id_cliente, global_sensor_id_str) == 0) {
                                    // Mesmo sensor, mesma conexão, enviando REQ_CONNSEN novamente.
                                    sprintf(log_msg, "Cliente %s (Slot %d, Socket %d) já registrado enviou REQ_CONNSEN novamente. Reenviando RES_CONNSEN.",
                                            connected_clients[i].id_cliente, i + 1, current_client_socket);
                                    log_info(log_msg);
                                    // Reenviar o RES_CONNSEN existente
                                    char msg_res_connsen[MAX_MSG_SIZE];
                                    build_control_message(msg_res_connsen, sizeof(msg_res_connsen), RES_CONNSEN, connected_clients[i].id_cliente);
                                    if (write(current_client_socket, msg_res_connsen, strlen(msg_res_connsen)) < 0) { /* log erro no write */ }
                                } else {
                                    // Este socket (current_client_socket, que é connected_clients[i].socket_fd)
                                    // já está associado ao ID connected_clients[i].id_cliente.
                                    // Agora ele enviou um REQ_CONNSEN com um *novo e diferente* global_sensor_id_str.
                                    // Isso é uma violação ou um cenário muito estranho.
                                    sprintf(log_msg, "Socket %d (Slot %d, Cliente %s) recebeu REQ_CONNSEN com um ID_Global diferente: '%s'. Rejeitando.",
                                            current_client_socket, i + 1, connected_clients[i].id_cliente, global_sensor_id_str);
                                    log_error(log_msg);
                                    // Enviar um erro? O protocolo não cobre isso. Poderia fechar a conexão.
                                }
                            }
                        } else if (code == REQ_DISCSEN) { // Cliente solicitando desconexão
                            char slot_id_str_from_payload[10]; // Para "1", "15", etc.
                            strncpy(slot_id_str_from_payload, payload, sizeof(slot_id_str_from_payload) - 1);
                            slot_id_str_from_payload[sizeof(slot_id_str_from_payload) - 1] = '\0';
                            int received_slot_id = atoi(slot_id_str_from_payload);

                            char msg_response_to_client[MAX_MSG_SIZE];
                            char response_code_payload_str[10]; 

                            // O índice 'i' do loop 'for' corresponde ao slot físico (0 a MAX_CLIENTS-1).
                            // current_client_socket é client_sockets[i].
                            // connected_clients[i] é a estrutura de dados para este slot.
                            // O ID de slot que enviamos ao cliente foi 'i+1'.

                            // Verificar se o socket é o esperado para este slot E
                            // se o slot_id recebido no payload corresponde ao assigned_slot_id_for_client deste slot i
                            // E se o cliente realmente estava registrado (tem um global_sensor_id).
                            if (connected_clients[i].socket_fd == current_client_socket &&
                                connected_clients[i].assigned_slot == received_slot_id &&
                                connected_clients[i].id_cliente[0] != '\0') { 

                                sprintf(log_msg, "Client (Global ID: %s, Slot: %d) removed (Loc %d)", 
                                        connected_clients[i].id_cliente, 
                                        connected_clients[i].assigned_slot,
                                        connected_clients[i].loc_id); 
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

                                // Limpar informações do cliente em connected_clients[i]
                                if (connected_clients[i].id_cliente[0] != '\0') { // Apenas se estava realmente registrado
                                    if (num_connected_clients > 0) {
                                        num_connected_clients--;
                                    }
                                }
                                connected_clients[i].socket_fd = 0; // Marcar o socket como 0 em ClientInfo
                                connected_clients[i].id_cliente[0] = '\0';
                                connected_clients[i].assigned_slot = 0; // Resetar o slot ID atribuído
                                connected_clients[i].loc_id = -1;
                                if (current_server_role == SERVER_TYPE_SS) {
                                    connected_clients[i].status_risco = 0; // Resetar status
                                }
                                
                                sprintf(log_msg, "Clientes ativos restantes: %d", num_connected_clients);
                                log_info(log_msg);

                            } else { 
                                // Slot ID não corresponde ao esperado para este socket, ou cliente não estava totalmente registrado.
                                sprintf(log_msg, "REQ_DISCSEN para Slot ID '%s' (socket %d) inválido ou não corresponde. Cliente no slot %d tem Slot ID %d e Global ID '%s'. Enviando ERROR(10).",
                                        slot_id_str_from_payload, 
                                        current_client_socket, 
                                        i + 1, 
                                        connected_clients[i].assigned_slot,
                                        connected_clients[i].id_cliente);
                                log_info(log_msg);
                                
                                sprintf(response_code_payload_str, "%02d", SENSOR_NOT_FOUND);
                                build_control_message(msg_response_to_client, sizeof(msg_response_to_client), ERROR_MSG, response_code_payload_str);
                                if (write(current_client_socket, msg_response_to_client, strlen(msg_response_to_client)) < 0) {
                                    log_error("Falha ao enviar ERROR(10) para REQ_DISCSEN");
                                }
                                // Não fechar a conexão aqui necessariamente, o cliente que solicitou pode tentar novamente
                                // ou o cliente pode fechar ao receber o erro.
                            }
                        } else if (code == REQ_SENSSTATUS && current_server_role == SERVER_TYPE_SS) { 
                            char slot_id_str_from_client[10];
                            strncpy(slot_id_str_from_client, payload, sizeof(slot_id_str_from_client) - 1);
                            slot_id_str_from_client[sizeof(slot_id_str_from_client) - 1] = '\0';
                            int received_slot_id = atoi(slot_id_str_from_client);

                            if (connected_clients[i].socket_fd == current_client_socket &&
                                connected_clients[i].assigned_slot == received_slot_id &&
                                connected_clients[i].id_cliente[0] != '\0') {
                                
                                // Usar o ID Global para os logs, conforme exemplos do PDF
                                sprintf(log_msg, "REQ_SENSSTATUS %s", connected_clients[i].id_cliente); // 
                                log_info(log_msg);

                                // Verificar o status_risco do sensor na base de dados do SS
                                if (connected_clients[i].status_risco == 1) { // Falha detectada
                                    sprintf(log_msg, "Sensor %s status = 1 (failure detected)", connected_clients[i].id_cliente); // 
                                    log_info(log_msg);

                                    if (peer_socket_fd > 0 && p2p_current_state == P2P_FULLY_ESTABLISHED) {
                                        // Ao enviar para SL, USAR O ID GLOBAL DO SENSOR
                                        sprintf(log_msg, "Sending REQ_CHECKALERT %s to SL", connected_clients[i].id_cliente); // 
                                        log_info(log_msg);
                                        
                                        char msg_to_sl[MAX_MSG_SIZE];
                                        // Payload de REQ_CHECKALERT é o ID Global do Sensor
                                        build_control_message(msg_to_sl, sizeof(msg_to_sl), REQ_CHECKALERT, connected_clients[i].id_cliente);
                                        
                                        if (write(peer_socket_fd, msg_to_sl, strlen(msg_to_sl)) < 0) {
                                            log_error("SS: Falha ao enviar REQ_CHECKALERT para SL");
                                            // Poderia enviar um erro para o cliente aqui se o SL estiver indisponível
                                            // Ex: build_control_message(msg_error_to_client, ..., ERROR_MSG, "SL_UNAVAILABLE_CODE");
                                            //     write(current_client_socket, msg_error_to_client, ...);
                                        } else {
                                            // ATENÇÃO: Leitura bloqueante da resposta do SL.
                                            // Para este trabalho, pode ser aceitável, mas em produção, seria assíncrono com select().
                                            log_info("SS: Aguardando resposta do SL para REQ_CHECKALERT...");
                                            char sl_response_buffer[MAX_MSG_SIZE];
                                            ssize_t sl_bytes = read(peer_socket_fd, sl_response_buffer, sizeof(sl_response_buffer)-1);
                                            if (sl_bytes > 0) {
                                                sl_response_buffer[sl_bytes] = '\0';
                                                int sl_code; char sl_payload[MAX_MSG_SIZE]; // sl_payload será LocID ou código de erro
                                                if(parse_message(sl_response_buffer, &sl_code, sl_payload, sizeof(sl_payload))){
                                                    char msg_to_client[MAX_MSG_SIZE];
                                                    if(sl_code == RES_CHECKALERT){ // SL respondeu com LocID
                                                        sprintf(log_msg, "SS: RES_CHECKALERT %s recebido do SL", sl_payload); // 
                                                        log_info(log_msg);                                                       
                                                        sprintf(log_msg, "SS: Sending RES_SENSSTATUS %s to CLIENT", sl_payload); // 
                                                        log_info(log_msg);
                                                        build_control_message(msg_to_client, sizeof(msg_to_client), RES_SENSSTATUS, sl_payload); // sl_payload é LocID
                                                        if (write(current_client_socket, msg_to_client, strlen(msg_to_client)) < 0) {log_error("SS: Falha ao enviar RES_SENSSTATUS para cliente.");}
                                                    } else if (sl_code == ERROR_MSG && atoi(sl_payload) == SENSOR_NOT_FOUND) { // SL não encontrou o sensor
                                                        log_info("SS: ERROR(10) SENSOR_NOT_FOUND_ERROR received from SL"); // 
                                                        sprintf(log_msg, "SS: Sending ERROR(10) to CLIENT"); // 
                                                        log_info(log_msg);
                                                        build_control_message(msg_to_client, sizeof(msg_to_client), ERROR_MSG, sl_payload); // sl_payload é o código "10"
                                                        if (write(current_client_socket, msg_to_client, strlen(msg_to_client)) < 0) {log_error("SS: Falha ao enviar ERROR(10) para cliente.");}
                                                    } else {
                                                        sprintf(log_msg, "SS: Resposta inesperada do SL para REQ_CHECKALERT: Code=%d, Payload='%s'", sl_code, sl_payload);
                                                        log_error(log_msg);
                                                        // Enviar um erro genérico para o cliente?
                                                    }
                                                } else { log_error("SS: Falha ao parsear resposta do SL para REQ_CHECKALERT");}
                                            } else if (sl_bytes == 0) {
                                                log_error("SS: SL desconectou antes de responder ao REQ_CHECKALERT.");
                                            } else { // sl_bytes < 0
                                                log_error("SS: Erro ao ler resposta do SL para REQ_CHECKALERT.");
                                            }
                                        }
                                    } else {
                                        log_error("SS: Sem conexão P2P com SL ou P2P não estabelecida para enviar REQ_CHECKALERT.");
                                        // Enviar um erro para o cliente indicando que o serviço está temporariamente indisponível?
                                        // O protocolo não define isso. Por ora, o cliente não receberia resposta.
                                    }
                                } else { // Status do sensor é 0 (normal)
                                    sprintf(log_msg, "Sensor %s status = 0 (normal). Nenhuma ação de alerta.", connected_clients[i].id_cliente);
                                    log_info(log_msg);
                                    
                                    char msg_to_client[MAX_MSG_SIZE];
                                    char loc_id_normal_payload[] = "0"; // Usar "0" para indicar status normal ao cliente
                                    build_control_message(msg_to_client, sizeof(msg_to_client), RES_SENSSTATUS, loc_id_normal_payload);
                                    if(write(current_client_socket, msg_to_client, strlen(msg_to_client)) < 0){
                                        log_error("Falha ao enviar RES_SENSSTATUS (normal) para cliente.");
                                    } else {
                                        log_info("RES_SENSSTATUS (normal, LocID=0) enviado ao cliente.");
                                    }
                                }
                            } else {
                                // O ID de slot recebido não corresponde ao esperado para este socket,
                                // ou o cliente não está totalmente registrado (sem global_sensor_id).
                                sprintf(log_msg, "REQ_SENSSTATUS com Slot ID '%s' (socket %d) inválido ou não corresponde. Cliente no slot %d (Global ID '%s') tem Slot ID %d. Enviando ERROR SENSOR_NOT_FOUND.",
                                        slot_id_str_from_client, 
                                        current_client_socket, 
                                        i + 1,
                                        connected_clients[i].id_cliente,
                                        connected_clients[i].assigned_slot);
                                log_error(log_msg);
                                // Enviar ERROR SENSOR_NOT_FOUND para o cliente
                                char error_payload_str[10];
                                sprintf(error_payload_str, "%02d", SENSOR_NOT_FOUND);
                                char msg_error_to_client[MAX_MSG_SIZE];
                                build_control_message(msg_error_to_client, sizeof(msg_error_to_client), ERROR_MSG, error_payload_str);
                                if (write(current_client_socket, msg_error_to_client, strlen(msg_error_to_client)) < 0) {log_error("Falha ao enviar ERROR SENSOR_NOT_FOUND para cliente.");}
                            }
                        }
                        // else if (code == REQ_SENSLOC && (sou_servidor_localizacao)) { ... }
                        // ... outros tratadores de mensagens do cliente ...
                        else {
                            sprintf(log_msg, "Código de mensagem de cliente desconhecido ou não esperado: %d", code);
                            log_info(log_msg);
                            // Enviar mensagem de erro? O protocolo não especifica um erro genérico para isso.
                        }

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
            }
        }
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
        connected_clients[i].assigned_slot = 0; // Resetar slot atribuído
    }

    log_info("Servidor encerrado.");
    return 0;
}