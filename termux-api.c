/* termux-api.c - helper binary for calling termux api classes
 * Usage: termux-api ${API_METHOD} ${ADDITIONAL_FLAGS}
 * This executes
 *   am broadcast com.termux.api/.TermuxApiReceiver \
 *   --es socket_input ${INPUT_SOCKET} \
 *   --es socket_output ${OUTPUT_SOCKET} \
 *   --es api_method ${API_METHOD} \
 *   ${ADDITIONAL_FLAGS}
 * where ${INPUT_SOCKET} and ${OUTPUT_SOCKET} are addresses to linux
 * abstract namespace sockets, used to pass on stdin to the java
 * implementation and pass back output from java to stdout.
 */
#define _POSIX_SOURCE
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "termuxAPI.h"

int main(int argc, char** argv) {
    // Do not transform children into zombies when they terminate:
    struct sigaction sigchld_action = {
        .sa_handler = SIG_DFL,
        .sa_flags = SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT
    };
    sigaction(SIGCHLD, &sigchld_action, NULL);

    char input_address_string[100];  // This program reads from it.
    char output_address_string[100]; // This program writes to it.

    generate_uuid(input_address_string);
    generate_uuid(output_address_string);

    struct sockaddr_un input_address = { .sun_family = AF_UNIX };
    struct sockaddr_un output_address = { .sun_family = AF_UNIX };

    // Leave struct sockaddr_un.sun_path[0] as 0 and use the UUID
    // string as abstract linux namespace:
    strncpy(&input_address.sun_path[1], input_address_string,
            strlen(input_address_string));
    strncpy(&output_address.sun_path[1], output_address_string,
            strlen(output_address_string));

    int input_server_socket = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (input_server_socket == -1) { perror("socket()"); return 1; }
    int output_server_socket = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (output_server_socket == -1) { perror("socket()"); return 1; }

    int ret = bind(input_server_socket, (struct sockaddr*) &input_address,
                   sizeof(sa_family_t) + strlen(input_address_string) + 1);
    if (ret == -1) {
        perror("bind(input)");
        return 1;
    }

    ret = bind(output_server_socket, (struct sockaddr*) &output_address,
               sizeof(sa_family_t) + strlen(output_address_string) + 1);
    if (ret == -1) {
        perror("bind(output)");
        return 1;
    }

    if (listen(input_server_socket, 1) == -1) { perror("listen()"); return 1; }
    if (listen(output_server_socket, 1) == -1) { perror("listen()"); return 1; }

    pid_t fork_result = fork();
    switch (fork_result) {
        case -1: perror("fork()"); return 1;
        case 0: exec_am_broadcast(argc, argv, input_address_string,
                                  output_address_string);
    }

    struct sockaddr_un remote_addr;
    socklen_t addrlen = sizeof(remote_addr);
    int input_client_socket = accept(input_server_socket, (struct sockaddr*)
                                     &remote_addr, &addrlen);

    pthread_t transmit_thread;
    pthread_create(&transmit_thread, NULL, transmit_stdin_to_socket,
                   &output_server_socket);

    int fd = transmit_socket_to_stdout(input_client_socket);
    close(input_client_socket);
    if (fd != -1) { exec_callback(fd); }

    return 0;
}
