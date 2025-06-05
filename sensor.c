#include "common.h"
#include <ctype.h> // For isdigit

// ID received from the servers
char my_sensor_id[MAX_PIDS_LENGTH] = "";
int initial_loc_id = -1;

// Connects to a server (SS or SL) and gets the sensor ID
int connect_and_get_id(const char *server_type_name, const char *server_ip, int server_port,
                       int loc_id,                      // Sensor's location ID
                       char *id_storage,                // Where the server-confirmed ID will be stored
                       const char *sensor_id_to_send) { // Sensor ID to be sent in REQ_CONNSEN
    int sockfd;
    struct sockaddr_in serv_addr;
    char buffer[MAX_MSG_SIZE + 1];                // Buffer for building and receiving messages
    char log_msg[150];                            // Buffer for log messages
    char payload_for_req_connsen[MAX_MSG_SIZE];   // Buffer for the composite REQ_CONNSEN payload

    // Create and connect the socket
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        sprintf(log_msg, "Failed to create socket for %s", server_type_name);
        log_error(log_msg);
        return -1;
    }

    serv_addr.sin_family = AF_INET;          // IPv4 address family
    serv_addr.sin_port = htons(server_port); // Server port
    // Convert the server IP address
    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {
        sprintf(log_msg, "Invalid IP address for %s", server_type_name);
        log_error(log_msg);
        close(sockfd);
        return -1;
    }

    // Connect to the server
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        sprintf(log_msg, "Failed to connect to %s server (%s:%d)", server_type_name, server_ip, server_port);
        log_error(log_msg);
        close(sockfd);
        return -1;
    }
    sprintf(log_msg, "Connected to %s server (%s:%d).", server_type_name, server_ip, server_port);
    log_info(log_msg);

    // Send REQ_CONNSEN with payload "Sensor_ID,LocId"
    char loc_id_as_string[12];
    sprintf(loc_id_as_string, "%d", loc_id);

    // Build the composite payload: "SENSOR_ID,LOC_ID"
    snprintf(payload_for_req_connsen, sizeof(payload_for_req_connsen), "%s,%s",
             sensor_id_to_send,
             loc_id_as_string);

    build_control_message(buffer, sizeof(buffer), REQ_CONNSEN, payload_for_req_connsen);

    sprintf(log_msg, "Sending REQ_CONNSEN (Payload: %s) to %s", payload_for_req_connsen, server_type_name);
    log_info(log_msg);

    if (write(sockfd, buffer, strlen(buffer)) < 0) {
        sprintf(log_msg, "Failed to send REQ_CONNSEN to %s", server_type_name);
        log_error(log_msg);
        close(sockfd);
        return -1;
    }

    // Wait for and process RES_CONNSEN(SlotID)
    ssize_t bytes_read = read(sockfd, buffer, MAX_MSG_SIZE);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        int code;
        char received_payload[MAX_PIDS_LENGTH]; // To store the received SlotID

        if (parse_message(buffer, &code, received_payload, sizeof(received_payload))) {
            if (code == RES_CONNSEN) {
                sprintf(log_msg, "%s responded with RES_CONNSEN: Code=%d, Payload='%s'", server_type_name, code, received_payload);
                log_info(log_msg);
                strncpy(id_storage, received_payload, MAX_PIDS_LENGTH - 1);
                id_storage[MAX_PIDS_LENGTH - 1] = '\0';
                return sockfd; // Success, return the socket descriptor
            } else if (code == ERROR_MSG) {
                int error_code_payload = atoi(received_payload);
                sprintf(log_msg, "%s responded with ERROR(%02d)", server_type_name, error_code_payload);
                log_error(log_msg);
            } else {
                sprintf(log_msg, "%s responded with an unexpected message: Code=%d, Payload='%s'", server_type_name, code, received_payload);
                log_info(log_msg);
            }
        } else {
            sprintf(log_msg, "Failed to parse response from %s server.", server_type_name);
            log_error(log_msg);
        }
    } else if (bytes_read == 0) {
        sprintf(log_msg, "%s server disconnected before sending RES_CONNSEN.", server_type_name);
        log_info(log_msg);
    } else { // bytes_read < 0
        sprintf(log_msg, "Failed to read RES_CONNSEN response from %s server", server_type_name);
        log_error(log_msg);
    }

    // If we reached here, something went wrong
    close(sockfd);
    return -1; // Failure
}


