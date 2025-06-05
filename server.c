#include "common.h"
#include <sys/select.h>
#include <errno.h>

#define MAX_CLIENTS 15  // Maximum number of clients per server

int peer_socket_fd = -1; // Active P2P connection socket

// P2P connection state
typedef enum {
    P2P_DISCONNECTED,
    P2P_ACTIVE_CONNECTING,
    P2P_PASSIVE_LISTENING,
    P2P_REQ_SENT,
    P2P_RES_SENT_AWAITING_RES,
    P2P_FULLY_ESTABLISHED,
    P2P_DISCONNECT_REQ_SENT
} P2PState;

P2PState p2p_current_state = P2P_DISCONNECTED;
char my_pids_for_peer[MAX_PIDS_LENGTH] = "";  // ID assigned by this server to the peer
char peer_pids_for_me[MAX_PIDS_LENGTH] = "";  // ID assigned by peer to this server

// Information about connected clients
typedef struct {
    int socket_fd;
    char client_id[MAX_PIDS_LENGTH];  // 10-digit sensor ID
    int assigned_slot;                // Slot (1 to 15)
    int location_id;                  // Location ID (used by SL)
    int risk_status;                  // Risk status (used by SS)
} ClientInfo;

ClientInfo connected_clients[MAX_CLIENTS];
int num_connected_clients = 0;

// Server roles
typedef enum {
    SERVER_TYPE_UNINITIALIZED,
    SERVER_TYPE_STATUS,
    SERVER_TYPE_LOCATION
} ServerRole;

