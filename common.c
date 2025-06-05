#include "common.h"

// Prints an error message
void log_error(const char *msg) {
    perror(msg); // perror displays the system error message
}

// Prints an informational message
void log_info(const char *msg) {
    fprintf(stdout, "[INFO] %s\n", msg);
    fflush(stdout);
}

// Builds a control message in the "code payload" format
void build_control_message(char *buffer, size_t buffer_size, int code, const char *payload) {
    if (payload != NULL && strlen(payload) > 0) {
        snprintf(buffer, buffer_size, "%d %s", code, payload);
    } else {
        snprintf(buffer, buffer_size, "%d ", code);
    }
}

// Parses a message, extracting the code and payload.
// Returns 1 if parsing is successful, 0 otherwise.
// Assumes the payload (if it exists) is a simple string until the end of the line.
int parse_message(const char *buffer, int *code, char *payload_buffer, size_t payload_buffer_size) {
    // Clear the payload_buffer beforehand to prevent garbage data if no payload exists
    if (payload_buffer_size > 0) {
        payload_buffer[0] = '\0';
    }

    int n_scanned = sscanf(buffer, "%d %[^\n]", code, payload_buffer);

    if (n_scanned >= 1) { // At least the code must be read successfully.
        if (n_scanned == 1) {
            // If only the code was read, ensure the payload is empty
            if (payload_buffer_size > 0) {
                 payload_buffer[0] = '\0';
            }
        }
        return 1; // Success
    }
    
    // Fallback for messages with only a code and trailing spaces
    n_scanned = sscanf(buffer, "%d", code);
    if (n_scanned == 1) {
        if (payload_buffer_size > 0) {
            payload_buffer[0] = '\0'; // Ensure payload is empty
        }
        return 1; // Success
    }

    return 0; // Parsing failed
}