int main(int argc, char *argv[]) {
    if (argc < 7) {
        fprintf(stderr, "Usage: %s <ss_server_ip> <ss_port> <sl_server_ip> <sl_port> <SENSOR_ID> <initial_loc_id>\n", argv[0]);
        fprintf(stderr, "Example: ./sensor 127.0.0.1 61000 127.0.0.1 62000 1234567890 1\n");
        exit(EXIT_FAILURE);
    }

    char *ss_ip = argv[1];
    int ss_port = atoi(argv[2]);
    char *sl_ip = argv[3];
    int sl_port = atoi(argv[4]);
    strncpy(my_sensor_id, argv[5], sizeof(my_sensor_id) - 1);
    my_sensor_id[sizeof(my_sensor_id) - 1] = '\0';
    initial_loc_id = atoi(argv[6]);

    char log_msg[150];

    // Validate Sensor ID
    if (strlen(my_sensor_id) != 10) {
        fprintf(stderr, "Error: SENSOR_ID must be exactly 10 numeric characters long.\n");
        exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < 10; ++i) {
        if (!isdigit((unsigned char)my_sensor_id[i])) {
            fprintf(stderr, "Error: SENSOR_ID must contain only numeric digits.\n");
            exit(EXIT_FAILURE);
        }
    }

    sprintf(log_msg, "Sensor initialized with ID: %s, LocId: %d", my_sensor_id, initial_loc_id);
    log_info(log_msg);

    int ss_fd = -1;
    int sl_fd = -1;
    char confirmed_slot_id_ss[MAX_PIDS_LENGTH];
    char confirmed_slot_id_sl[MAX_PIDS_LENGTH];

    // Connect to Status Server (SS)
    ss_fd = connect_and_get_id("SS", ss_ip, ss_port, initial_loc_id, confirmed_slot_id_ss, my_sensor_id);
    if (ss_fd < 0) {
        log_info("Could not get Slot ID from Status Server. Shutting down.");
        exit(EXIT_FAILURE);
    }

    // Connect to Location Server (SL)
    sl_fd = connect_and_get_id("SL", sl_ip, sl_port, initial_loc_id, confirmed_slot_id_sl, my_sensor_id);
    if (sl_fd < 0) {
        log_info("Could not get Slot ID from Location Server. Shutting down.");
        if (ss_fd > 0) close(ss_fd);
        exit(EXIT_FAILURE);
    }

    // Check if the slot IDs from both servers match
    if (strcmp(confirmed_slot_id_ss, confirmed_slot_id_sl) != 0) {
        log_error("Slot IDs confirmed by SS and SL do not match. Shutting down.");
        close(ss_fd);
        close(sl_fd);
        exit(EXIT_FAILURE);
    }

    log_info("Initial handshake with SS and SL completed.");
    sprintf(log_msg, "Sensor slot ID %s confirmed by both SS and SL.", confirmed_slot_id_ss);
    log_info(log_msg);

    // Main loop for user commands
    printf("Enter commands ('check failure', 'locate <SensorID>', 'diagnose <LocID>', 'kill' to exit):\n");
    char command_line[MAX_MSG_SIZE];
    while (fgets(command_line, sizeof(command_line), stdin) != NULL) {
        command_line[strcspn(command_line, "\n")] = 0; // Remove newline
        char sensor_log_msg[150];
        char msg_buffer[MAX_MSG_SIZE];
        char response_buffer[MAX_MSG_SIZE];

        if (strcmp(command_line, "kill") == 0) {
            log_info("'kill' command received. Disconnecting from SS and SL servers...");

            // Disconnect from SS
            if (ss_fd > 0) {
                sprintf(sensor_log_msg, "Sending REQ_DISCSEN (Slot ID: %s) to SS...", confirmed_slot_id_ss);
                log_info(sensor_log_msg);
                build_control_message(msg_buffer, sizeof(msg_buffer), REQ_DISCSEN, confirmed_slot_id_ss);
                if (write(ss_fd, msg_buffer, strlen(msg_buffer)) < 0) {
                    log_error("Failed to send REQ_DISCSEN to SS");
                } else {
                    read(ss_fd, response_buffer, sizeof(response_buffer) - 1); // Read response, but don't strictly need to process it for 'kill'
                    log_info("Received disconnect confirmation from SS.");
                }
                close(ss_fd);
                ss_fd = -1;
            }

            // Disconnect from SL
            if (sl_fd > 0) {
                sprintf(sensor_log_msg, "Sending REQ_DISCSEN (Slot ID: %s) to SL...", confirmed_slot_id_sl);
                log_info(sensor_log_msg);
                build_control_message(msg_buffer, sizeof(msg_buffer), REQ_DISCSEN, confirmed_slot_id_sl);
                if (write(sl_fd, msg_buffer, strlen(msg_buffer)) < 0) {
                    log_error("Failed to send REQ_DISCSEN to SL");
                } else {
                    read(sl_fd, response_buffer, sizeof(response_buffer) - 1);
                    log_info("Received disconnect confirmation from SL.");
                }
                close(sl_fd);
                sl_fd = -1;
            }

            log_info("Disconnection requested from servers. Shutting down sensor.");
            break;
        } else if (strcmp(command_line, "check failure") == 0) {
            if (ss_fd > 0) {
                sprintf(sensor_log_msg, "Sending REQ_SENSSTATUS (Slot ID: %s) to SS...", confirmed_slot_id_ss);
                log_info(sensor_log_msg);
                build_control_message(msg_buffer, sizeof(msg_buffer), REQ_SENSSTATUS, confirmed_slot_id_ss);
                if (write(ss_fd, msg_buffer, strlen(msg_buffer)) < 0) {
                    log_error("Failed to send REQ_SENSSTATUS to SS");
                } else {
                    ssize_t bytes_read = read(ss_fd, response_buffer, sizeof(response_buffer) - 1);
                    if (bytes_read > 0) {
                        response_buffer[bytes_read] = '\0';
                        int code; char payload[MAX_MSG_SIZE];
                        if (parse_message(response_buffer, &code, payload, sizeof(payload))) {
                            if (code == RES_SENSSTATUS) {
                                int loc_id = atoi(payload);
                                if (loc_id == -1) {
                                    log_info("Normal status reported for the sensor.");
                                } else {
                                    sprintf(sensor_log_msg, "Alert received from location ID: %d", loc_id);
                                    log_info(sensor_log_msg);
                                }
                            } else {
                                log_info("Received error or unexpected response from SS.");
                            }
                        } else { log_error("Failed to parse response from SS for REQ_SENSSTATUS"); }
                    } else { log_error("Failed to read response from SS or disconnected"); }
                }
            }
        } else if (strncmp(command_line, "locate ", strlen("locate ")) == 0) {
            char target_sensor_id[MAX_PIDS_LENGTH];
            if (sscanf(command_line, "locate %49s", target_sensor_id) == 1) {
                if (sl_fd > 0) {
                    sprintf(sensor_log_msg, "Sending REQ_SENSLOC for sensor '%s' to SL...", target_sensor_id);
                    log_info(sensor_log_msg);
                    build_control_message(msg_buffer, sizeof(msg_buffer), REQ_SENSLOC, target_sensor_id);
                    if (write(sl_fd, msg_buffer, strlen(msg_buffer)) < 0) {
                        log_error("Failed to send REQ_SENSLOC to SL");
                    } else {
                        ssize_t bytes_read = read(sl_fd, response_buffer, sizeof(response_buffer) - 1);
                        if (bytes_read > 0) {
                            response_buffer[bytes_read] = '\0';
                            int code; char payload[MAX_MSG_SIZE];
                            if (parse_message(response_buffer, &code, payload, sizeof(payload))) {
                                if (code == RES_SENSLOC) {
                                    sprintf(sensor_log_msg, "Sensor '%s' is at location ID: %s", target_sensor_id, payload);
                                    log_info(sensor_log_msg);
                                } else if (code == ERROR_MSG && atoi(payload) == SENSOR_NOT_FOUND) {
                                    log_info("Sensor not found at SL.");
                                } else {
                                    log_info("Received error or unexpected response from SL for REQ_SENSLOC.");
                                }
                            } else { log_error("Failed to parse response from SL for REQ_SENSLOC"); }
                        } else { log_error("Failed to read response from SL or disconnected"); }
                    }
                }
            }
        } else if (strncmp(command_line, "diagnose ", strlen("diagnose ")) == 0) {
            int target_loc_id;
            if (sscanf(command_line, "diagnose %d", &target_loc_id) == 1) {
                if (sl_fd > 0) {
                    char payload_req_loclist[MAX_MSG_SIZE];
                    snprintf(payload_req_loclist, sizeof(payload_req_loclist), "%s,%d", confirmed_slot_id_sl, target_loc_id);
                    
                    sprintf(sensor_log_msg, "Sending REQ_LOCLIST for location %d to SL...", target_loc_id);
                    log_info(sensor_log_msg);
                    build_control_message(msg_buffer, sizeof(msg_buffer), REQ_LOCLIST, payload_req_loclist);
                    if (write(sl_fd, msg_buffer, strlen(msg_buffer)) < 0) {
                        log_error("Failed to send REQ_LOCLIST to SL");
                    } else {
                        ssize_t bytes_read = read(sl_fd, response_buffer, sizeof(response_buffer) - 1);
                        if (bytes_read > 0) {
                            response_buffer[bytes_read] = '\0';
                            int code; char payload[MAX_MSG_SIZE];
                            if (parse_message(response_buffer, &code, payload, sizeof(payload))) {
                                if (code == RES_LOCLIST) {
                                    sprintf(sensor_log_msg, "Sensors at location %d: [%s]", target_loc_id, payload);
                                    log_info(sensor_log_msg);
                                } else {
                                    log_info("Received error or unexpected response from SL (Location possibly has no sensors or is invalid).");
                                }
                            } else { log_error("Failed to parse response from SL for REQ_LOCLIST"); }
                        } else { log_error("Failed to read response from SL or disconnected"); }
                    }
                }
            }
        } else {
            log_info("Unknown command.");
        }
        printf("Enter commands ('check failure', 'locate <SensorID>', 'diagnose <LocID>', 'kill' to exit):\n");
    }

    if (ss_fd > 0) close(ss_fd);
    if (sl_fd > 0) close(sl_fd);
    log_info("Sensor shut down.");
    return 0;
}