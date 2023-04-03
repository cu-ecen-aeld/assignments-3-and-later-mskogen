#include "threading.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

#define MSEC_TO_NSEC(X) (X * 1000000)

void* threadfunc(void* thread_param)
{
    int ret_val = 0;
    struct thread_data* thread_func_args = (struct thread_data*) thread_param;
    struct timespec wait_remaining;

    // Wait to obtain mutex
    retry_obtain:
    ret_val = nanosleep(&(thread_func_args->wait_obtain_time), &wait_remaining);

    if (ret_val != 0) {
        if (errno == EINTR) {
            // Retry sleep with remaining time
            thread_func_args->wait_obtain_time.tv_sec = wait_remaining.tv_sec;
            thread_func_args->wait_obtain_time.tv_nsec = wait_remaining.tv_nsec;
            goto retry_obtain;
        }
        ERROR_LOG("obtain nanosleep: %s", strerror(errno));
    }

    // Obtain mutex
    ret_val = pthread_mutex_lock(thread_func_args->mutex);
    if (ret_val != 0) {
        ERROR_LOG("pthread_mutex_lock: %s", strerror(ret_val));
    }

    // Wait to release mutex
    retry_release:
    ret_val = nanosleep(&(thread_func_args->wait_release_time), &wait_remaining);

    if (ret_val != 0) {
        if (errno == EINTR) {
            // Retry sleep with remaining time
            thread_func_args->wait_release_time.tv_sec = wait_remaining.tv_sec;
            thread_func_args->wait_release_time.tv_nsec = wait_remaining.tv_nsec;
            goto retry_release;
        }
        ERROR_LOG("release nanosleep: %s", strerror(errno));
    }

    // Release mutex
    ret_val = pthread_mutex_unlock(thread_func_args->mutex);
    if (ret_val != 0) {
        ERROR_LOG("pthread_mutex_unlock: %s", strerror(ret_val));
    }

    // If we made it this far then the thread has completed successfully
    thread_func_args->thread_complete_success = true;

    return thread_param;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex, int wait_to_obtain_ms, int wait_to_release_ms)
{
    int ret_val = 0;

    // Allocate memory for thread_data
    struct thread_data* thread_func_args = (struct thread_data*) malloc(sizeof(struct thread_data));
    
    // Setup mutex and wait arguments
    thread_func_args->mutex = mutex;
    thread_func_args->wait_obtain_time.tv_sec = 0;
    thread_func_args->wait_obtain_time.tv_nsec = MSEC_TO_NSEC(wait_to_obtain_ms);
    thread_func_args->wait_release_time.tv_sec = 0;
    thread_func_args->wait_release_time.tv_nsec = MSEC_TO_NSEC(wait_to_release_ms);
    
    // Pass thread_data to created thread. Use threadfunc() as entry point.
    ret_val = pthread_create(thread, NULL, threadfunc, thread_func_args);

    if (ret_val != 0) {
        ERROR_LOG("pthread_create: %s", strerror(ret_val));
        free(thread_func_args);
        return false;
    }

    return true;
}

