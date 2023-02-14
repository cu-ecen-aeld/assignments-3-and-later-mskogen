#include "threading.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

#define MSEC_TO_USEC(X) (X * 1000)

void* threadfunc(void* thread_param)
{
    int ret_val = 0;
    struct thread_data* thread_func_args = (struct thread_data*) thread_param;

    // Wait to obtain mutex
    usleep(thread_func_args->wait_obtain_usec);

    // Obtain mutex
    ret_val = pthread_mutex_lock(thread_func_args->mutex);
    if (ret_val != 0) {
        ERROR_LOG("pthread_mutex_lock: %s", strerror(ret_val));
    }

    // Wait to release mutex
    usleep(thread_func_args->wait_release_usec);

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
    thread_func_args->wait_obtain_usec = MSEC_TO_USEC(wait_to_obtain_ms);
    thread_func_args->wait_release_usec = MSEC_TO_USEC(wait_to_release_ms);
    
    // Pass thread_data to created thread. Use threadfunc() as entry point.
    ret_val = pthread_create(thread, NULL, threadfunc, thread_func_args);

    if (ret_val != 0) {
        ERROR_LOG("pthread_create: %s", strerror(ret_val));
        return false;
    }

    return true;
}

