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
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>

#include "aesd_ioctl.h"

// FreeBSD Macro for safe slist looping
// Copied from: https://github.com/stockrt/queue.h/blob/master/queue.h
#define SLIST_FOREACH_SAFE(var, head, field, tvar)                           \
    for ((var) = SLIST_FIRST((head));                                        \
            (var) && ((tvar) = SLIST_NEXT((var), field), 1);                 \
            (var) = (tvar))

#define DEFAULT_PROTOCOL    (0)
#define BACKLOG             (10)
#define SERVER_SUCCESS      (0)
#define SERVER_FAILURE      (-1)
#define SERVER_PORT         ("9000")
#if USE_AESD_CHAR_DEVICE == 1
    #define TMP_FILE            ("/dev/aesdchar")
#else
    #define TMP_FILE            ("/var/tmp/aesdsocketdata")
#endif
#define READ_SIZE           (1024)
#define WRITE_SIZE          (1024)
#define AESD_IOCTL_PREFIX_LEN (19)

bool exit_status = false;
int socket_fd = 0;
bool socket_connected = false;
bool syslog_open = false;
bool tmp_file_exists = false;
pthread_mutex_t thread_mutex;
bool mutex_active = false;

#if USE_AESD_CHAR_DEVICE == 0
timer_t timer;
bool timer_active = false;
#endif

struct thread_info {
    pthread_t thread_id;
    pthread_mutex_t *mutex;
    bool thread_complete;
    bool client_connected;
    int client_fd;
    struct sockaddr_storage client_addr;
    SLIST_ENTRY(thread_info) threads;
};

// Handles both SIGINT and SIGTERM signals
static void signal_handler(int signum)
{
    if ((signum == SIGINT) || (signum == SIGTERM)) {
        syslog(LOG_INFO, "Caught signal, exiting\n");

        if (shutdown(socket_fd, SHUT_RDWR) == -1) {
            syslog(LOG_ERR, "Error: shutdown() %s\n", strerror(errno));
        }

        // Set global to start server shutdown when possible
        exit_status = true;
        return;
    }
    
    // Unknown signal
    syslog(LOG_ERR, "Error: Caught unknown signal\n");
    return;
}

#if USE_AESD_CHAR_DEVICE == 0
// Handler serviced everytime timer expires
// String to write is RFC 2822 compliant "timestamp:%a, %d %b %Y %T %z"
void timer_thread_handler(union sigval sv)
{
    int status = 0;
    int ts_len = 0;
    char ts_str[200];
    char ts_format[] = "timestamp:%a, %d %b %Y %T %z\n";
    time_t t;
    struct tm *ts;
    pthread_mutex_t* mutex = (pthread_mutex_t*)sv.sival_ptr;

    // Fetch current time since Epoch
    t = time(NULL);
    if (t == ((time_t)-1)) {
        syslog(LOG_ERR, "time(): %s\n", strerror(errno));
        return;
    }

    // Fetch local timestamp from time since Epoch
    ts = localtime(&t);
    if (ts == NULL) {
        syslog(LOG_ERR, "localtime(): %s\n", strerror(errno));
        return;
    }

    // Ensure timestamp memory is zero'd and populate string
    memset(&ts_str, 0, sizeof(ts_str));
    ts_len = strftime(ts_str, sizeof(ts_str), ts_format, ts);

    // Open file to log timestamp to
    FILE* data_file = fopen(TMP_FILE, "a");
    if (data_file == NULL) {
        syslog(LOG_ERR, "fopen(): %s\n", strerror(errno));
        return;
    }

    // Lock thread mutex and write to file then release
    status = pthread_mutex_lock(mutex);
    if (status) {
        syslog(LOG_ERR, "pthread_mutex_lock(): %s\n", strerror(status));
    }

    if (ts_len != fwrite(&ts_str, sizeof(char), ts_len, data_file)) {
        syslog(LOG_ERR, "Failed to write timestamp()");
    }

    status = pthread_mutex_unlock(mutex);
    if (status) {
        syslog(LOG_ERR, "pthread_mutex_unlock(): %s\n", strerror(status));
    }

    // If we made it here, file was opened successfully so close file
    fclose(data_file);

    return;
}
#endif