ServerRole current_server_role = SERVER_TYPE_UNINITIALIZED;

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <peer_ip> <p2p_port> <client_listen_port> <SS|SL>\n", argv[0]);
        fprintf(stderr, "Example: %s 127.0.0.1 60000 61000 SS\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Argument parsing
    char *peer_ip = argv[1];
    int peer_port = atoi(argv[2]);
    int client_listen_port = atoi(argv[3]);
    char *role_arg = argv[4];

    // Role setup
    if (strcmp(role_arg, "SS") == 0) {
        current_server_role = SERVER_TYPE_STATUS;
        log_info("Server configured as STATUS SERVER (SS).");
    } else if (strcmp(role_arg, "SL") == 0) {
        current_server_role = SERVER_TYPE_LOCATION;
        log_info("Server configured as LOCATION SERVER (SL).");
    } else {
        fprintf(stderr, "Error: Invalid server type '%s'. Use 'SS' or 'SL'.\n", role_arg);
        exit(EXIT_FAILURE);
    }

        int client_master_fd, new_client_fd;
    int peer_listen_fd = -1;

    struct sockaddr_in addr_clients, addr_peer_listen, addr_peer_target;
    socklen_t client_addr_len = sizeof(struct sockaddr_in);

    char buffer[MAX_MSG_SIZE + 1];
    char log_msg[150];
    int client_fds[MAX_CLIENTS];

    // Initialize client structures
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_fds[i] = 0;
        connected_clients[i].socket_fd = 0;
        connected_clients[i].client_id[0] = '\0';
        connected_clients[i].assigned_slot = 0;
        connected_clients[i].location_id = 0;
        connected_clients[i].risk_status = -1;
    }

    fd_set read_fds;
    int max_fd;

    // --- CLIENT SOCKET SETUP ---
    if ((client_master_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        log_error("Failed to create client master socket.");
        exit(EXIT_FAILURE);
    }
    log_info("Client master socket created.");

    int opt = 1;
    if (setsockopt(client_master_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_error("Failed to set socket options for client master socket.");
        close(client_master_fd);
        exit(EXIT_FAILURE);
    }

    addr_clients.sin_family = AF_INET;
    addr_clients.sin_addr.s_addr = INADDR_ANY;
    addr_clients.sin_port = htons(client_listen_port);

    if (bind(client_master_fd, (struct sockaddr *)&addr_clients, sizeof(addr_clients)) < 0) {
        close(client_master_fd);
        exit(EXIT_FAILURE);
    }

    sprintf(log_msg, "Client master socket bound to port %d.", client_listen_port);
    log_info(log_msg);

    if (listen(client_master_fd, SERVER_BACKLOG) < 0) {
        close(client_master_fd);
        exit(EXIT_FAILURE);
    }

    sprintf(log_msg, "Server listening for clients on port %d...", client_listen_port);
    log_info(log_msg);

    // --- ACTIVE P2P CONNECTION ATTEMPT ---
    log_info("Attempting active connection to peer...");
    if ((peer_socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        log_error("Failed to create socket for active P2P connection.");
    } else {
        addr_peer_target.sin_family = AF_INET;
        addr_peer_target.sin_port = htons(peer_port);

        if (inet_pton(AF_INET, peer_ip, &addr_peer_target.sin_addr) <= 0) {
            log_error("Invalid peer IP address for P2P connection.");
            close(peer_socket_fd);
            peer_socket_fd = -1;
        } else {
            if (connect(peer_socket_fd, (struct sockaddr *)&addr_peer_target, sizeof(addr_peer_target)) < 0) {
                sprintf(log_msg, "Failed to connect to peer %s:%d. %s.", peer_ip, peer_port, strerror(errno));
                log_info(log_msg);
                close(peer_socket_fd);
                peer_socket_fd = -1;

                // Fallback to passive P2P listening
                log_info("No peer found, starting passive P2P listener...");
                if ((peer_listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
                    log_error("Failed to create socket for passive P2P.");
                    peer_listen_fd = -1;
                } else {
                    if (setsockopt(peer_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
                        log_error("Failed to set socket options for passive P2P.");
                        close(peer_listen_fd);
                        peer_listen_fd = -1;
                        return -1;
                    }

                    addr_peer_listen.sin_family = AF_INET;
                    addr_peer_listen.sin_addr.s_addr = INADDR_ANY;
                    addr_peer_listen.sin_port = htons(peer_port);

                    if (bind(peer_listen_fd, (struct sockaddr *)&addr_peer_listen, sizeof(addr_peer_listen)) < 0) {
                        close(peer_listen_fd);
                        peer_listen_fd = -1;
                    } else if (listen(peer_listen_fd, 1) < 0) {
                        close(peer_listen_fd);
                        peer_listen_fd = -1;
                    } else {
                        sprintf(log_msg, "Server listening for P2P connections on port %d...", peer_port);
                        log_info(log_msg);
                    }
                }
            } else {
                sprintf(log_msg, "Connected to peer %s:%d on P2P socket %d. Sending REQ_CONNPEER...",
                        peer_ip, peer_port, peer_socket_fd);
                log_info(log_msg);
                p2p_current_state = P2P_ACTIVE_CONNECTING;

                char msg_buf[MAX_MSG_SIZE];
                build_control_message(msg_buf, sizeof(msg_buf), REQ_CONNPEER, NULL);

                if (write(peer_socket_fd, msg_buf, strlen(msg_buf)) < 0) {
                    log_error("Failed to send REQ_CONNPEER.");
                    close(peer_socket_fd);
                    peer_socket_fd = -1;
                    p2p_current_state = P2P_DISCONNECTED;
                } else {
                    log_info("REQ_CONNPEER sent.");
                    p2p_current_state = P2P_REQ_SENT;
                }
            }
        }
    }

    log_info("Waiting for client/P2P connections or keyboard input...");

        printf("Available commands:\n");
    printf("  kill                      - Sends REQ_DISCPEER to the peer if connected.\n");
    printf("  exit                      - Terminates the server.\n");
    printf("  set_risk <SensorID> <0|1> - Updates risk status of a sensor (only for SS).\n");

    while (1) {
        FD_ZERO(&read_fds);

        FD_SET(STDIN_FILENO, &read_fds);
        max_fd = STDIN_FILENO;

        FD_SET(client_master_fd, &read_fds);
        if (client_master_fd > max_fd) max_fd = client_master_fd;

        if (peer_listen_fd > 0) {
            FD_SET(peer_listen_fd, &read_fds);
            if (peer_listen_fd > max_fd) max_fd = peer_listen_fd;
        }

        if (peer_socket_fd > 0) {
            FD_SET(peer_socket_fd, &read_fds);
            if (peer_socket_fd > max_fd) max_fd = peer_socket_fd;
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] > 0) {
                FD_SET(client_fds[i], &read_fds);
                if (client_fds[i] > max_fd) max_fd = client_fds[i];
            }
        }

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if ((activity < 0) && (errno != EINTR)) {
            log_error("select() error.");
            continue;
        }

        // --- STDIN (keyboard input) ---
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char cmd_buf[MAX_MSG_SIZE];
            if (fgets(cmd_buf, sizeof(cmd_buf), stdin) != NULL) {
                cmd_buf[strcspn(cmd_buf, "\n")] = 0;

                char command[20];
                char sensor_id[MAX_PIDS_LENGTH];
                int new_status;

                sprintf(log_msg, "Keyboard command received: '%s'", cmd_buf);
                log_info(log_msg);

                if (strcmp(cmd_buf, "kill") == 0) {
                    if (p2p_current_state == P2P_FULLY_ESTABLISHED && peer_socket_fd > 0) {
                        sprintf(log_msg, "'kill' command received. Sending REQ_DISCPEER to peer %s...", my_pids_for_peer);
                        log_info(log_msg);

                        char disconnect_msg[MAX_MSG_SIZE];
                        build_control_message(disconnect_msg, sizeof(disconnect_msg), REQ_DISCPEER, my_pids_for_peer);

                        if (write(peer_socket_fd, disconnect_msg, strlen(disconnect_msg)) < 0) {
                            log_error("Failed to send REQ_DISCPEER.");
                            close(peer_socket_fd);
                            peer_socket_fd = -1;
                            p2p_current_state = P2P_DISCONNECTED;
                        } else {
                            log_info("REQ_DISCPEER sent.");
                            p2p_current_state = P2P_DISCONNECT_REQ_SENT;
                        }
                    } else {
                        log_info("No active P2P connection to disconnect.");
                    }
                } else if (strcmp(cmd_buf, "exit") == 0) {
                    log_info("'exit' command received. Shutting down server...");
                    break;
                } else if (sscanf(cmd_buf, "%19s %49s %d", command, sensor_id, &new_status) == 3 &&
                           strcmp(command, "set_risk") == 0) {
                    if (current_server_role == SERVER_TYPE_STATUS) {
                        if (new_status == 0 || new_status == 1) {
                            int found = 0;
                            for (int i = 0; i < MAX_CLIENTS; i++) {
                                if (connected_clients[i].socket_fd > 0 &&
                                    strcmp(connected_clients[i].client_id, sensor_id) == 0) {
                                    connected_clients[i].risk_status = new_status;
                                    found = 1;
                                    sprintf(log_msg, "Risk status of sensor %s (Slot %d) updated to %d.",
                                            connected_clients[i].client_id,
                                            connected_clients[i].assigned_slot,
                                            new_status);
                                    log_info(log_msg);
                                    break;
                                }
                            }
                            if (!found) {
                                sprintf(log_msg, "set_risk: Sensor '%s' not found or inactive.", sensor_id);
                                log_info(log_msg);
                            }
                        } else {
                            log_info("set_risk: Invalid status. Use 0 or 1.");
                        }
                    } else {
                        log_info("set_risk: This command is only valid for STATUS SERVER (SS).");
                    }
                } else {
                    sprintf(log_msg, "Unknown command: '%s'", cmd_buf);
                    log_info(log_msg);
                }
            } else {
                log_info("STDIN closed or read error.");
                break;
            }
        }

        // --- NEW CLIENT CONNECTION ---
        if (FD_ISSET(client_master_fd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_addr_len = sizeof(client_addr);

            if ((new_client_fd = accept(client_master_fd, (struct sockaddr *)&client_addr, &client_addr_len)) < 0) {
                log_error("Failed to accept new client connection.");
            } else {
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

                int assigned_slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (client_fds[i] == 0) {
                        client_fds[i] = new_client_fd;
                        connected_clients[i].socket_fd = new_client_fd;

                        sprintf(log_msg, "New client connected from %s:%d on socket %d, assigned to slot %d.",
                                client_ip, ntohs(client_addr.sin_port), new_client_fd, i + 1);
                        log_info(log_msg);

                        assigned_slot = i;
                        break;
                    }
                }

                if (assigned_slot == -1) {
                    log_info("Client limit reached. Rejecting new connection.");
                    char err_payload[10];
                    sprintf(err_payload, "%02d", SENSOR_LIMIT_EXCEEDED);
                    char err_msg[MAX_MSG_SIZE];
                    build_control_message(err_msg, sizeof(err_msg), ERROR_MSG, err_payload);

                    if (write(new_client_fd, err_msg, strlen(err_msg)) < 0) {
                        log_error("Failed to send error message to new client.");
                    }
                    close(new_client_fd);
                }
            }
        }

        // --- PASSIVE P2P CONNECTION ---
        if (peer_listen_fd > 0 && FD_ISSET(peer_listen_fd, &read_fds)) {
            struct sockaddr_in peer_addr;
            socklen_t peer_addr_len = sizeof(peer_addr);

            int new_peer_fd = accept(peer_listen_fd, (struct sockaddr *)&peer_addr, &peer_addr_len);
            if (new_peer_fd < 0) {
                log_error("Failed to accept new P2P connection.");
            } else {
                peer_socket_fd = new_peer_fd;
                close(peer_listen_fd);  // only accept one peer
                peer_listen_fd = -1;
                p2p_current_state = P2P_PASSIVE_LISTENING;
                sprintf(log_msg, "New P2P connection accepted on socket %d. State: PASSIVE_LISTENING.", peer_socket_fd);
                log_info(log_msg);
            }
        }

        // --- P2P MESSAGE PROCESSING ---
        if (peer_socket_fd > 0 && FD_ISSET(peer_socket_fd, &read_fds)) {
            ssize_t bytes_read = read(peer_socket_fd, buffer, MAX_MSG_SIZE);
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                sprintf(log_msg, "Raw data received from peer: [%s]", buffer);
                log_info(log_msg);

                int code;
                char payload[MAX_MSG_SIZE];

                if (parse_message(buffer, &code, payload, sizeof(payload))) {
                    sprintf(log_msg, "P2P message received: Code=%d, Payload='%s'", code, payload);
                    log_info(log_msg);

                    char msg_out[MAX_MSG_SIZE];
                    char payload_out[10];

                    if (p2p_current_state == P2P_PASSIVE_LISTENING && code == REQ_CONNPEER) {
                        snprintf(my_pids_for_peer, sizeof(my_pids_for_peer), "Peer%d_Active", peer_socket_fd);
                        sprintf(log_msg, "Connected peer assigned ID: %s", my_pids_for_peer);
                        log_info(log_msg);

                        build_control_message(msg_out, sizeof(msg_out), RES_CONNPEER, my_pids_for_peer);
                        if (write(peer_socket_fd, msg_out, strlen(msg_out)) < 0) {
                            log_error("Failed to send RES_CONNPEER.");
                            close(peer_socket_fd);
                            peer_socket_fd = -1;
                            p2p_current_state = P2P_DISCONNECTED;
                        } else {
                            log_info("RES_CONNPEER sent.");
                            p2p_current_state = P2P_RES_SENT_AWAITING_RES;
                        }

                    } else if (p2p_current_state == P2P_REQ_SENT && code == RES_CONNPEER) {
                        strncpy(peer_pids_for_me, payload, sizeof(peer_pids_for_me) - 1);
                        peer_pids_for_me[sizeof(peer_pids_for_me) - 1] = '\0';

                        snprintf(my_pids_for_peer, sizeof(my_pids_for_peer), "Peer%d_Passive", peer_socket_fd);
                        log_info("P2P handshake complete (active side). Sending confirmation...");

                        build_control_message(msg_out, sizeof(msg_out), RES_CONNPEER, my_pids_for_peer);
                        if (write(peer_socket_fd, msg_out, strlen(msg_out)) < 0) {
                            log_error("Failed to send RES_CONNPEER confirmation.");
                            close(peer_socket_fd);
                            peer_socket_fd = -1;
                            p2p_current_state = P2P_DISCONNECTED;
                        } else {
                            p2p_current_state = P2P_FULLY_ESTABLISHED;
                            sprintf(log_msg, "P2P connection with peer %s (ID: %s) fully established.",
                                    my_pids_for_peer, peer_pids_for_me);
                            log_info(log_msg);
                        }

                    } else if (p2p_current_state == P2P_RES_SENT_AWAITING_RES && code == RES_CONNPEER) {
                        strncpy(peer_pids_for_me, payload, sizeof(peer_pids_for_me) - 1);
                        peer_pids_for_me[sizeof(peer_pids_for_me) - 1] = '\0';

                        p2p_current_state = P2P_FULLY_ESTABLISHED;
                        sprintf(log_msg, "P2P connection with peer %s (ID: %s) fully established.",
                                my_pids_for_peer, peer_pids_for_me);
                        log_info(log_msg);
                    } else if (code == REQ_DISCPEER) {
                        if (strcmp(payload, peer_pids_for_me) == 0) {
                            sprintf(log_msg, "REQ_DISCPEER received from peer %s (ID: %s). Confirming.", my_pids_for_peer, peer_pids_for_me);
                            log_info(log_msg);

                            sprintf(payload_out, "%02d", OK_SUCCESSFUL_DISCONNECT);
                            build_control_message(msg_out, sizeof(msg_out), OK_MSG, payload_out);

                            if (write(peer_socket_fd, msg_out, strlen(msg_out)) < 0) {
                                log_error("Failed to send OK(01) to peer.");
                            } else {
                                log_info("OK(01) sent to peer.");
                            }

                            sprintf(log_msg, "Peer %s disconnected.", my_pids_for_peer);
                            log_info(log_msg);

                            close(peer_socket_fd);
                            peer_socket_fd = -1;
                            p2p_current_state = P2P_DISCONNECTED;
                            my_pids_for_peer[0] = '\0';
                            peer_pids_for_me[0] = '\0';

                            log_info("Switching to passive P2P listening...");

                            if (peer_listen_fd <= 0) {
                                if ((peer_listen_fd = socket(AF_INET, SOCK_STREAM, 0)) != -1) {
                                    int opt = 1;
                                    setsockopt(peer_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                                    addr_peer_listen.sin_family = AF_INET;
                                    addr_peer_listen.sin_addr.s_addr = INADDR_ANY;
                                    addr_peer_listen.sin_port = htons(peer_port);

                                    if (bind(peer_listen_fd, (struct sockaddr *)&addr_peer_listen, sizeof(addr_peer_listen)) == 0 &&
                                        listen(peer_listen_fd, 1) == 0) {
                                        sprintf(log_msg, "Now listening for new P2P connections on port %d...", peer_port);
                                        log_info(log_msg);
                                    } else {
                                        log_error("Failed to restart passive P2P listening.");
                                        close(peer_listen_fd);
                                        peer_listen_fd = -1;
                                    }
                                }
                            }
                        } else {
                            sprintf(log_msg, "REQ_DISCPEER received with mismatched ID '%s'. Expected '%s'. Sending ERROR(02).", payload, peer_pids_for_me);
                            log_info(log_msg);

                            sprintf(payload_out, "%02d", PEER_NOT_FOUND);
                            build_control_message(msg_out, sizeof(msg_out), ERROR_MSG, payload_out);

                            if (write(peer_socket_fd, msg_out, strlen(msg_out)) < 0) {
                                log_error("Failed to send ERROR(02) to peer.");
                            }
                        }

                    } else if (code == OK_MSG && atoi(payload) == OK_SUCCESSFUL_DISCONNECT) {
                        log_info("OK(01) 'Successful disconnect' received from peer.");
                        sprintf(log_msg, "Peer %s disconnected.", my_pids_for_peer);
                        log_info(log_msg);

                        close(peer_socket_fd);
                        peer_socket_fd = -1;
                        p2p_current_state = P2P_DISCONNECTED;
                        my_pids_for_peer[0] = '\0';
                        peer_pids_for_me[0] = '\0';

                        log_info("Server shutting down after peer disconnection.");
                        close(client_master_fd);
                        break;

                    } else if (code == ERROR_MSG && atoi(payload) == PEER_NOT_FOUND) {
                        log_info("ERROR(02) 'Peer not found' received from peer.");
                        close(peer_socket_fd);
                        peer_socket_fd = -1;
                        p2p_current_state = P2P_DISCONNECTED;

                    } else if (code == REQ_CHECKALERT && current_server_role == SERVER_TYPE_LOCATION) {
                        char sensor_id[MAX_PIDS_LENGTH];
                        strncpy(sensor_id, payload, sizeof(sensor_id) - 1);
                        sensor_id[sizeof(sensor_id) - 1] = '\0';

                        sprintf(log_msg, "[SL] REQ_CHECKALERT for sensor %s", sensor_id);
                        log_info(log_msg);

                        int found_loc_id = -1;
                        for (int i = 0; i < MAX_CLIENTS; i++) {
                            if (connected_clients[i].socket_fd > 0 &&
                                strcmp(connected_clients[i].client_id, sensor_id) == 0) {
                                found_loc_id = connected_clients[i].location_id;
                                break;
                            }
                        }

                        if (found_loc_id > 0) {
                            sprintf(payload_out, "%d", found_loc_id);
                            build_control_message(msg_out, sizeof(msg_out), RES_CHECKALERT, payload_out);
                            sprintf(log_msg, "[SL] Found location %d for sensor %s. Sending RES_CHECKALERT.", found_loc_id, sensor_id);
                            log_info(log_msg);
                        } else {
                            sprintf(payload_out, "%02d", SENSOR_NOT_FOUND);
                            build_control_message(msg_out, sizeof(msg_out), ERROR_MSG, payload_out);
                            sprintf(log_msg, "[SL] Sensor %s not found. Sending ERROR(10).", sensor_id);
                            log_info(log_msg);
                        }

                        if (write(peer_socket_fd, msg_out, strlen(msg_out)) < 0) {
                            log_error("SL: Failed to send response to REQ_CHECKALERT.");
                        }

                    } else {
                        sprintf(log_msg, "Unexpected P2P message (Code=%d) or invalid state (%d).", code, p2p_current_state);
                        log_info(log_msg);
                    }
                } else {
                    log_error("Failed to parse P2P message.");
                    close(peer_socket_fd);
                    peer_socket_fd = -1;
                    p2p_current_state = P2P_DISCONNECTED;
                }
            } else if (bytes_read == 0) {
                log_info("Peer disconnected.");
                close(peer_socket_fd);
                peer_socket_fd = -1;
                p2p_current_state = P2P_DISCONNECTED;
                my_pids_for_peer[0] = '\0';
                peer_pids_for_me[0] = '\0';
            } else {
                log_error("Error reading from peer.");
                close(peer_socket_fd);
                peer_socket_fd = -1;
                p2p_current_state = P2P_DISCONNECTED;
            }
        }

        // --- PROCESS MESSAGES FROM CONNECTED CLIENTS ---
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int client_fd = client_fds[i];

            if (client_fd > 0 && FD_ISSET(client_fd, &read_fds)) {
                ssize_t bytes_read = read(client_fd, buffer, MAX_MSG_SIZE);
                if (bytes_read > 0) {
                    buffer[bytes_read] = '\0';

                    int code;
                    char payload[MAX_MSG_SIZE];

                    struct sockaddr_in cli_addr;
                    socklen_t cli_len = sizeof(cli_addr);
                    getpeername(client_fd, (struct sockaddr *)&cli_addr, &cli_len);
                    sprintf(log_msg, "Data received from client %s:%d (socket %d)",
                            inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port), client_fd);
                    log_info(log_msg);

                    if (parse_message(buffer, &code, payload, sizeof(payload))) {
                        sprintf(log_msg, "Parsed client message: Code=%d, Payload='%s'", code, payload);
                        log_info(log_msg);

                        // --- SENSOR REGISTRATION ---
                        if (code == REQ_CONNSEN) {
                            char sensor_id[MAX_PIDS_LENGTH];
                            char loc_id_str[10];
                            int loc_id;
                            int valid = 0;

                            char *comma = strchr(payload, ',');
                            if (comma != NULL) {
                                size_t id_len = comma - payload;
                                if (id_len > 0 && id_len < MAX_PIDS_LENGTH) {
                                    strncpy(sensor_id, payload, id_len);
                                    sensor_id[id_len] = '\0';

                                    strncpy(loc_id_str, comma + 1, sizeof(loc_id_str) - 1);
                                    loc_id_str[sizeof(loc_id_str) - 1] = '\0';

                                    if (strlen(loc_id_str) > 0) {
                                        loc_id = atoi(loc_id_str);
                                        if (strlen(sensor_id) == 10) {
                                            valid = 1;
                                            sprintf(log_msg, "REQ_CONNSEN parsed: ID='%s', LocId=%d", sensor_id, loc_id);
                                            log_info(log_msg);
                                        } else {
                                            log_error("REQ_CONNSEN: Sensor ID must be exactly 10 characters.");
                                        }
                                    } else {
                                        log_error("REQ_CONNSEN: Missing LocId.");
                                    }
                                } else {
                                    log_error("REQ_CONNSEN: Invalid sensor ID length.");
                                }
                            } else {
                                log_error("REQ_CONNSEN: Invalid format, expected 'ID,LocId'.");
                            }

                            if (!valid) {
                                char err_payload[10];
                                sprintf(err_payload, "%02d", INVALID_PAYLOAD_ERROR);
                                char msg_err[MAX_MSG_SIZE];
                                build_control_message(msg_err, sizeof(msg_err), ERROR_MSG, err_payload);
                                write(client_fd, msg_err, strlen(msg_err));
                                close(client_fd);
                                client_fds[i] = 0;
                                connected_clients[i].socket_fd = 0;
                                continue;
                            }

                            if (connected_clients[i].socket_fd != client_fd) {
                                sprintf(log_msg, "Mismatch in client slot %d: socket %d != %d. Closing connection.",
                                        i, connected_clients[i].socket_fd, client_fd);
                                log_error(log_msg);
                                close(client_fd);
                                client_fds[i] = 0;
                                if (connected_clients[i].client_id[0] != '\0') num_connected_clients--;
                                connected_clients[i] = (ClientInfo){0};
                                continue;
                            }

                            if (connected_clients[i].client_id[0] == '\0') {
                                int id_in_use = 0;
                                for (int k = 0; k < MAX_CLIENTS; k++) {
                                    if (connected_clients[k].socket_fd > 0 &&
                                        strcmp(connected_clients[k].client_id, sensor_id) == 0) {
                                        id_in_use = 1;
                                        sprintf(log_msg, "Sensor ID '%s' already in use (slot %d). Rejecting.", sensor_id, k + 1);
                                        log_error(log_msg);

                                        char err_payload[10];
                                        sprintf(err_payload, "%02d", SENSOR_ID_ALREADY_EXISTS_ERROR);
                                        char msg_err[MAX_MSG_SIZE];
                                        build_control_message(msg_err, sizeof(msg_err), ERROR_MSG, err_payload);
                                        write(client_fd, msg_err, strlen(msg_err));
                                        close(client_fd);
                                        client_fds[i] = 0;
                                        connected_clients[i].socket_fd = 0;
                                        break;
                                    }
                                }

                                if (id_in_use) continue;

                                if (num_connected_clients >= MAX_CLIENTS) {
                                    log_info("Sensor limit reached. Sending ERROR(09).");
                                    char err_payload[10];
                                    sprintf(err_payload, "%02d", SENSOR_LIMIT_EXCEEDED);
                                    char msg_err[MAX_MSG_SIZE];
                                    build_control_message(msg_err, sizeof(msg_err), ERROR_MSG, err_payload);
                                    write(client_fd, msg_err, strlen(msg_err));
                                    continue;
                                }

                                strncpy(connected_clients[i].client_id, sensor_id, MAX_PIDS_LENGTH - 1);
                                connected_clients[i].location_id = loc_id;
                                connected_clients[i].assigned_slot = i + 1;
                                if (current_server_role == SERVER_TYPE_STATUS) {
                                    connected_clients[i].risk_status = 0;
                                }
                                num_connected_clients++;

                                sprintf(log_msg, "Client registered: ID='%s', Slot=%d, LocId=%d",
                                        sensor_id, connected_clients[i].assigned_slot, loc_id);
                                log_info(log_msg);

                                char slot_str[10];
                                sprintf(slot_str, "%d", connected_clients[i].assigned_slot);
                                char res_msg[MAX_MSG_SIZE];
                                build_control_message(res_msg, sizeof(res_msg), RES_CONNSEN, slot_str);
                                write(client_fd, res_msg, strlen(res_msg));

                            } else {
                                if (strcmp(connected_clients[i].client_id, sensor_id) == 0) {
                                    sprintf(log_msg, "Client %s re-sent REQ_CONNSEN. Re-sending RES_CONNSEN.", sensor_id);
                                    log_info(log_msg);
                                    char res_msg[MAX_MSG_SIZE];
                                    build_control_message(res_msg, sizeof(res_msg), RES_CONNSEN, sensor_id);
                                    write(client_fd, res_msg, strlen(res_msg));
                                } else {
                                    sprintf(log_msg, "Client slot %d already registered with ID %s. Ignoring conflicting REQ_CONNSEN with ID %s.",
                                            i + 1, connected_clients[i].client_id, sensor_id);
                                    log_error(log_msg);
                                }
                            }

                        // --- SENSOR DISCONNECTION ---
                        } else if (code == REQ_DISCSEN) {
                            char slot_str[10];
                            strncpy(slot_str, payload, sizeof(slot_str) - 1);
                            slot_str[sizeof(slot_str) - 1] = '\0';
                            int received_slot = atoi(slot_str);

                            if (connected_clients[i].socket_fd == client_fd &&
                                connected_clients[i].assigned_slot == received_slot &&
                                connected_clients[i].client_id[0] != '\0') {

                                sprintf(log_msg, "Client (ID: %s, Slot: %d) disconnected.",
                                        connected_clients[i].client_id, connected_clients[i].assigned_slot);
                                log_info(log_msg);

                                char ok_payload[10];
                                sprintf(ok_payload, "%02d", OK_SUCCESSFUL_DISCONNECT);
                                char msg_ok[MAX_MSG_SIZE];
                                build_control_message(msg_ok, sizeof(msg_ok), OK_MSG, ok_payload);
                                write(client_fd, msg_ok, strlen(msg_ok));

                                close(client_fd);
                                client_fds[i] = 0;
                                if (connected_clients[i].client_id[0] != '\0') num_connected_clients--;
                                connected_clients[i] = (ClientInfo){0};
                            } else {
                                sprintf(log_msg, "Invalid REQ_DISCSEN: slot '%s' mismatch or client not registered. Sending ERROR(10).",
                                        slot_str);
                                log_info(log_msg);

                                char err_payload[10];
                                sprintf(err_payload, "%02d", SENSOR_NOT_FOUND);
                                char msg_err[MAX_MSG_SIZE];
                                build_control_message(msg_err, sizeof(msg_err), ERROR_MSG, err_payload);
                                write(client_fd, msg_err, strlen(msg_err));
                            }
                        // --- SENSOR STATUS REQUEST (SS only) ---
                        } else if (code == REQ_SENSSTATUS && current_server_role == SERVER_TYPE_STATUS) {
                            int slot_id = atoi(payload);

                            if (connected_clients[i].socket_fd == client_fd &&
                                connected_clients[i].assigned_slot == slot_id &&
                                connected_clients[i].client_id[0] != '\0') {

                                sprintf(log_msg, "REQ_SENSSTATUS from sensor %s (Slot: %d)",
                                        connected_clients[i].client_id, connected_clients[i].assigned_slot);
                                log_info(log_msg);

                                if (connected_clients[i].risk_status == 1) {
                                    if (peer_socket_fd > 0 && p2p_current_state == P2P_FULLY_ESTABLISHED) {
                                        sprintf(log_msg, "Sending REQ_CHECKALERT %s to SL...", connected_clients[i].client_id);
                                        log_info(log_msg);

                                        char msg_to_sl[MAX_MSG_SIZE];
                                        build_control_message(msg_to_sl, sizeof(msg_to_sl), REQ_CHECKALERT, connected_clients[i].client_id);
                                        if (write(peer_socket_fd, msg_to_sl, strlen(msg_to_sl)) < 0) {
                                            log_error("SS: Failed to send REQ_CHECKALERT to SL.");
                                        } else {
                                            char sl_response[MAX_MSG_SIZE];
                                            ssize_t sl_read = read(peer_socket_fd, sl_response, sizeof(sl_response) - 1);
                                            if (sl_read > 0) {
                                                sl_response[sl_read] = '\0';
                                                int sl_code;
                                                char sl_payload[MAX_MSG_SIZE];
                                                if (parse_message(sl_response, &sl_code, sl_payload, sizeof(sl_payload))) {
                                                    char msg_to_client[MAX_MSG_SIZE];
                                                    if (sl_code == RES_CHECKALERT) {
                                                        sprintf(log_msg, "SL responded with RES_CHECKALERT %s", sl_payload);
                                                        log_info(log_msg);
                                                        build_control_message(msg_to_client, sizeof(msg_to_client), RES_SENSSTATUS, sl_payload);
                                                        write(client_fd, msg_to_client, strlen(msg_to_client));
                                                    } else if (sl_code == ERROR_MSG && atoi(sl_payload) == SENSOR_NOT_FOUND) {
                                                        log_info("SL returned SENSOR_NOT_FOUND.");
                                                        build_control_message(msg_to_client, sizeof(msg_to_client), ERROR_MSG, sl_payload);
                                                        write(client_fd, msg_to_client, strlen(msg_to_client));
                                                    } else {
                                                        sprintf(log_msg, "Unexpected SL response: Code=%d, Payload=%s", sl_code, sl_payload);
                                                        log_error(log_msg);
                                                    }
                                                } else {
                                                    log_error("Failed to parse SL response.");
                                                }
                                            } else if (sl_read == 0) {
                                                log_error("SL disconnected before responding.");
                                            } else {
                                                log_error("Error reading SL response.");
                                            }
                                        }
                                    } else {
                                        log_error("No active P2P connection to SL.");
                                    }
                                } else {
                                    log_info("Sensor status is normal (0), no alert.");
                                    char msg_normal[MAX_MSG_SIZE];
                                    build_control_message(msg_normal, sizeof(msg_normal), RES_SENSSTATUS, "-1");
                                    write(client_fd, msg_normal, strlen(msg_normal));
                                }
                            } else {
                                sprintf(log_msg, "Invalid REQ_SENSSTATUS from client. Slot mismatch or not registered.");
                                log_error(log_msg);
                                char err_payload[10];
                                sprintf(err_payload, "%02d", SENSOR_NOT_FOUND);
                                char msg_err[MAX_MSG_SIZE];
                                build_control_message(msg_err, sizeof(msg_err), ERROR_MSG, err_payload);
                                write(client_fd, msg_err, strlen(msg_err));
                            }

                        // --- SENSOR LOCATION REQUEST (SL only) ---
                        } else if (code == REQ_SENSLOC && current_server_role == SERVER_TYPE_LOCATION) {
                            char sensor_id[MAX_PIDS_LENGTH];
                            strncpy(sensor_id, payload, sizeof(sensor_id) - 1);
                            sensor_id[sizeof(sensor_id) - 1] = '\0';

                            int loc_id_found = -1;
                            for (int k = 0; k < MAX_CLIENTS; k++) {
                                if (connected_clients[k].socket_fd > 0 &&
                                    strcmp(connected_clients[k].client_id, sensor_id) == 0) {
                                    loc_id_found = connected_clients[k].location_id;
                                    break;
                                }
                            }

                            char msg_out[MAX_MSG_SIZE];
                            if (loc_id_found != -1) {
                                sprintf(log_msg, "Sensor %s found with LocId=%d", sensor_id, loc_id_found);
                                log_info(log_msg);
                                char loc_str[10];
                                sprintf(loc_str, "%d", loc_id_found);
                                build_control_message(msg_out, sizeof(msg_out), RES_SENSLOC, loc_str);
                            } else {
                                log_info("Sensor not found. Sending ERROR(10).");
                                char err_payload[10];
                                sprintf(err_payload, "%02d", SENSOR_NOT_FOUND);
                                build_control_message(msg_out, sizeof(msg_out), ERROR_MSG, err_payload);
                            }
                            write(client_fd, msg_out, strlen(msg_out));

                        // --- LIST SENSORS AT LOCATION (SL only) ---
                        } else if (code == REQ_LOCLIST && current_server_role == SERVER_TYPE_LOCATION) {
                            char requester_slot_str[10], target_loc_str[10];
                            int target_loc_id = -1;
                            int valid = 0;

                            char *comma = strchr(payload, ',');
                            if (comma != NULL) {
                                size_t slot_len = comma - payload;
                                if (slot_len > 0 && slot_len < sizeof(requester_slot_str)) {
                                    strncpy(requester_slot_str, payload, slot_len);
                                    requester_slot_str[slot_len] = '\0';
                                    strncpy(target_loc_str, comma + 1, sizeof(target_loc_str) - 1);
                                    target_loc_str[sizeof(target_loc_str) - 1] = '\0';
                                    target_loc_id = atoi(target_loc_str);
                                    valid = 1;
                                }
                            }

                            if (!valid || target_loc_id < 1 || target_loc_id > 10) {
                                log_error("REQ_LOCLIST: Invalid format or location.");
                                char err_payload[10];
                                sprintf(err_payload, "%02d", SENSOR_NOT_FOUND);
                                char msg_err[MAX_MSG_SIZE];
                                build_control_message(msg_err, sizeof(msg_err), ERROR_MSG, err_payload);
                                write(client_fd, msg_err, strlen(msg_err));
                                continue;
                            }

                            char sensor_list[MAX_MSG_SIZE] = "";
                            int count = 0;
                            for (int k = 0; k < MAX_CLIENTS; k++) {
                                if (connected_clients[k].socket_fd > 0 &&
                                    connected_clients[k].location_id == target_loc_id) {
                                    if (count++ > 0) strcat(sensor_list, ",");
                                    strcat(sensor_list, connected_clients[k].client_id);
                                }
                            }

                            char msg_out[MAX_MSG_SIZE];
                            if (count > 0) {
                                sprintf(log_msg, "Found %d sensors at location %d", count, target_loc_id);
                                log_info(log_msg);
                                build_control_message(msg_out, sizeof(msg_out), RES_LOCLIST, sensor_list);
                            } else {
                                sprintf(log_msg, "No sensors found at location %d. Sending ERROR(10).", target_loc_id);
                                log_info(log_msg);
                                char err_payload[10];
                                sprintf(err_payload, "%02d", SENSOR_NOT_FOUND);
                                build_control_message(msg_out, sizeof(msg_out), ERROR_MSG, err_payload);
                            }

                            write(client_fd, msg_out, strlen(msg_out));

                        } else {
                            sprintf(log_msg, "Unknown or unexpected client message code: %d", code);
                            log_info(log_msg);
                        }
                    } else {
                        log_error("Failed to parse client message.");
                    }
                } else if (bytes_read == 0) {
                    sprintf(log_msg, "Client (socket %d) disconnected.", client_fd);
                    log_info(log_msg);
                    close(client_fd);
                    client_fds[i] = 0;
                    if (connected_clients[i].socket_fd != 0) {
                        connected_clients[i] = (ClientInfo){0};
                        if (num_connected_clients > 0) num_connected_clients--;
                    }
                } else {
                    log_error("Error reading from client.");
                    close(client_fd);
                    client_fds[i] = 0;
                    if (connected_clients[i].socket_fd != 0) {
                        connected_clients[i] = (ClientInfo){0};
                        if (num_connected_clients > 0) num_connected_clients--;
                    }
                }
            }
        }
    } // end of main loop
    log_info("Shutting down and cleaning up...");
    close(client_master_fd);
    if (peer_socket_fd > 0) close(peer_socket_fd);
    if (peer_listen_fd > 0) close(peer_listen_fd);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        connected_clients[i] = (ClientInfo){0};
    }

    log_info("Server terminated.");
    return 0;
}
