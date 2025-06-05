#include "common.h"
#include <sys/select.h>  // Para select() e fd_set
#include <errno.h>       // Para errno

#define MAX_CLIENTS 15   // Conforme especificado, cada servidor trata até 15 equipamentos

int peer_socket_fd = -1; // Socket para conexão P2P ativa

// Estado da conexão P2P
typedef enum {
    P2P_DISCONNECTED,
    P2P_ACTIVE_CONNECTING,      // S_i tentou connect(), aguardando resultado ou enviando REQ
    P2P_PASSIVE_LISTENING,      // S_i falhou em conectar, agora escuta
    P2P_REQ_SENT,               // S_i (ativo) enviou REQ_CONNPEER, aguarda RES
    P2P_RES_SENT_AWAITING_RES,  // S_j (passivo) recebeu REQ, enviou RES, aguarda RES de S_i
    P2P_FULLY_ESTABLISHED,      // S_i e S_j estão conectados e trocaram IDs
    P2P_DISCONNECT_REQ_SENT     // S_i enviou REQ_DISCPEER, aguardando confirmação de desconexão
} P2PState;

P2PState p2p_current_state = P2P_DISCONNECTED;
char my_pids_for_peer[MAX_PIDS_LENGTH] = ""; // ID que este servidor atribui ao peer
char peer_pids_for_me[MAX_PIDS_LENGTH] = ""; // ID que o peer atribuiu a este servidor

// Estrutura para armazenar informações de clientes conectados
typedef struct {
    int socket_fd;                      // Socket do cliente conectado
    char id_cliente[MAX_PIDS_LENGTH];   // ID do cliente (10 caracteres numéricos)
    int assigned_slot;                  // Slot atribuído para este cliente (1 a 15)
    int loc_id;                         // Se este for o SL (1 a 10)
    int status_risco;                   // Se este for o SS (0 ou 1)
} ClientInfo;

ClientInfo connected_clients[MAX_CLIENTS]; // Array de clientes conectados
int num_connected_clients = 0;             // Contador

// Enumeração para o papel do servidor (SS ou SL)
typedef enum {
    SERVER_TYPE_UNINITIALIZED,  // Servidor não inicializado
    SERVER_TYPE_SS,             // Servidor de Status
    SERVER_TYPE_SL              // Servidor de Localização
} ServerRole;

ServerRole current_server_role = SERVER_TYPE_UNINITIALIZED; // Inicializa como não definido

