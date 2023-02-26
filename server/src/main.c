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

#include "server.h"

#define DEFAULT_PROTOCOL    (0)
#define SERVER_SUCCESS      (0)
#define SERVER_FAILURE      (-1)
#define SERVER_PORT         ("9000")
#define TMP_FILE            ("/var/tmp/aesdsocketdata")
#define READ_SIZE           (1024)
#define WRITE_SIZE          (1024)

static int exit_status = 0;

// Handles both SIGINT and SIGTERM signals
static void signal_handler(int signum)
{
    if (signum == SIGINT) {
        printf("Caught SIGINT, cleaning up...\n");
    } else if (signum == SIGTERM) {
        printf("Caught SIGTERM, cleaning up...\n");
    } else {
        // Unknown signal
        printf("Error: unknown signal\n");
        return;
    }
    exit_status = 1;
    return;
}

int main(int argc, char *argv[])
{
    if (signal(SIGINT, signal_handler) == SIG_ERR) {
        // Cannot register SIGINT, error
        printf("Error: Cannot register SIGINT\n");
        return SERVER_FAILURE;
    }

    if (signal(SIGTERM, signal_handler) == SIG_ERR) {
        // Cannot register SIGTERM, error
        printf("Error: Cannot register SIGTERM\n");
        return SERVER_FAILURE;
    }

    int status = 0, errors = 0;

    int server_fd, client_fd;
    int sockopt_yes = 1;
    struct sockaddr client_addr;
    socklen_t client_addrlen = sizeof(client_addr);
    
    struct addrinfo *socket_addrinfo, *p_ai;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_protocol = DEFAULT_PROTOCOL;


    const char* socket_port = SERVER_PORT;

    status = getaddrinfo(NULL, socket_port, &hints, &socket_addrinfo);

    if (status != 0) {
        // ERROR LOG HERE ("getaddrinfo: %s\n", gai_strerror(status))
        printf("Error getaddrinfo(): %s\n", gai_strerror(status));
        return SERVER_FAILURE;
    }
    
    for (p_ai = socket_addrinfo ; p_ai != NULL; p_ai = p_ai->ai_next) {
        // Open a stream socket SOCK_STREAM bound to port 9000, return -1 if 
        // connection steps fail
        server_fd = socket(p_ai->ai_family, p_ai->ai_socktype, p_ai->ai_protocol);

        if (server_fd == -1) {
            // ERROR LOG HERE "Failed to create socker" errno set
            printf("Error socket(): %s\n", strerror(errno));
            errors++;
        } else {
            status = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                                &sockopt_yes, sizeof(sockopt_yes));

            if (status == -1) {
                // ERROR LOG HERE "Failed to setsockopt" errno set
                printf("Error setsockopt(): %s\n", strerror(errno));
                errors++;
            } else {
                status = bind(server_fd, p_ai->ai_addr, p_ai->ai_addrlen);
                
                if (status == -1) {
                    // ERROR LOG HERE ("bind(): %s\n" errno set
                    close(server_fd);
                    printf("Error bind(): %s\n", strerror(errno));
                    errors++;
                }
                break;
            }
        }
    }

    if (socket_addrinfo != NULL) {
        freeaddrinfo(socket_addrinfo);
    }

    if (errors > 0) {
        if (server_fd != -1) {
            close(server_fd);
        }
        printf("Errors during socket setup\n");
        return SERVER_FAILURE;
    }

    // DAEMON argument fork() here

    char client_ip[INET_ADDRSTRLEN];

    // Listen for and accept a connection, restarts when connection closed
    // listens forever unless SIGINT or SIGTERM received, if signal received,
    // gracefully exit.
    status = listen(server_fd, SOMAXCONN);

    if (status == -1) {
        // ERROR LOG HERE "Failed to listen" errno set
        printf("Error listen(): %s\n", strerror(errno));
        return SERVER_FAILURE;
    }

    printf("Waiting for a client to connect...\n");
    while (!exit_status)
    {
        client_fd = accept(server_fd, &client_addr, &client_addrlen);

        if (client_fd == -1) {
            // ERROR LOG HERE "Failed to accept" errno set
            printf("Error accept(): %s\n", strerror(errno));
            return SERVER_FAILURE;
        }

        // Log message to syslog "Accepted connection from <CLIENT_IP_ADDRESS>"
        memset(client_ip, 0, sizeof(client_ip));
        inet_ntop(AF_INET, &client_addr, client_ip, sizeof(client_ip));
        printf("Accepted connection from %s\n", client_ip);

        int rx_size = READ_SIZE;
        int rx_bytes = 0;
        char* rx_buffer = malloc(rx_size);

        if (rx_buffer == NULL) {
            printf("Error failed to malloc()\n");
        }

        // Zero memory before reading
        memset(rx_buffer, 0, rx_size);
        int total_bytes = 0;
        int bytes_to_read = 0;
        char* p_end = NULL;

        // Create file to write packets to
        FILE* data_file = fopen(TMP_FILE, "a");

        if (data_file == NULL) {
            printf("Error fopen(): %s\n", strerror(errno));
        } else {
            printf("Successfully opened file %s\n", TMP_FILE);
        }

        while (1) {

            bytes_to_read = rx_size - total_bytes;

            if (bytes_to_read == 0) {
                // Increase size of rx_buffer
                rx_size += READ_SIZE;
                bytes_to_read = READ_SIZE;
                rx_buffer = realloc(rx_buffer, rx_size);
                if (rx_buffer == NULL) {
                    printf("Error failed to realloc()\n");
                    if (fclose(data_file) != 0) {
                        printf("Error fclose(): %s\n", strerror(errno));
                    }
                    break; // send to cleanup
                }
                memset(&(rx_buffer[total_bytes]), 0, READ_SIZE);
            }


            rx_bytes = recv(client_fd, &(rx_buffer[total_bytes]), rx_size - total_bytes, MSG_WAITALL);
            printf("Read %i bytes\n", rx_bytes);

            if (rx_bytes == 0) {
                printf("Done reading!\n");
                if (fclose(data_file) != 0) {
                    printf("Error fclose(): %s\n", strerror(errno));
                }
                break; // done sending
            }

            total_bytes += rx_bytes;
            printf("total_bytes now %i bytes\n", total_bytes);

            // check buffer for '\n' newline character
            // If it exists, append to file and send back on client 
            // check for all '\n' characters, after transmitting
            // move all data down to zero index of malloc'd packet
            // USE strchr() to fine \n characters: https://man7.org/linux/man-pages/man3/strchr.3.html
            if (total_bytes < rx_size) {
                while (1) {
                    printf("checking for newline\n");
                    p_end = strchr(rx_buffer, '\n');

                    if (p_end == NULL) {
                        break;
                    }

                    printf("Found a newline! saving to file\n");

                    char* p = rx_buffer;
                    int packet_len = p_end - p;
                    while (p <= p_end) {
                        fprintf(data_file, "%c", *p);
                        p++;
                        total_bytes--;
                    }

                    printf("packet saved, new total_bytes is %i\n", total_bytes);

                    // Remove saved packet and shift data down to start of buffer.
                    memset(rx_buffer, 0, packet_len);
                    memcpy(rx_buffer, p, total_bytes);
                }
            }
        }
        
        if (rx_buffer != NULL) {
            free(rx_buffer);
        }

        // Open file to send back
        data_file = fopen(TMP_FILE, "r");

        if (data_file == NULL) {
            printf("Error fopen(): %s\n", strerror(errno));
        } else {
            printf("Opened %s to send back!\n", TMP_FILE);
            // char* line = malloc(WRITE_SIZE);
            char line[WRITE_SIZE];

            // if (line == NULL) {
            //     printf("Error failed to malloc line\n");
            // } else {
            memset(line, 0, WRITE_SIZE);
            int tx_bytes = 0;

            while (fgets(line, (WRITE_SIZE - 1), data_file) != NULL) {
                
                printf("Found a line, sending %s now\n", line);

                tx_bytes = send(client_fd, line, strlen(line), 0);
                if (tx_bytes == -1) {
                    printf("Error send(): %s\n", strerror(errno));
                    break;
                } else {
                    printf("Sent a %i byte packet\n", tx_bytes);
                    memset(line, 0, sizeof(line));
                }
            }
            //     free(line);
            // }

            if (fclose(data_file) != 0) {
                printf("Error fclose(): %s\n", strerror(errno));
            }
        }

        printf("Finished sending to client\n");
        if (close(client_fd) == -1) {
            printf("Error close(client_fd): %s\n", strerror(errno));
        }
        printf("Closed connection from %s\n", client_ip);
    }
    // Receive data over the connection and append to file 
    // /var/tmp/aesdsocketdata, Create file if doesn't exist
    // Separate data packets by "/n", each newline appends to /var/tmp/aesdsocketdata
    // Assume no null characters, can use string handling functions
    // use malloc, assume packet will be shorter than available heap size so you
    // can fail program if malloc fails and can discard over-length packets


    // At end of of transmission send /var/tmp/aesdsocketdata back to client
    // Don't assume total size of all packets sent will be less than available RAM
    // for process heap, meaning you need to read line by line to send data back

    // Log message to syslog "Closed connection from <CLIENT_IP_ADDRESS>"

    // Loop back to Listen for accept connection

    // Cleanup from SIGINT/SIGTERM 
    // Gracefully exit,
    
    // Log message to syslog "Caught signal, exiting" for SIGINT/SIGTERM received

    // Completes any open connection operations
    
    // close sockets
    // close file descritors for socket() and accept() and /var/tmp/aesdsocketdata
    if (close(server_fd) == -1) {
        printf("Error close(server_fd): %s\n", strerror(errno));
    }
    // delete /var/tmp/aesdsocketdata
    if (remove(TMP_FILE) == -1) {
        printf("Error remove(%s): %s\n", TMP_FILE, strerror(errno));
    }

    return SERVER_SUCCESS;
}