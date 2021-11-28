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

#ifndef PREFIX
# define PREFIX "/data/data/com.termux/files/usr"
#endif

// Function which execs "am broadcast ..".
_Noreturn void exec_am_broadcast(int argc, char** argv,
                                 char* input_address_string,
                                 char* output_address_string)
{
    // Redirect stdout to /dev/null (but leave stderr open):
    close(STDOUT_FILENO);
    open("/dev/null", O_RDONLY);
    // Close stdin:
    close(STDIN_FILENO);

    int const extra_args = 15; // Including ending NULL.
    char** child_argv = malloc((sizeof(char*)) * (argc + extra_args));

    child_argv[0] = "am";
    child_argv[1] = "broadcast";
    child_argv[2] = "--user";
    child_argv[3] = "0";
    child_argv[4] = "-n";
    child_argv[5] = "com.termux.api/.TermuxApiReceiver";
    child_argv[6] = "--es";
    // Input/output are reversed for the java process (our output is its input):
    child_argv[7] = "socket_input";
    child_argv[8] = output_address_string;
    child_argv[9] = "--es";
    child_argv[10] = "socket_output";
    child_argv[11] = input_address_string;
    child_argv[12] = "--es";
    child_argv[13] = "api_method";
    child_argv[14] = argv[1];

    // Copy the remaining arguments -2 for first binary and second api name:
    memcpy(child_argv + extra_args, argv + 2, (argc-1) * sizeof(char*));

    // End with NULL:
    child_argv[argc + extra_args] = NULL;

    // Use an a executable taking care of PATH and LD_LIBRARY_PATH:
    execv(PREFIX "/bin/am", child_argv);

    perror("execv(\"" PREFIX "/bin/am\")");
    exit(1);
}

_Noreturn void exec_callback(int fd)
{
    char *fds;
    if (asprintf(&fds, "%d", fd) == -1)
        perror("asprintf");

    /* TERMUX_EXPORT_FD and TERMUX_USB_FD are (currently) specific for
       termux-usb, so there's some room for improvement here (this
       function should be generic) */
    char errmsg[256];
    char *export_to_env = getenv("TERMUX_EXPORT_FD");
    if (export_to_env && strncmp(export_to_env, "true", 4) == 0) {
        if (setenv("TERMUX_USB_FD", fds, true) == -1)
            perror("setenv");
        execl(PREFIX "/libexec/termux-callback", "termux-callback", NULL);
        sprintf(errmsg, "execl(\"" PREFIX "/libexec/termux-callback\")");
    } else {
        execl(PREFIX "/libexec/termux-callback", "termux-callback", fds, NULL);
        sprintf(errmsg, "execl(\"" PREFIX "/libexec/termux-callback\", %s)", fds);
    }
    perror(errmsg);
    exit(1);
}

void generate_uuid(char* str) {
    sprintf(str, "%x%x-%x-%x-%x-%x%x%x",
            /* 64-bit Hex number */
            arc4random(), arc4random(),
            /* 32-bit Hex number */
            (uint32_t) getpid(),
            /* 32-bit Hex number of the form 4xxx (4 is the UUID version) */
            ((arc4random() & 0x0fff) | 0x4000),
            /* 32-bit Hex number in the range [0x8000, 0xbfff] */
            arc4random() % 0x3fff + 0x8000,
            /*  96-bit Hex number */
            arc4random(), arc4random(), arc4random());
}

// Thread function which reads from stdin and writes to socket.
void* transmit_stdin_to_socket(void* arg) {
    int output_server_socket = *((int*) arg);
    struct sockaddr_un remote_addr;
    socklen_t addrlen = sizeof(remote_addr);
    int output_client_socket = accept(output_server_socket,
                                      (struct sockaddr*) &remote_addr,
                                      &addrlen);

    ssize_t len;
    char buffer[1024];
    while (len = read(STDIN_FILENO, &buffer, sizeof(buffer)), len > 0) {
        if (write(output_client_socket, buffer, len) < 0) break;
    }
    // Close output socket on end of input:
    close(output_client_socket);
    return NULL;
}

// Main thread function which reads from input socket and writes to stdout.
int transmit_socket_to_stdout(int input_socket_fd) {
    ssize_t len;
    char buffer[1024];
    char cbuf[256];
    struct iovec io = { .iov_base = buffer, .iov_len = sizeof(buffer) };
    struct msghdr msg = { 0 };
    int fd = -1;  // An optional file descriptor received through the socket
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    while ((len = recvmsg(input_socket_fd, &msg, 0)) > 0) {
        struct cmsghdr * cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            if (cmsg->cmsg_type == SCM_RIGHTS) {
                fd = *((int *) CMSG_DATA(cmsg));
            }
        }
        // A file descriptor must be accompanied by a non-empty message,
        // so we use "@" when we don't want any output.
        if (fd != -1 && len == 1 && buffer[0] == '@') { len = 0; }
        write(STDOUT_FILENO, buffer, len);
        msg.msg_controllen = sizeof(cbuf);
    }
    if (len < 0) perror("recvmsg()");
    return fd;
}
