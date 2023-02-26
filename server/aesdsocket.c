/*****************************************************************************
​*​ ​Copyright​ ​(C)​ ​2023 ​by​ Matthew Skogen
​*
​*​ ​Redistribution,​ ​modification​ ​or​ ​use​ ​of​ ​this​ ​software​ ​in​ ​source​ ​or​ ​binary
​*​ ​forms​ ​is​ ​permitted​ ​as​ ​long​ ​as​ ​the​ ​files​ ​maintain​ ​this​ ​copyright.​ ​Users​ ​are
​*​ ​permitted​ ​to​ ​modify​ ​this​ ​and​ ​use​ ​it​ ​to​ ​learn​ ​about​ ​the​ ​field​ ​of​ ​embedded
​*​ ​software.​ ​Matthew Skogen ​and​ ​the​ ​University​ ​of​ ​Colorado​ ​are​ ​not​ ​liable​ ​for
​*​ ​any​ ​misuse​ ​of​ ​this​ ​material.
​*
*****************************************************************************/
/**
​*​ ​@file​ aesdsocket.c
​*​ ​@brief​ Server module for basic socket communication
​*
​*​ ​@author​s ​Matthew Skogen
​*​ ​@date​ February 23 ​2023
* 
*
* Sources & Links: Used examples and inspiration from the Beej Server Guide:
*    https://beej.us/guide/bgnet/html/
*    And context/examples from the book, "Linux System Programming" Chapter 10
*    authored by, Robert Love 
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>


#define DEFAULT_PROTOCOL    (0)
#define BACKLOG             (10)
#define SERVER_SUCCESS      (0)
#define SERVER_FAILURE      (-1)
#define SERVER_PORT         ("9000")
#define TMP_FILE            ("/var/tmp/aesdsocketdata")
#define READ_SIZE           (1024)
#define WRITE_SIZE          (1024)

int exit_status = 0;
int socket_fd = 0;

// Handles both SIGINT and SIGTERM signals
static void signal_handler(int signum)
{
    if ((signum == SIGINT) || (signum == SIGTERM)) {
        syslog(LOG_INFO, "Caught signal, exiting\n");
    } else { // Unknown signal
        syslog(LOG_ERR, "Error: Caught unknown signal\n");
        return;
    }
    close(socket_fd);
    exit_status = 1;
    return;
}

// Beej Guide helper function for fetching proper sockaddr
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char *argv[])
{
    int daemon;

    // Verify proper usage of program
    if ((argc == 2) && (strcmp(argv[1],"-d") == 0)) {
        // daemon mode specified
        daemon = 1;
    } else if (argc == 1) {
        // foreground application
        daemon = 0;
    } else {
        printf("ERROR: Invalid arguments %i\n", argc);
        printf("Usage: ./aesdsocket [-d]\n");
        return SERVER_FAILURE;
    }

    openlog("aesdsocket", LOG_CONS, LOG_USER);

    if (signal(SIGINT, signal_handler) == SIG_ERR) {
        syslog(LOG_ERR, "Error: Cannot register SIGINT\n");
        closelog();
        return SERVER_FAILURE;
    }

    if (signal(SIGTERM, signal_handler) == SIG_ERR) {
        syslog(LOG_ERR, "Error: Cannot register SIGTERM\n");
        closelog();
        return SERVER_FAILURE;
    }

    int status = 0, errors = 0;

    int client_fd;
    int sockopt_yes = 1;
    struct sockaddr_storage client_addr;
    socklen_t client_addrlen;
    
    struct addrinfo *socket_addrinfo, *p_ai;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_protocol = DEFAULT_PROTOCOL;

    status = getaddrinfo(NULL, SERVER_PORT, &hints, &socket_addrinfo);

    if (status != 0) {
        syslog(LOG_ERR, "Error getaddrinfo(): %s\n", gai_strerror(status));
        closelog();
        return SERVER_FAILURE;
    }
    
    // Open a stream socket SOCK_STREAM bound to port 9000, return -1 if 
    // connection steps fail
    for (p_ai = socket_addrinfo ; p_ai != NULL; p_ai = p_ai->ai_next) {

        socket_fd = socket(p_ai->ai_family, p_ai->ai_socktype, p_ai->ai_protocol);

        if (socket_fd == -1) {
            syslog(LOG_ERR, "Error socket(): %s\n", strerror(errno));
            errors++;
        } else {
            status = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR,
                                &sockopt_yes, sizeof(sockopt_yes));

            if (status == -1) {
                syslog(LOG_ERR, "Error setsockopt(): %s\n", strerror(errno));
                errors++;
            } else {
                status = bind(socket_fd, p_ai->ai_addr, p_ai->ai_addrlen);
                
                if (status == -1) {
                    syslog(LOG_ERR, "Error bind(): %s\n", strerror(errno));
                    errors++;
                }
                break;
            }
        }
    }

    // Free memory allocated in getaddrinfo() call
    if (socket_addrinfo != NULL) {
        freeaddrinfo(socket_addrinfo);
        socket_addrinfo = NULL;
    }

    if (errors > 0) {
        close(socket_fd);
        syslog(LOG_ERR, "Errors during socket setup\n");
        closelog();
        return SERVER_FAILURE;
    }

    // DAEMON argument fork() here
    if (daemon) {
        pid_t pid = fork();
        switch (pid) {
        case -1:
            // Failed to create child process
            close(socket_fd);
            syslog(LOG_ERR, "Error fork()\n");
            closelog();
            return SERVER_FAILURE;
        case 0:
            // Inside child process
            syslog(LOG_DEBUG, "Successfully created child process()\n");
            break;
        default:
            // Close parent process
            close(socket_fd);
            closelog();
            return SERVER_SUCCESS;
        }
    }
        
    char client_ip[INET6_ADDRSTRLEN];

    // Listen for and accept a connection, restarts when connection closed
    // listens forever unless SIGINT or SIGTERM received, if signal received,
    // gracefully exit.
    status = listen(socket_fd, BACKLOG);

    if (status == -1) {
        close(socket_fd);
        syslog(LOG_ERR, "Error listen(): %s\n", strerror(errno));
        closelog();
        return SERVER_FAILURE;
    }

    syslog(LOG_DEBUG, "Waiting for a client to connect...\n");

    // Loop back to Listen for accept connection
    while (exit_status == 0)
    {
        client_addrlen = sizeof(client_addr);
        client_fd = accept(socket_fd, 
                            (struct sockaddr*)&client_addr, 
                            &client_addrlen);

        if (client_fd == -1) {
            // Ignore bad file descriptor error when shutdown starts
            if (errno != EBADF) {
                close(socket_fd);
                syslog(LOG_ERR, "Error accept(): %s\n", strerror(errno));
            }
            exit_status = 1;
            continue;
        }

        // Log message to syslog "Accepted connection from <CLIENT_IP_ADDRESS>"
        memset(client_ip, 0, sizeof(client_ip));
        inet_ntop(client_addr.ss_family, 
                    get_in_addr((struct sockaddr*)&client_addr), 
                    client_ip, 
                    sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s\n", client_ip);

        int rx_size = READ_SIZE;
        int rx_bytes = 0;
        char* rx_buffer = malloc(rx_size);

        if (rx_buffer == NULL) {
            syslog(LOG_ERR, "Error failed to malloc()\n");
            close(socket_fd);
            close(client_fd);
            exit_status = 1;
            continue;
        }

        // Zero memory before reading
        memset(rx_buffer, 0, rx_size);
        int total_bytes = 0;
        int bytes_to_read = 0;
        char* p_end = NULL;

        // Create file to write packets to
        FILE* data_file = fopen(TMP_FILE, "a+");

        if (data_file == NULL) {
            syslog(LOG_ERR, "Error fopen(): %s\n", strerror(errno));
            close(socket_fd);
            close(client_fd);
            if (rx_buffer != NULL) {
                free(rx_buffer);
                rx_buffer = NULL;
            }
            exit_status = 1;
            continue;
        }

        while (1) {

            bytes_to_read = rx_size - total_bytes;

            if (bytes_to_read == 0) {
                // Increase size of rx_buffer
                rx_size += READ_SIZE;
                bytes_to_read = READ_SIZE;
                rx_buffer = realloc(rx_buffer, rx_size);
                if (rx_buffer == NULL) {
                    syslog(LOG_ERR, "Error failed to realloc()\n");
                    fclose(data_file);
                    close(socket_fd);
                    close(client_fd);
                    break; // send to cleanup
                }
                memset(&(rx_buffer[total_bytes]), 0, READ_SIZE);
            }

            rx_bytes = recv(client_fd, &(rx_buffer[total_bytes]), rx_size - total_bytes, 0);

            if (rx_bytes == 0) {
                break; // done sending
            }

            total_bytes += rx_bytes;

            // check buffer for '\n' newline character
            // If it exists, append to file and send back on client 
            // check for all '\n' characters, after transmitting
            // move all data down to zero index of malloc'd packet
            // USE strchr() to fine \n characters: 
            // https://man7.org/linux/man-pages/man3/strchr.3.html
            if (total_bytes < rx_size) {
                while (1) {
                    // Check for newline
                    p_end = strchr(rx_buffer, '\n');

                    if (p_end == NULL) {
                        break; // no newline found
                    }

                    // Write packet to file
                    char* p = rx_buffer;
                    int packet_len = p_end - p;
                    while (p <= p_end) {
                        fprintf(data_file, "%c", *p);
                        p++;
                        total_bytes--;
                    }

                    // Remove saved packet and shift data down to start of buffer.
                    memset(rx_buffer, 0, packet_len);
                    memcpy(rx_buffer, p, total_bytes);

                    // Send packet back on client
                    char line[WRITE_SIZE];
                    memset(line, 0, WRITE_SIZE);
                    int tx_bytes = 0;
                    int tx_bytes_to_send = 0;

                    // Reset file pointer to read from beginning of file
                    rewind(data_file);

                    while (fgets(line, WRITE_SIZE, data_file) != NULL) {
                        tx_bytes_to_send = strlen(line);
                        tx_bytes = send(client_fd, line, tx_bytes_to_send, 0);
                        if (tx_bytes == -1) {
                            syslog(LOG_ERR, "Error send(): %s\n", strerror(errno));
                            break;
                        } else if (tx_bytes_to_send != tx_bytes) {
                            syslog(LOG_ERR, "Error only sent %i bytes but"
                                "should have been %i bytes\n", 
                                tx_bytes, tx_bytes_to_send);
                            break;
                        } else {
                            syslog(LOG_DEBUG, "Success: sent %i bytes\n", tx_bytes);
                        }
                        memset(line, 0, sizeof(line));
                    }
                }
            }
        }

        if (fclose(data_file) != 0) {
            syslog(LOG_ERR, "Error fclose(): %s\n", strerror(errno));
        }

        if (rx_buffer != NULL) {
            free(rx_buffer);
            rx_buffer = NULL;
        }

        if (close(client_fd) == -1) {
            syslog(LOG_ERR, "Error close(client_fd): %s\n", strerror(errno));
        }

        // Log message to syslog "Closed connection from <CLIENT_IP_ADDRESS>"
        syslog(LOG_INFO, "Closed connection from %s\n", client_ip);
    }

    // Cleanup any open file decriptors or files
    if (close(client_fd) == -1) {
        syslog(LOG_DEBUG, "Error close(client_fd): %s\n", strerror(errno));
    }
    if (remove(TMP_FILE) == -1) {
        syslog(LOG_DEBUG, "Error remove(%s): %s\n", TMP_FILE, strerror(errno));
    }

    // Close syslog file
    closelog();

    return SERVER_SUCCESS;
}