// Cleanup connections before closing
void cleanup(bool terminate)
{
    int status = 0;

    if (socket_connected) {
        close(socket_fd);
        socket_connected = false;
    }

    // If we are exiting after this call, close all open file descriptors
    if (terminate) {
#if USE_AESD_CHAR_DEVICE == 0
        if (timer_active) {
            status = timer_delete(timer);
            if (status != 0) {
                syslog(LOG_ERR, "Error timer_delete(): %s\n", strerror(errno));
            }
            timer_active = false;
        }
#endif

        if (mutex_active) {
            status = pthread_mutex_destroy(&thread_mutex);
            if (status != 0) {
                syslog(LOG_ERR, "Error pthread_mutex_destroy(): %s\n", strerror(status));
            }
            mutex_active = false;
        }

        if (tmp_file_exists) {
            status = remove(TMP_FILE);
            if (status != 0) {
                syslog(LOG_ERR, "Error remove(): %s\n", strerror(errno));
            }
            tmp_file_exists = false;
        }

        if (syslog_open) {
            closelog();
            syslog_open = false;
        }
    }

    // Set exit status in case it hasn't been set already
    exit_status = true;

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

void* client_thread_func (void *thread_args)
{
    struct thread_info* client_info = (struct thread_info*)thread_args;
    int client_errors = 0;
    bool tmp_file_open = false;

    // Log message to syslog "Accepted connection from <CLIENT_IP_ADDRESS>"
    char client_ip[INET6_ADDRSTRLEN];
    memset(client_ip, 0, sizeof(client_ip));
    inet_ntop(client_info->client_addr.ss_family, 
                get_in_addr((struct sockaddr*)&(client_info->client_addr)), 
                client_ip, 
                sizeof(client_ip));
    syslog(LOG_INFO, "Accepted connection from %s\n", client_ip);

    int rx_size = READ_SIZE;
    int rx_bytes = 0;
    char* rx_buffer = malloc(rx_size);

    if (rx_buffer == NULL) {
        syslog(LOG_ERR, "Error failed to malloc()\n");
        close(client_info->client_fd);
        client_info->client_connected = false;
        client_info->thread_complete = true;
        client_errors++;
        cleanup(false);
        pthread_exit(&client_errors);
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
        if (rx_buffer != NULL) {
            free(rx_buffer);
            rx_buffer = NULL;
        }
        close(client_info->client_fd);
        client_info->client_connected = false;
        client_info->thread_complete = true;
        client_errors++;
        cleanup(false);
        pthread_exit(&client_errors);
    } else {
        tmp_file_exists = true;
        tmp_file_open = true;
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
                client_errors++;
                break; // send to cleanup
            }
            memset(&(rx_buffer[total_bytes]), 0, READ_SIZE);
        }

        rx_bytes = recv(client_info->client_fd,
                        &(rx_buffer[total_bytes]),
                        rx_size - total_bytes,
                        0);

        if (rx_bytes == 0) {
            break; // Client is done sending
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
                int status = 0, num_tokens = 0;
                struct aesd_seekto ioctl_arg;
                memset(&ioctl_arg, 0, sizeof(ioctl_arg));

                // Check for newline
                p_end = strchr(rx_buffer, '\n');

                if (p_end == NULL) {
                    break; // no newline found
                }

                // Write packet to file
                char* p = rx_buffer;
                int packet_len = p_end - p;
                status = pthread_mutex_lock(client_info->mutex);
                if (status) {
                    syslog(LOG_ERR, "pthread_mutex_lock(): %s\n", strerror(status));
                    client_errors++;
                    break;
                }

                // Special handling for AESDCHAR_IOCSEEKTO:X,Y commands
                if (strncmp(p, "AESDCHAR_IOCSEEKTO:", AESD_IOCTL_PREFIX_LEN) == 0) {
                    syslog(LOG_INFO, "Received AESDCHAR_IOCSEEKTO command.\n");
                    p += AESD_IOCTL_PREFIX_LEN;
                    char *token = strtok(p, ",");

                    if (token != NULL) {
                        ioctl_arg.write_cmd = (uint32_t)strtoul(token, NULL, 10);
                        num_tokens++;

                        // Get next token
                        token = strtok(NULL, ",");

                        if (token != NULL) {
                            ioctl_arg.write_cmd_offset = (uint32_t)strtoul(token, NULL, 10);
                            num_tokens++;
                        } else {
                            syslog(LOG_ERR, "write_cmd_offset missing.\n");
                            client_errors++;
                        }
                    } else {
                        syslog(LOG_ERR, "write_cmd missing.\n");
                        client_errors++;
                    }

                    if ((!client_errors) && (num_tokens == 2)) {
                        if(ioctl(fileno(data_file), AESDCHAR_IOCSEEKTO, &ioctl_arg)) {
                            syslog(LOG_ERR, "ioctl AESDCHAR_IOCSEEKTO failed.\n");
                            client_errors++;
                        }
                    }

                } else {
                    // Normal write received command to file
                    while (p <= p_end) {
                        fprintf(data_file, "%c", *p);
                        p++;
                        total_bytes--;
                    }

                    // Reset file pointer to read from beginning of file for
                    // sending file back
                    rewind(data_file);
                }

                status = pthread_mutex_unlock(client_info->mutex);
                if (status) {
                    syslog(LOG_ERR, "pthread_mutex_unlock(): %s\n", strerror(status));
                    client_errors++;
                    break;
                }

                // Remove saved packet and shift data down to start of buffer.
                memset(rx_buffer, 0, packet_len);
                memcpy(rx_buffer, p, total_bytes);

                // Send contents of file back to client
                char line[WRITE_SIZE];
                memset(line, 0, WRITE_SIZE);
                int tx_bytes = 0;
                int tx_bytes_to_send = 0;

                while (fgets(line, WRITE_SIZE, data_file) != NULL) {
                    tx_bytes_to_send = strlen(line);
                    tx_bytes = send(client_info->client_fd, line, 
                                    tx_bytes_to_send, 0);
                    if (tx_bytes == -1) {
                        syslog(LOG_ERR, "Error send(): %s\n", strerror(errno));
                        client_errors++;
                        break;
                    } else if (tx_bytes_to_send != tx_bytes) {
                        syslog(LOG_ERR, "Error only sent %i bytes but"
                            "should have been %i bytes\n", 
                            tx_bytes, tx_bytes_to_send);
                        client_errors++;
                        break;
                    } else {
                        syslog(LOG_DEBUG, "Success: sent %i bytes\n", tx_bytes);
                    }
                    memset(line, 0, sizeof(line));
                }
            }
        }

        // If there were any errors, don't try to receive more data, exit
        if (client_errors > 0) {
            break;
        }
    }

    if (tmp_file_open) {
        if (fclose(data_file) != 0) {
            syslog(LOG_ERR, "Error fclose(): %s\n", strerror(errno));
        }
        tmp_file_open = false;
    }

    if (rx_buffer != NULL) {
        free(rx_buffer);
        rx_buffer = NULL;
    }

    if (client_info->client_connected) {
        close(client_info->client_fd);
        client_info->client_connected = false;
    }

    // Log message to syslog "Closed connection from <CLIENT_IP_ADDRESS>"
    syslog(LOG_INFO, "Closed connection from %s\n", client_ip);

    client_info->thread_complete = true;
    pthread_exit(&client_errors);
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
    syslog_open = true;

    if (signal(SIGINT, signal_handler) == SIG_ERR) {
        syslog(LOG_ERR, "Error: Cannot register SIGINT\n");
        cleanup(true);
        return SERVER_FAILURE;
    }

    if (signal(SIGTERM, signal_handler) == SIG_ERR) {
        syslog(LOG_ERR, "Error: Cannot register SIGTERM\n");
        cleanup(true);
        return SERVER_FAILURE;
    }

    int status = 0;
    int errors = 0;
    int sockopt_yes = 1;
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
        cleanup(true);
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
            socket_connected = true;
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
        syslog(LOG_ERR, "Errors during socket setup\n");
        cleanup(true);
        return SERVER_FAILURE;
    }

    // DAEMON argument fork() here
    if (daemon) {
        pid_t pid = fork();
        switch (pid) {
        case -1:
            // Failed to create child process
            syslog(LOG_ERR, "Error fork()\n");
            cleanup(true);
            return SERVER_FAILURE;
        case 0:
            // Inside child process
            syslog(LOG_DEBUG, "Successfully created child process()\n");
            break;
        default:
            // Close parent process
            cleanup(true);
            return SERVER_SUCCESS;
        }
    }

    // Listen for and accept a connection, restarts when connection closed
    // listens forever unless SIGINT or SIGTERM received, if signal received,
    // gracefully exit.
    status = listen(socket_fd, BACKLOG);

    if (status == -1) {
        syslog(LOG_ERR, "Error listen(): %s\n", strerror(errno));
        cleanup(true);
        return SERVER_FAILURE;
    }

    syslog(LOG_DEBUG, "Waiting for a client to connect...\n");

    // Setup thread linked list
    struct thread_info* p_thread_info = NULL;
    struct thread_info* p_thread_temp = NULL;
    SLIST_HEAD(head_thread, thread_info) head;
    SLIST_INIT(&head);

    // Setup thread mutex, we are about to start spawning threads
    status = pthread_mutex_init(&thread_mutex, NULL);
    if (status != 0) {
        syslog(LOG_ERR, "Error pthread_mutex_init(): %s\n", strerror(status));
        cleanup(true);
        return SERVER_FAILURE;
    } else {
        mutex_active = true;
    }