int main(int argc, char *argv[]) {
    // Verificar argumentos
    if (argc < 5) {
        fprintf(stderr, "Uso: %s <ip_peer_destino> <porta_p2p> <porta_escuta_clientes> <SS|SL>\n", argv[0]);
        fprintf(stderr, "Exemplo: %s 127.0.0.1 60000 61000 SS\n", argv[0]);
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

    int client_master_socket_fd,    // Socket mestre para escutar clientes
        new_socket_fd;              // Socket para nova conexão de cliente
    int peer_listen_socket_fd = -1; // Socket para escutar P2P se a conexão ativa falhar
    
    struct sockaddr_in server_addr_clients, // Endereço do servidor para escutar clientes
        server_addr_peer_listen,            // Endereço do servidor para escutar P2P
        peer_target_addr;                   // Endereço do peer destino para conexão P2P ativa

    socklen_t client_addr_len = sizeof(struct sockaddr_in); // Tamanho do endereço do cliente

    char buffer[MAX_MSG_SIZE + 1]; // Buffer para mensagens recebidas
    char log_msg[150];             // Buffer para mensagens de log

    int client_sockets[MAX_CLIENTS]; // Array para armazenar sockets de clientes conectados

    // Inicializar todos os sockets de clientes como 0 (desconectados)
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_sockets[i] = 0;
        connected_clients[i].socket_fd = 0;        // Inicializar todos os slots como vazios
        connected_clients[i].id_cliente[0] = '\0'; // Limpar ID do cliente    
        connected_clients[i].assigned_slot = 0;    // Inicializar slot atribuído como 0
        connected_clients[i].loc_id = 0;           // Inicializar LocId como 0
        connected_clients[i].status_risco = -1;    // Inicializar status de risco como -1
    }

    fd_set read_fds; // Conjunto de descritores de arquivo para select()
    int max_sd;      // Variável para armazenar o maior descritor de socket

    // --- INICIALIZAÇÃO DO SOCKET DE ESCUTA DE CLIENTES ---
    if ((client_master_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {  // Cria o socket mestre
        log_error("Falha ao criar socket mestre de clientes.");
        exit(EXIT_FAILURE); 
    }
    log_info("Socket mestre de clientes criado.");
    int opt = 1;

    // Definir opções de socket para permitir reutilização do endereço
    if (setsockopt(client_master_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) { 
        log_error("Falha ao definir opções de socket para o socket mestre de clientes.");
        close(client_master_socket_fd);
        exit(EXIT_FAILURE);
    }
    server_addr_clients.sin_family = AF_INET;                 // Família de endereços IPv4
    server_addr_clients.sin_addr.s_addr = INADDR_ANY;         // Aceita conexões de qualquer endereço IP
    server_addr_clients.sin_port = htons(client_listen_port); // Porta onde o servidor escuta por clientes
    
    // Bind do socket mestre de clientes
    if (bind(client_master_socket_fd, (struct sockaddr *)&server_addr_clients, sizeof(server_addr_clients)) < 0) {
        close(client_master_socket_fd);
        exit(EXIT_FAILURE); 
    }
    sprintf(log_msg, "Socket mestre de clientes fez bind na porta %d.", client_listen_port); log_info(log_msg);
    
    // Escutar por conexões de clientes
    if (listen(client_master_socket_fd, SERVER_BACKLOG) < 0) {
        close(client_master_socket_fd); 
        exit(EXIT_FAILURE); 
    }
    sprintf(log_msg, "Servidor ouvindo por clientes na porta %d...", client_listen_port); log_info(log_msg);

    // --- TENTATIVA DE CONEXÃO P2P ATIVA ---
    log_info("Tentando conectar ao peer...");
    // Criar socket para tentar conectar ao peer
    if ((peer_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        log_error("Falha ao criar socket para conexão P2P ativa.");
    } else {
        peer_target_addr.sin_family = AF_INET;               // Família de endereços IPv4
        peer_target_addr.sin_port = htons(peer_target_port); // Porta do peer destino
        // Converter o endereço IP do peer destino
        if (inet_pton(AF_INET, peer_target_ip, &peer_target_addr.sin_addr) <= 0) {
            log_error("Endereço IP do peer inválido para conexão P2P ativa.");
            close(peer_socket_fd);
            peer_socket_fd = -1;
        } else {
            if (connect(peer_socket_fd, (struct sockaddr *)&peer_target_addr, sizeof(peer_target_addr)) < 0) { // P2P fail
                sprintf(log_msg, "Falha ao conectar ao peer %s:%d. %s.", peer_target_ip, peer_target_port, strerror(errno));
                log_info(log_msg);
                close(peer_socket_fd);
                peer_socket_fd = -1; // Marcar que a conexão ativa falhou

                // Se falhou, prepara para escutar P2P passivamente
                log_info("No peer found, starting to listen for P2P connections..."); 
                if ((peer_listen_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
                    log_error("Falha ao criar socket para escuta P2P passiva.");
                    peer_listen_socket_fd = -1; // Marcar como inválido                
                } else {
                    if (setsockopt(peer_listen_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
                        log_error("Falha ao definir opções de socket para escuta P2P passiva.");
                        close(peer_listen_socket_fd);
                        peer_listen_socket_fd = -1; // Marcar como inválido
                        return -1; // Sair com erro
                    }
                    server_addr_peer_listen.sin_family = AF_INET;               // Família de endereços IPv4
                    server_addr_peer_listen.sin_addr.s_addr = INADDR_ANY;       // Aceita conexões de qualquer endereço IP
                    server_addr_peer_listen.sin_port = htons(peer_target_port); // Escuta na mesma porta P2P
                    // Bind do socket de escuta P2P passiva
                    if (bind(peer_listen_socket_fd, (struct sockaddr *)&server_addr_peer_listen, sizeof(server_addr_peer_listen)) < 0) {
                        close(peer_listen_socket_fd); 
                        peer_listen_socket_fd = -1;
                    } else if (listen(peer_listen_socket_fd, 1) < 0) { // Só escuta por uma conexão P2P
                        close(peer_listen_socket_fd); 
                        peer_listen_socket_fd = -1; 
                    } else {
                        sprintf(log_msg, "Servidor ouvindo por conexão P2P na porta %d...", peer_target_port);
                        log_info(log_msg);
                    }
                }
            } else { // P2P success
                sprintf(log_msg, "Conectado ao peer %s:%d no socket P2P %d. Enviando REQ_CONNPEER...",
                        peer_target_ip, peer_target_port, peer_socket_fd);
                log_info(log_msg);
                p2p_current_state = P2P_ACTIVE_CONNECTING;

                char msg_buffer[MAX_MSG_SIZE];

                // Construir mensagem REQ_CONNPEER
                build_control_message(msg_buffer, sizeof(msg_buffer), REQ_CONNPEER, NULL);
                if (write(peer_socket_fd, msg_buffer, strlen(msg_buffer)) < 0) {
                    log_error("Falha ao enviar REQ_CONNPEER");
                    close(peer_socket_fd);
                    peer_socket_fd = -1;
                    p2p_current_state = P2P_DISCONNECTED; // Voltar ao estado desconectado
                } else {
                    log_info("REQ_CONNPEER enviado.");
                    p2p_current_state = P2P_REQ_SENT;     // Agora aguardando RES_CONNPEER
                }
            }
        }
    }

    log_info("Aguardando conexões (clientes/P2P) ou entrada do teclado...");

    printf("Comandos disponíveis:\n");
    printf("  kill                      - Envia REQ_DISCPEER para o peer, se conectado.\n");
    printf("  exit                      - Encerra o servidor.\n");
    printf("  set_risk <SensorID> <0|1> - Atualiza o status de risco de um sensor (SS).\n");

    // Loop principal do servidor
    while (1) { 
        FD_ZERO(&read_fds); // Limpar o conjunto de descritores de arquivo

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
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            // Ler o comando do teclado
            // Usar fgets para ler a linha inteira de forma mais segura
            char command_buffer[MAX_MSG_SIZE];
            if (fgets(command_buffer, sizeof(command_buffer), stdin) != NULL) {
                // Remover o caractere de nova linha, se presente
                command_buffer[strcspn(command_buffer, "\n")] = 0;

                // Variáveis auxiliares para parsing de comandos
                char command_name[20];
                char param1_sensor_id[MAX_PIDS_LENGTH];
                int param2_new_status;

                sprintf(log_msg, "Comando do teclado recebido: '%s'", command_buffer);
                log_info(log_msg);

                // Lógica para processar comandos do teclado
                if (strcmp(command_buffer, "kill") == 0) {
                    if (p2p_current_state == P2P_FULLY_ESTABLISHED && peer_socket_fd > 0) {
                        sprintf(log_msg, "Comando 'kill' recebido. Enviando REQ_DISCPEER para o peer %s...", my_pids_for_peer);
                        log_info(log_msg);

                        char msg_req_disc[MAX_MSG_SIZE];
                        build_control_message(msg_req_disc, sizeof(msg_req_disc), REQ_DISCPEER, my_pids_for_peer);
                        
                        if (write(peer_socket_fd, msg_req_disc, strlen(msg_req_disc)) < 0) {
                            log_error("Falha ao enviar REQ_DISCPEER");
                            close(peer_socket_fd);
                            peer_socket_fd = -1;
                            p2p_current_state = P2P_DISCONNECTED;
                        } else {
                            log_info("REQ_DISCPEER enviado.");
                            p2p_current_state = P2P_DISCONNECT_REQ_SENT;
                        }
                    } else {
                        log_info("No peer connected to close connection");
                    }
                } else if (strcmp(command_buffer, "exit") == 0) {
                    log_info("Comando 'exit' recebido. Encerrando o servidor...");
                    break;
                // set_risk <SensorID> <0|1>
                } else if (sscanf(command_buffer, "%19s %49s %d", command_name, param1_sensor_id, &param2_new_status) == 3 && 
                        strcmp(command_name, "set_risk") == 0) {
                    
                    if (current_server_role == SERVER_TYPE_SS) {
                        if (param2_new_status == 0 || param2_new_status == 1) {
                            int sensor_found = 0;
                            for (int k = 0; k < MAX_CLIENTS; k++) {
                                if (connected_clients[k].socket_fd > 0 && 
                                    strcmp(connected_clients[k].id_cliente, param1_sensor_id) == 0) {
                                    
                                    connected_clients[k].status_risco = param2_new_status;
                                    sensor_found = 1;
                                    sprintf(log_msg, "Status de risco do sensor %s (Slot %d) alterado para %d.", 
                                            connected_clients[k].id_cliente, 
                                            connected_clients[k].assigned_slot, 
                                            param2_new_status);
                                    log_info(log_msg);
                                    break; 
                                }
                            }
                            if (!sensor_found) {
                                sprintf(log_msg, "Comando set_risk: Sensor com ID Global '%s' não encontrado ou não ativo.", param1_sensor_id);
                                log_info(log_msg);
                            }
                        } else {
                            log_info("Comando set_risk: Status inválido. Use 0 ou 1.");
                        }
                    } else {
                        log_info("Comando set_risk: Este comando só é válido para o Servidor de Status (SS).");
                    }
                } else {
                    sprintf(log_msg, "Comando desconhecido: '%s'", command_buffer);
                    log_info(log_msg);
                }
            } else {
                log_info("Entrada do teclado finalizada (EOF) ou erro de leitura.");
                break;
            }
        }

        // Verificar se há novas conexões de clientes
        if (FD_ISSET(client_master_socket_fd, &read_fds)) {
            struct sockaddr_in new_client_addr;                      // Definir struct para o novo cliente
            socklen_t new_client_addr_len = sizeof(new_client_addr); // Tamanho do endereço do novo cliente
            if ((new_socket_fd = accept(client_master_socket_fd, (struct sockaddr *)&new_client_addr, &new_client_addr_len)) < 0) {
                log_error("Falha ao aceitar nova conexão de cliente");
            } else {
                char client_ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &new_client_addr.sin_addr, client_ip_str, INET_ADDRSTRLEN);
                
                int added_to_slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) { // Tentar alocar o novo socket em um slot livre
                    if (client_sockets[i] == 0) {
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

                sprintf(log_msg, "Dados brutos recebidos do peer: [%s]", buffer);
                log_info(log_msg);
                
                int code;
                char payload[MAX_MSG_SIZE]; // Aumentar se MAX_PIDS_LENGTH for maior
                if (parse_message(buffer, &code, payload, sizeof(payload))) {
                    sprintf(log_msg, "Mensagem P2P recebida: Code=%d, Payload='%s'", code, payload);
                    log_info(log_msg);

                    char msg_to_send[MAX_MSG_SIZE];
                    char response_payload_str[10];

                    if (p2p_current_state == P2P_PASSIVE_LISTENING && code == REQ_CONNPEER) {
                        // Se o estado for P2P_PASSIVE_LISTENING, significa que S_j recebeu REQ_CONNPEER
                        // e está aguardando o ID do peer para enviar RES_CONNPEER
                        snprintf(my_pids_for_peer, sizeof(my_pids_for_peer), "Peer%d_Active", peer_socket_fd);
                        
                        sprintf(log_msg, "Peer %s, connected", my_pids_for_peer);
                        log_info(log_msg);
                        
                        // Enviar RES_CONNPEER com o ID atribuído
                        build_control_message(msg_to_send, sizeof(msg_to_send), RES_CONNPEER, my_pids_for_peer);
                        if (write(peer_socket_fd, msg_to_send, strlen(msg_to_send)) < 0) {
                            log_error("Falha ao enviar RES_CONNPEER");
                            close(peer_socket_fd);
                            peer_socket_fd = -1;
                            p2p_current_state = P2P_DISCONNECTED; 
                        } else {
                            log_info("RES_CONNPEER enviado.");
                            p2p_current_state = P2P_RES_SENT_AWAITING_RES; // Agora aguarda RES_CONNPEER
                        }

                    } else if (p2p_current_state == P2P_REQ_SENT && code == RES_CONNPEER) {
                        // Se o estado for P2P_REQ_SENT, significa que S_i enviou REQ_CONNPEER e está aguardando RES_CONNPEER
                        strncpy(peer_pids_for_me, payload, sizeof(peer_pids_for_me) - 1);
                        peer_pids_for_me[sizeof(peer_pids_for_me) - 1] = '\0'; // Garantir terminação nula
                        
                        sprintf(log_msg, "New Peer ID: %s", peer_pids_for_me); 
                        log_info(log_msg);
                        p2p_current_state = P2P_FULLY_ESTABLISHED;
                        sprintf(log_msg, "Conexão P2P com %s (ID: %s) totalmente estabelecida.", my_pids_for_peer, peer_pids_for_me);
                        log_info(log_msg);

                        // Enviar mensagem de confirmação de conexão
                        snprintf(my_pids_for_peer, sizeof(my_pids_for_peer), "Peer%d_Passive", peer_socket_fd);
                        sprintf(log_msg, "Peer %s, connected", my_pids_for_peer);
                        log_info(log_msg);

                        build_control_message(msg_to_send, sizeof(msg_to_send), RES_CONNPEER, my_pids_for_peer);
                        if (write(peer_socket_fd, msg_to_send, strlen(msg_to_send)) < 0) {
                            log_error("Falha ao enviar RES_CONNPEER");
                            close(peer_socket_fd);
                            peer_socket_fd = -1;
                            p2p_current_state = P2P_DISCONNECTED; 
                        } else {
                            log_info("RES_CONNPEER enviado.");
                            p2p_current_state = P2P_FULLY_ESTABLISHED;
                            sprintf(log_msg, "Conexão P2P com %s (ID: %s) totalmente estabelecida.", my_pids_for_peer, peer_pids_for_me);
                            log_info(log_msg);
                        }
                    } else if (p2p_current_state == P2P_RES_SENT_AWAITING_RES && code == RES_CONNPEER) { 
                        // Se o estado for P2P_RES_SENT_AWAITING_RES, significa que S_j recebeu REQ_CONNPEER e enviou RES_CONNPEER
                        strncpy(peer_pids_for_me, payload, sizeof(peer_pids_for_me) - 1);
                        peer_pids_for_me[sizeof(peer_pids_for_me) - 1] = '\0';

                        sprintf(log_msg, "New Peer ID: %s", peer_pids_for_me); 
                        log_info(log_msg);
                        p2p_current_state = P2P_FULLY_ESTABLISHED;
                        sprintf(log_msg, "Conexão P2P com %s (ID: %s) totalmente estabelecida.", my_pids_for_peer, peer_pids_for_me);
                        log_info(log_msg);
                    } else if (code == REQ_DISCPEER) {
                        // Se o estado for P2P_FULLY_ESTABLISHED, significa que S_i enviou REQ_DISCPEER
                        if (strcmp(payload, peer_pids_for_me) == 0) {
                            sprintf(log_msg, "REQ_DISCPEER recebido do peer %s (ID: %s). Confirmado.", my_pids_for_peer, peer_pids_for_me);
                            log_info(log_msg);

                            sprintf(response_payload_str, "%02d", OK_SUCCESSFUL_DISCONNECT);
                            build_control_message(msg_to_send, sizeof(msg_to_send), OK_MSG, response_payload_str);
                            if (write(peer_socket_fd, msg_to_send, strlen(msg_to_send)) < 0) {
                                log_error("Falha ao enviar OK(01) de desconexão para o peer.");
                            } else {
                                log_info("OK(01) de desconexão enviado para o peer.");
                            }
                            
                            sprintf(log_msg, "Peer %s disconnected.", my_pids_for_peer);
                            log_info(log_msg);
                            
                            // Fechar o socket P2P e reiniciar a escuta
                            close(peer_socket_fd);
                            peer_socket_fd = -1;
                            p2p_current_state = P2P_DISCONNECTED;
                            my_pids_for_peer[0] = '\0';
                            peer_pids_for_me[0] = '\0';

                            log_info("No peer found, starting to listen for P2P connections...");
                            
                            // Reiniciar o socket de escuta P2P
                            if (peer_listen_socket_fd <= 0) { 
                                if ((peer_listen_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) != -1) {
                                    int opt = 1;
                                    setsockopt(peer_listen_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                                    server_addr_peer_listen.sin_family = AF_INET;
                                    server_addr_peer_listen.sin_addr.s_addr = INADDR_ANY;
                                    server_addr_peer_listen.sin_port = htons(peer_target_port);
                                    if (bind(peer_listen_socket_fd, (struct sockaddr *)&server_addr_peer_listen, sizeof(server_addr_peer_listen)) == 0 &&
                                        listen(peer_listen_socket_fd, 1) == 0) {
                                        sprintf(log_msg, "Servidor ouvindo por nova conexão P2P na porta %d...", peer_target_port);
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
                    
                    else if (code == OK_MSG && atoi(payload) == OK_SUCCESSFUL_DISCONNECT) {
                        // Se o estado for P2P_DISCONNECT_REQ_SENT, significa que S_i recebeu OK(01) de desconexão
                        // do peer, confirmando a desconexão
                        log_info("OK(01) 'Successful disconnect' recebido do peer.");
                    
                        sprintf(log_msg, "Peer %s disconnected.", my_pids_for_peer);
                        log_info(log_msg);
                        
                        close(peer_socket_fd);
                        peer_socket_fd = -1;
                        p2p_current_state = P2P_DISCONNECTED;
                        my_pids_for_peer[0] = '\0';
                        peer_pids_for_me[0] = '\0';

                        log_info("Encerrando execução do servidor.");
                        close(client_master_socket_fd);
                        break; 
                    }
                    else if (code == ERROR_MSG && atoi(payload) == PEER_NOT_FOUND) {
                        log_info("ERROR(02) 'Peer not found' recebido do peer.");
                        close(peer_socket_fd);
                        peer_socket_fd = -1;
                        p2p_current_state = P2P_DISCONNECTED;
                    } else if (code == REQ_CHECKALERT && current_server_role == SERVER_TYPE_SL) {
                        // Mensagem REQ_CHECKALERT recebida do SS
                        // Payload é o ID_SENSOR enviado pelo SS
                            char global_sensor_id_from_ss[MAX_PIDS_LENGTH];
                            strncpy(global_sensor_id_from_ss, payload, sizeof(global_sensor_id_from_ss) - 1);
                            global_sensor_id_from_ss[sizeof(global_sensor_id_from_ss) - 1] = '\0';

                            sprintf(log_msg, "[SL] REQ_CHECKALERT %s", global_sensor_id_from_ss);
                            log_info(log_msg);

                            int loc_id_found = -1; // Usar -1 para indicar que não foi encontrado inicialmente
                            
                            // Procurar o sensor na base de dados do SL usando o ID_SENSOR
                            for (int k = 0; k < MAX_CLIENTS; k++) {
                                if (connected_clients[k].socket_fd > 0 && // Cliente está ativo neste slot
                                    strcmp(connected_clients[k].id_cliente, global_sensor_id_from_ss) == 0) {
                                    loc_id_found = connected_clients[k].loc_id;
                                    break;
                                }
                            }

                            char msg_to_ss_buffer[MAX_MSG_SIZE];
                            char response_payload_str[10]; // Para LocID ou código de erro

                            if (loc_id_found != -1 && loc_id_found != 0) { // Sensor encontrado e tem uma localização válida
                                sprintf(log_msg, "[SL] Found location of sensor %s: Location %d", 
                                        global_sensor_id_from_ss, loc_id_found);
                                log_info(log_msg);
                                
                                // Enviar RES_CHECKALERT(LocId) para o SS
                                sprintf(response_payload_str, "%d", loc_id_found);
                                sprintf(log_msg, "[SL] Sending RES_CHECKALERT %s to SS", response_payload_str);
                                log_info(log_msg);
                                build_control_message(msg_to_ss_buffer, sizeof(msg_to_ss_buffer), RES_CHECKALERT, response_payload_str);
                            } else {
                                // Sensor não encontrado ou loc_id inválida
                                sprintf(log_msg, "[SL] ERROR(10) - Sensor %s not found or location invalid", global_sensor_id_from_ss);
                                log_info(log_msg);

                                // Enviar ERROR_MSG(SENSOR_NOT_FOUND_ERROR) para o SS
                                sprintf(response_payload_str, "%02d", SENSOR_NOT_FOUND);
                                build_control_message(msg_to_ss_buffer, sizeof(msg_to_ss_buffer), ERROR_MSG, response_payload_str);
                            }
                            
                            if (write(peer_socket_fd, msg_to_ss_buffer, strlen(msg_to_ss_buffer)) < 0) {
                                sprintf(log_msg, "SL: Falha ao enviar resposta (%d) para REQ_CHECKALERT ao SS", 
                                        (loc_id_found != -1 && loc_id_found != 0) ? RES_CHECKALERT : ERROR_MSG);
                                log_error(log_msg);
                            }
                        } else {
                        sprintf(log_msg, "Mensagem P2P inesperada (Code=%d) ou estado P2P incorreto (%d).", code, p2p_current_state);
                        log_info(log_msg);
                    }
                } else {
                    log_error("Falha ao parsear mensagem P2P.");
                    close(peer_socket_fd);
                    peer_socket_fd = -1;
                    p2p_current_state = P2P_DISCONNECTED;
                }
            } else if (bytes_read == 0) { // Peer desconectou
                log_info("Peer desconectado.");
                close(peer_socket_fd);
                peer_socket_fd = -1;
                p2p_current_state = P2P_DISCONNECTED;
                // Limpar PidS
                my_pids_for_peer[0] = '\0';
                peer_pids_for_me[0] = '\0';
            } else { // Erro na leitura
                log_error("Erro ao ler do peer.");
                close(peer_socket_fd);
                peer_socket_fd = -1;
                p2p_current_state = P2P_DISCONNECTED;
            }
        }

        // Verificar se há atividade em algum socket de cliente conectado
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int current_client_socket = client_sockets[i]; // Obter o socket do cliente atual

            if (current_client_socket > 0 && FD_ISSET(current_client_socket, &read_fds)) {
                ssize_t bytes_read = read(current_client_socket, buffer, MAX_MSG_SIZE);

                if (bytes_read > 0) {
                    buffer[bytes_read] = '\0'; // Garantir terminação nula
                    int code;
                    char payload[MAX_MSG_SIZE];
                    
                    // Obter informações do cliente
                    struct sockaddr_in addr_cli_log;
                    socklen_t len_cli_log = sizeof(addr_cli_log);
                    getpeername(current_client_socket, (struct sockaddr*)&addr_cli_log, &len_cli_log);
                    sprintf(log_msg, "Dados recebidos do cliente %s:%d (socket %d)",
                            inet_ntoa(addr_cli_log.sin_addr), ntohs(addr_cli_log.sin_port), current_client_socket);
                    log_info(log_msg);
                    
                    // Verificar se a mensagem foi parseada corretamente
                    if (parse_message(buffer, &code, payload, sizeof(payload))) {
                        sprintf(log_msg, "Mensagem recebida: Code=%d, Payload='%s'", code, payload);
                        log_info(log_msg);
                        // Lógica para processar a mensagem recebida
                        if (code == REQ_CONNSEN) { // Registro de Sensor
                            char global_sensor_id_str[MAX_PIDS_LENGTH]; // Para o ID Global de 10 chars
                            char loc_id_str_from_payload[10];           // Para a parte da string da LocId
                            int client_loc_id_from_payload;             // Variável para armazenar o LocId do payload
                            int proceed_with_registration = 0;          // Flag para controlar o fluxo

                            // Parsear o payload "ID_GLOBAL_SENSOR,LocId"
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
                                        
                                        // Validação do ID Global
                                        if (strlen(global_sensor_id_str) == 10) {
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
                                if (write(current_client_socket, msg_error, strlen(msg_error)) < 0) {
                                    log_error("Falha ao enviar mensagem de erro para o cliente.");
                                }
                                close(current_client_socket);       // Fechar conexão com o cliente inválido
                                client_sockets[i] = 0;              // Limpar slot do cliente
                                connected_clients[i].socket_fd = 0; // Limpar socket_fd do cliente
                                continue;                           // Pula para o próximo cliente
                            }

                            // Verificar se o socket realmente corresponde
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

                            // Lógica de Registro
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
                                        if (write(current_client_socket, msg_error, strlen(msg_error)) < 0) {
                                            log_error("Falha ao enviar mensagem de erro para o cliente.");
                                        }
                                        // Fechar a conexão com o cliente atual, pois não pode ser registrado
                                        sprintf(log_msg, "Fechando conexão com socket %d (slot %d) devido a ID Global já em uso.", 
                                                current_client_socket, i + 1);
                                        log_info(log_msg);
                                        close(current_client_socket); client_sockets[i]=0; connected_clients[i].socket_fd=0;
                                        break; 
                                    }
                                }

                                if (id_already_in_use) {
                                    // Se o ID já está em uso por outro socket, não prosseguir com este.
                                    close(current_client_socket);
                                    client_sockets[i] = 0;
                                    connected_clients[i].socket_fd = 0; 
                                    continue; // Pula para o próximo cliente
                                }

                                // Verificar limite de clientes *registrados*
                                if (num_connected_clients >= MAX_CLIENTS) {
                                    log_info("Limite de sensores excedido. Enviando ERROR(09)...");
                                    char error_payload_str[10];
                                    sprintf(error_payload_str, "%02d", SENSOR_LIMIT_EXCEEDED);
                                    char msg_error[MAX_MSG_SIZE];
                                    build_control_message(msg_error, sizeof(msg_error), ERROR_MSG, error_payload_str);
                                    if (write(current_client_socket, msg_error, strlen(msg_error)) < 0) {
                                        log_error("Falha ao enviar mensagem de erro para o cliente.");
                                    }
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

                                    // Enviar RES_CONNSEN com o ID do SLOT como payload
                                    char id_slot_str[10];
                                    sprintf(id_slot_str, "%d", connected_clients[i].assigned_slot);
                                    
                                    char msg_res_connsen[MAX_MSG_SIZE];
                                    build_control_message(msg_res_connsen, sizeof(msg_res_connsen), RES_CONNSEN, id_slot_str);
                                    if (write(current_client_socket, msg_res_connsen, strlen(msg_res_connsen)) < 0) {
                                        log_error("Falha ao enviar RES_CONNSEN para o cliente.");
                                    }
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
                                    if (write(current_client_socket, msg_res_connsen, strlen(msg_res_connsen)) < 0) {
                                        log_error("Falha ao reenviar RES_CONNSEN para o cliente.");
                                    } else {
                                        log_info("RES_CONNSEN reenviado ao cliente.");
                                    }
                                } else {
                                    // Este socket (current_client_socket, que é connected_clients[i].socket_fd)
                                    // já está associado ao ID connected_clients[i].id_cliente.
                                    sprintf(log_msg, "Socket %d (Slot %d, Cliente %s) recebeu REQ_CONNSEN com um ID_Global diferente: '%s'. Rejeitando.",
                                            current_client_socket, i + 1, connected_clients[i].id_cliente, global_sensor_id_str);
                                    log_error(log_msg);
                                }
                            }
                        } else if (code == REQ_DISCSEN) { // Cliente solicitando desconexão
                            char slot_id_str_from_payload[10];
                            strncpy(slot_id_str_from_payload, payload, sizeof(slot_id_str_from_payload) - 1);
                            slot_id_str_from_payload[sizeof(slot_id_str_from_payload) - 1] = '\0';
                            int received_slot_id = atoi(slot_id_str_from_payload);
                            char msg_response_to_client[MAX_MSG_SIZE];
                            char response_code_payload_str[10]; 

                            // Verificar se o socket corresponde ao cliente que está tentando desconectar
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
                                connected_clients[i].socket_fd = 0;        // Marcar o socket como 0 em ClientInfo
                                connected_clients[i].id_cliente[0] = '\0'; // Limpar ID Global
                                connected_clients[i].assigned_slot = 0;    // Resetar o slot ID atribuído
                                connected_clients[i].loc_id = -1;          // Resetar LocId
                                connected_clients[i].status_risco = 0; // Resetar status

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
                            }
                        } else if (code == REQ_SENSSTATUS && current_server_role == SERVER_TYPE_SS) { // Solicitação de status do sensor
                            char slot_id_str_from_client[10];
                            strncpy(slot_id_str_from_client, payload, sizeof(slot_id_str_from_client) - 1);
                            slot_id_str_from_client[sizeof(slot_id_str_from_client) - 1] = '\0';
                            int received_slot_id = atoi(slot_id_str_from_client);

                            if (connected_clients[i].socket_fd == current_client_socket &&
                                connected_clients[i].assigned_slot == received_slot_id &&
                                connected_clients[i].id_cliente[0] != '\0') {
                                
                                sprintf(log_msg, "REQ_SENSSTATUS %s (Slot: %d)",
                                    connected_clients[i].id_cliente,
                                    connected_clients[i].assigned_slot);
                                log_info(log_msg);

                                // Verificar o status_risco do sensor na base de dados do SS
                                if (connected_clients[i].status_risco == 1) { // Falha detectada
                                    sprintf(log_msg, "Sensor %s (Slot: %d) status = 1 (failure detected)", 
                                        connected_clients[i].id_cliente,
                                        connected_clients[i].assigned_slot);
                                    log_info(log_msg);

                                    // Enviar REQ_CHECKALERT para o SL para verificar o alerta
                                    if (peer_socket_fd > 0 && p2p_current_state == P2P_FULLY_ESTABLISHED) {
                                        sprintf(log_msg, "Sending REQ_CHECKALERT %d to SL", connected_clients[i].assigned_slot);
                                        log_info(log_msg);
                                        
                                        char msg_to_sl[MAX_MSG_SIZE];
                                        build_control_message(msg_to_sl, sizeof(msg_to_sl), REQ_CHECKALERT, connected_clients[i].id_cliente);
                                        
                                        if (write(peer_socket_fd, msg_to_sl, strlen(msg_to_sl)) < 0) {
                                            log_error("SS: Falha ao enviar REQ_CHECKALERT para SL");
                                        } else {
                                            log_info("SS: Aguardando resposta do SL para REQ_CHECKALERT...");
                                            char sl_response_buffer[MAX_MSG_SIZE];
                                            ssize_t sl_bytes = read(peer_socket_fd, sl_response_buffer, sizeof(sl_response_buffer)-1);
                                            if (sl_bytes > 0) {
                                                sl_response_buffer[sl_bytes] = '\0';
                                                int sl_code; char sl_payload[MAX_MSG_SIZE];
                                                if(parse_message(sl_response_buffer, &sl_code, sl_payload, sizeof(sl_payload))){
                                                    char msg_to_client[MAX_MSG_SIZE];
                                                    if(sl_code == RES_CHECKALERT){ // SL respondeu com LocID
                                                        sprintf(log_msg, "SS: RES_CHECKALERT %s recebido do SL", sl_payload);
                                                        log_info(log_msg);                                                       
                                                        sprintf(log_msg, "SS: Sending RES_SENSSTATUS %s to CLIENT", sl_payload);
                                                        log_info(log_msg);
                                                        build_control_message(msg_to_client, sizeof(msg_to_client), RES_SENSSTATUS, sl_payload); // Payload é o LocID do sensor
                                                        if (write(current_client_socket, msg_to_client, strlen(msg_to_client)) < 0) {log_error("SS: Falha ao enviar RES_SENSSTATUS para cliente.");}
                                                    } else if (sl_code == ERROR_MSG && atoi(sl_payload) == SENSOR_NOT_FOUND) { // SL não encontrou o sensor
                                                        log_info("SS: ERROR(10) SENSOR_NOT_FOUND received from SL");
                                                        sprintf(log_msg, "SS: Sending ERROR(10) to CLIENT");
                                                        log_info(log_msg);
                                                        build_control_message(msg_to_client, sizeof(msg_to_client), ERROR_MSG, sl_payload);
                                                        if (write(current_client_socket, msg_to_client, strlen(msg_to_client)) < 0) {log_error("SS: Falha ao enviar ERROR(10) para cliente.");}
                                                    } else {
                                                        sprintf(log_msg, "SS: Resposta inesperada do SL para REQ_CHECKALERT: Code=%d, Payload='%s'", sl_code, sl_payload);
                                                        log_error(log_msg);
                                                    }
                                                } else { log_error("SS: Falha ao parsear resposta do SL para REQ_CHECKALERT");}
                                            } else if (sl_bytes == 0) {
                                                log_error("SS: SL desconectou antes de responder ao REQ_CHECKALERT.");
                                            } else {
                                                log_error("SS: Erro ao ler resposta do SL para REQ_CHECKALERT.");
                                            }
                                        }
                                    } else {
                                        log_error("SS: Sem conexão P2P com SL ou P2P não estabelecida para enviar REQ_CHECKALERT.");
                                    }
                                } else { // Status do sensor é 0 (normal)
                                    sprintf(log_msg, "Sensor %s status = 0 (normal). Nenhuma ação de alerta.", connected_clients[i].id_cliente);
                                    log_info(log_msg);
                                    
                                    char msg_to_client[MAX_MSG_SIZE];
                                    char loc_id_normal_payload[] = "-1"; // "-1" indica status normal ao cliente
                                    build_control_message(msg_to_client, sizeof(msg_to_client), RES_SENSSTATUS, loc_id_normal_payload);
                                    if(write(current_client_socket, msg_to_client, strlen(msg_to_client)) < 0){
                                        log_error("Falha ao enviar RES_SENSSTATUS (normal) para cliente.");
                                    } else {
                                        log_info("RES_SENSSTATUS (normal, LocID=0) enviado ao cliente.");
                                    }
                                }
                            } else {
                                // O ID de slot recebido não corresponde ao esperado para este socket,
                                // ou o cliente não está totalmente registrado (sem sensor_id).
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
                        else if (code == REQ_SENSLOC && current_server_role == SERVER_TYPE_SL) { // Solicitação de localização do sensor
                            // Payload é o ID_SENSOR do sensor a ser localizado
                            char target_global_sensor_id[MAX_PIDS_LENGTH];
                            strncpy(target_global_sensor_id, payload, sizeof(target_global_sensor_id) - 1);
                            target_global_sensor_id[sizeof(target_global_sensor_id) - 1] = '\0';

                            sprintf(log_msg, "[SL] REQ_SENSLOC %s", target_global_sensor_id);
                            log_info(log_msg);

                            int loc_id_found = -1; // Usar -1 para indicar que não foi encontrado

                            // Procurar o sensor na base de dados do SL usando o ID_SENSOR
                            for (int k = 0; k < MAX_CLIENTS; k++) {
                                if (connected_clients[k].socket_fd > 0 && // Cliente está ativo neste slot
                                    strcmp(connected_clients[k].id_cliente, target_global_sensor_id) == 0) {
                                    loc_id_found = connected_clients[k].loc_id;
                                    break;
                                }
                            }

                            char msg_to_client_buffer[MAX_MSG_SIZE];
                            char response_payload_str[10]; 

                            if (loc_id_found != -1) { // Sensor encontrado
                                // Enviar RES_SENSLOC(LocId) para o cliente requisitante
                                sprintf(response_payload_str, "%d", loc_id_found);
                                build_control_message(msg_to_client_buffer, sizeof(msg_to_client_buffer), RES_SENSLOC, response_payload_str);
                                sprintf(log_msg, "[SL] Enviando RES_SENSLOC %s para cliente (socket %d)", response_payload_str, current_client_socket);
                                log_info(log_msg);
                            } else {
                                // Sensor não encontrado
                                // Enviar ERROR_MSG(SENSOR_NOT_FOUND_ERROR) para o cliente requisitante
                                sprintf(log_msg, "[SL] Sensor %s não encontrado. Enviando ERROR(10) para cliente (socket %d).", target_global_sensor_id, current_client_socket);
                                log_info(log_msg);
                                sprintf(response_payload_str, "%02d", SENSOR_NOT_FOUND);
                                build_control_message(msg_to_client_buffer, sizeof(msg_to_client_buffer), ERROR_MSG, response_payload_str);
                            }
                            
                            if (write(current_client_socket, msg_to_client_buffer, strlen(msg_to_client_buffer)) < 0) {
                                sprintf(log_msg, "SL: Falha ao enviar resposta (%d) para REQ_SENSLOC ao cliente (socket %d)", 
                                        (loc_id_found != -1) ? RES_SENSLOC : ERROR_MSG, current_client_socket);
                                log_error(log_msg);
                            }
                        } else if (code == REQ_LOCLIST && current_server_role == SERVER_TYPE_SL) { // Solicitação de lista de sensores em uma localização específica
                            // Payload esperado: "SlotID_Cliente_Requisitante,LocId_Alvo"
                            char slot_id_requisitante_str[10];
                            char target_loc_id_str[10];
                            int target_loc_id_int;

                            // Parsear o payload composto
                            char *comma_ptr = strchr(payload, ',');
                            int parsed_correctly = 0;
                            if (comma_ptr != NULL) {
                                size_t slot_id_len = comma_ptr - payload;
                                if (slot_id_len < sizeof(slot_id_requisitante_str) && slot_id_len > 0) {
                                    strncpy(slot_id_requisitante_str, payload, slot_id_len);
                                    slot_id_requisitante_str[slot_id_len] = '\0';

                                    strncpy(target_loc_id_str, comma_ptr + 1, sizeof(target_loc_id_str) - 1);
                                    target_loc_id_str[sizeof(target_loc_id_str) - 1] = '\0';
                                    
                                    if (strlen(target_loc_id_str) > 0) {
                                        target_loc_id_int = atoi(target_loc_id_str);
                                        parsed_correctly = 1;
                                    }
                                }
                            }

                            if (!parsed_correctly) {
                                log_error("[SL] REQ_LOCLIST: Formato de payload inválido. Esperado 'SlotID_Cliente,LocId_Alvo'.");
                                continue;
                            }
                            
                            sprintf(log_msg, "[SL] REQ_LOCLIST (SlotCliente: %s, LocAlvo: %d)", 
                                    slot_id_requisitante_str, target_loc_id_int);
                            log_info(log_msg);

                            // Validar LocId_Alvo (1-10)
                            if (target_loc_id_int < 1 || target_loc_id_int > 10) {
                                sprintf(log_msg, "[SL] REQ_LOCLIST: LocId %d inválida. Enviando ERROR(10).", target_loc_id_int);
                                log_info(log_msg);
                                char error_payload[10];
                                sprintf(error_payload, "%02d", SENSOR_NOT_FOUND);
                                build_control_message(buffer, sizeof(buffer), ERROR_MSG, error_payload);
                                if (write(current_client_socket, buffer, strlen(buffer)) < 0) {
                                    log_error("SL: Falha ao enviar ERROR(10) para REQ_LOCLIST ao cliente.");
                                }
                                continue;
                            }
                            
                            char list_of_sensor_global_ids[MAX_MSG_SIZE] = "";
                            int count_found_at_loc = 0;
                            size_t current_len = 0;

                            // Buscar todos os sensores registrados naquela LocId_Alvo
                            for (int k = 0; k < MAX_CLIENTS; k++) {
                                if (connected_clients[k].socket_fd > 0 &&         // Cliente está ativo
                                    connected_clients[k].id_cliente[0] != '\0' && // Tem um ID Global registrado
                                    connected_clients[k].loc_id == target_loc_id_int) {
                                    
                                    if (count_found_at_loc > 0) {
                                        // Adicionar vírgula antes do próximo ID, se não for o primeiro
                                        if (current_len < sizeof(list_of_sensor_global_ids) - 1) {
                                            list_of_sensor_global_ids[current_len++] = ',';
                                        } else break; // Buffer cheio
                                    }
                                    // Concatenar o ID Global do sensor encontrado
                                    size_t id_global_len = strlen(connected_clients[k].id_cliente);
                                    if (current_len + id_global_len < sizeof(list_of_sensor_global_ids) -1) {
                                        strcat(list_of_sensor_global_ids, connected_clients[k].id_cliente);
                                        current_len += id_global_len;
                                        count_found_at_loc++;
                                    } else break; // Buffer cheio
                                }
                            }

                            char msg_to_client_buffer[MAX_MSG_SIZE];
                            
                            if (count_found_at_loc > 0) { // Sensores encontrados na localização
                                sprintf(log_msg, "[SL] Found sensors at location %d. Sending RES_LOCLIST: %s", 
                                        target_loc_id_int, list_of_sensor_global_ids);
                                log_info(log_msg);
                                build_control_message(msg_to_client_buffer, sizeof(msg_to_client_buffer), RES_LOCLIST, list_of_sensor_global_ids);
                            } else { 
                                sprintf(log_msg, "[SL] No sensors found for LocId %d or location considered not found for listing. Sending ERROR(10).", target_loc_id_int);
                                log_info(log_msg);
                                
                                char error_payload[10];
                                sprintf(error_payload, "%02d", SENSOR_NOT_FOUND); // Código 10
                                build_control_message(msg_to_client_buffer, sizeof(msg_to_client_buffer), ERROR_MSG, error_payload);
                            }
                            
                            if (write(current_client_socket, msg_to_client_buffer, strlen(msg_to_client_buffer)) < 0) {
                                log_error("SL: Falha ao enviar resposta para REQ_LOCLIST ao cliente.");
                            }
                        }
                        else {
                            sprintf(log_msg, "Código de mensagem de cliente desconhecido ou não esperado: %d", code);
                            log_info(log_msg);
                        }

                    } else {
                        log_error("Falha ao parsear mensagem do cliente.");
                    }
                } else if (bytes_read == 0) {
                    // Cliente desconectou (EOF)
                    sprintf(log_msg, "Cliente (socket %d) desconectou.", current_client_socket);
                    log_info(log_msg);
                    close(current_client_socket);
                    client_sockets[i] = 0;
                    if (connected_clients[i].socket_fd != 0) { 
                        connected_clients[i].socket_fd = 0; 
                        connected_clients[i].id_cliente[0] = '\0';
                        if (num_connected_clients > 0) num_connected_clients--;
                    }
                } else { // Erro na leitura (bytes_read < 0)
                    log_error("Erro ao ler do cliente.");
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
        connected_clients[i].socket_fd = 0;
        connected_clients[i].id_cliente[0] = '\0';
        connected_clients[i].loc_id = -1;
        connected_clients[i].status_risco = -1;
        connected_clients[i].assigned_slot = 0;
    }

    log_info("Servidor encerrado.");
    return 0;
}
