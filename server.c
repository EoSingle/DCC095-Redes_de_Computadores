// server.c
#include "common.h"
#include <sys/select.h> // Para select() e fd_set
#include <errno.h>      // Para errno

#define MAX_CLIENTS 15 // Conforme especificado, cada servidor trata até 15 equipamentos

int peer_socket_fd = -1;

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
                sprintf(log_msg, "Conectado ao peer %s:%d no socket P2P %d.", peer_target_ip, peer_target_port, peer_socket_fd);
                log_info(log_msg);
                // Aqui enviaria REQ_CONNPEER e esperaria RES_CONNPEER
            }
        }
    }

    log_info("Aguardando conexões (clientes/P2P) ou entrada do teclado...");

    while (1) { // Loop principal do servidor
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
            int current_client_socket = client_sockets[i];
            if (current_client_socket > 0) {
                FD_SET(current_client_socket, &read_fds);
            }
            if (current_client_socket > max_sd) {
                max_sd = current_client_socket;
            }
        }


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
                    log_info("Comando 'kill' recebido. Iniciando procedimento para desconectar do peer...");
                    // Aqui virá a lógica para enviar REQ_DISCPEER ao peer.
                    // Por enquanto, apenas logamos.
                    // Se não houver peer conectado: "No peer connected to close connection" [cite: 115]
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
            if ((new_socket_fd = accept(client_master_socket_fd, (struct sockaddr *)&server_addr_clients, &client_addr_len)) < 0) {
                log_error("Falha ao aceitar nova conexão de cliente");
            } else {
                // Encontrar um slot livre para o novo cliente
                int i;
                for (i = 0; i < MAX_CLIENTS; i++) {
                    if (client_sockets[i] == 0) { // Slot livre encontrado
                        client_sockets[i] = new_socket_fd;
                        break;
                    }
                }
                if (i == MAX_CLIENTS) {
                    log_info("Máximo de clientes atingido, rejeitando nova conexão.");
                    close(new_socket_fd); // Fecha a conexão se não houver espaço
                } else {
                    sprintf(log_msg, "Novo cliente conectado no socket %d.", new_socket_fd);
                    log_info(log_msg);
                }
            }
         }
        

        // Aceitar nova conexão P2P (se estiver escutando e ainda não conectado)
        if (peer_listen_socket_fd > 0 && FD_ISSET(peer_listen_socket_fd, &read_fds)) {
            if (peer_socket_fd <= 0) { // Só aceita se não tiver um peer já conectado ativamente
                struct sockaddr_in incoming_peer_addr;
                socklen_t incoming_peer_addr_len = sizeof(incoming_peer_addr);
                int new_peer_conn_fd = accept(peer_listen_socket_fd, (struct sockaddr *)&incoming_peer_addr, &incoming_peer_addr_len);
                if (new_peer_conn_fd < 0) {
                    log_error("Falha ao aceitar conexão P2P passiva");
                } else {
                    char peer_ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &incoming_peer_addr.sin_addr, peer_ip_str, INET_ADDRSTRLEN);
                    sprintf(log_msg, "Conexão P2P passiva aceita de %s:%d no socket %d.",
                            peer_ip_str, ntohs(incoming_peer_addr.sin_port), new_peer_conn_fd);
                    log_info(log_msg);
                    peer_socket_fd = new_peer_conn_fd; // Agora temos uma conexão P2P
                    // Fechar o socket de escuta P2P, pois só queremos 1 peer
                    close(peer_listen_socket_fd);
                    peer_listen_socket_fd = -1; 
                    // Aqui S_j (este servidor) receberia REQ_CONNPEER e responderia com RES_CONNPEER
                }
            } else {
                // Já tem um peer, talvez rejeitar ou fechar a nova conexão P2P indesejada
                log_info("Já possui uma conexão P2P, ignorando nova tentativa de P2P passiva.");
                // Poderia fechar a nova conexão aceita aqui se não a quisesse.
                // int temp_fd = accept(peer_listen_socket_fd, NULL, NULL); close(temp_fd);
            }
        }

        // Ler dados do peer conectado
        if (peer_socket_fd > 0 && FD_ISSET(peer_socket_fd, &read_fds)) {
            // Lógica para ler mensagens do protocolo P2P (REQ_CONNPEER, RES_CONNPEER, REQ_DISCPEER, etc.)
            ssize_t bytes_read = read(peer_socket_fd, buffer, MAX_MSG_SIZE);
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                sprintf(log_msg, "Mensagem P2P recebida: %s", buffer); // Placeholder
                log_info(log_msg);
                // Processar mensagem P2P aqui
            } else if (bytes_read == 0) {
                log_info("Peer desconectado.");
                close(peer_socket_fd);
                peer_socket_fd = -1;
                // Aqui, o servidor S_j (se este for S_j) deveria voltar a escutar por um novo peer. [cite: 66]
                // E S_i (se este for S_i) terminaria. [cite: 65]
                // A lógica exata de quem faz o quê ao desconectar P2P precisa ser implementada
                // conforme o protocolo de fechamento.
            } else {
                log_error("Erro ao ler do peer.");
                close(peer_socket_fd);
                peer_socket_fd = -1;
            }
        }

        // Ler dados dos clientes conectados
        for (int i = 0; i < MAX_CLIENTS; i++) { 
            int current_client_socket = client_sockets[i];
            if (current_client_socket > 0 && FD_ISSET(current_client_socket, &read_fds)) {
                ssize_t bytes_read = read(current_client_socket, buffer, MAX_MSG_SIZE);
                if (bytes_read > 0) {
                    buffer[bytes_read] = '\0'; // Garantir terminação nula
                    sprintf(log_msg, "Mensagem recebida do cliente %d: %s", current_client_socket, buffer);
                    log_info(log_msg);
                    // Aqui poderia processar a mensagem do cliente
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

    } // Fim do while(1)

    // --- LIMPEZA FINAL ---
    log_info("Encerrando e limpando sockets...");
    close(client_master_socket_fd);
    if (peer_socket_fd > 0) close(peer_socket_fd);
    if (peer_listen_socket_fd > 0) close(peer_listen_socket_fd);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_sockets[i] > 0) close(client_sockets[i]);
    }
    log_info("Servidor encerrado.");
    return 0;
}