#if USE_AESD_CHAR_DEVICE == 0
    // Setup timer for logging to tmp file
    struct sigevent timer_event;
    struct itimerspec itime_spec;

    memset(&timer_event, 0, sizeof(struct sigevent));
    timer_event.sigev_value.sival_ptr = &thread_mutex;
    timer_event.sigev_notify = SIGEV_THREAD;
    timer_event.sigev_notify_function = &timer_thread_handler;

    status = timer_create(CLOCK_REALTIME, &timer_event, &timer);

    if (status) {
        syslog(LOG_ERR, "Error timer_create(): %s\n", strerror(errno));
        cleanup(true);
        return SERVER_FAILURE;
    } else {
        timer_active = true;
    }

    // Set time to trigger every 10 seconds, 0 nanoseconds
    memset(&itime_spec, 0, sizeof(struct itimerspec));
    itime_spec.it_interval.tv_sec = 10;
    itime_spec.it_value.tv_sec = 10;

    status = timer_settime(timer, 0, &itime_spec, NULL);

    if (status) {
        syslog(LOG_ERR, "Error timer_settime(): %s\n", strerror(errno));
        cleanup(true);
        return SERVER_FAILURE;
    }
#endif

    // Loop back to accept multiple connections
    while (!exit_status)
    {
        int client_fd;
        struct sockaddr_storage client_addr;
        socklen_t client_addrlen = sizeof(client_addr);
        client_fd = accept(socket_fd, 
                            (struct sockaddr*)&client_addr, 
                            &client_addrlen);

        if (client_fd == -1) {
            // Ignore bad file descriptor error when shutdown starts
            if (errno != EBADF) {
                syslog(LOG_ERR, "Error accept(): %s\n", strerror(errno));
            }
            exit_status = true;
            continue;
        } else {
            // Allocate memory for thread_data            
            p_thread_info = (struct thread_info*) malloc(sizeof(struct thread_info));

            if (p_thread_info == NULL) {
                syslog(LOG_ERR, "Failed to malloc for new thread(): %s\n", strerror(errno));
                exit_status = true;
                continue;
            }
            
            // Setup mutex and wait arguments
            p_thread_info->mutex = &thread_mutex;
            p_thread_info->thread_complete = false;
            p_thread_info->client_connected = true;
            p_thread_info->client_fd = client_fd;
            p_thread_info->client_addr = client_addr;

            // Pass thread_data to created thread. Use threadfunc() as entry point.
            status = pthread_create(&(p_thread_info->thread_id), NULL, client_thread_func, p_thread_info);

            // ADD THREAD TO LINKED LIST
            SLIST_INSERT_HEAD(&head, p_thread_info, threads);
        }

        // Check on all threads to see if they are complete
        SLIST_FOREACH_SAFE(p_thread_info, &head, threads, p_thread_temp) {
            // If thread is complete remove
            if (p_thread_info->thread_complete) {
                if (p_thread_info->client_connected) {
                    close(p_thread_info->client_fd);
                }
                pthread_join(p_thread_info->thread_id, NULL);
                SLIST_REMOVE(&head, p_thread_info, thread_info, threads);
                free(p_thread_info);
            }

        }
    }

    // Cleanup all threads here - join calls and free all pthread objects
    while (!SLIST_EMPTY(&head)) {
        p_thread_info = SLIST_FIRST(&head);
        if (p_thread_info->client_connected) {
            close(p_thread_info->client_fd);
        }
        pthread_join(p_thread_info->thread_id, NULL);
        SLIST_REMOVE_HEAD(&head, threads);
        free(p_thread_info);
    }

    // Finish cleanup of socket and syslog if needed
    cleanup(true);

    return SERVER_SUCCESS;
}
