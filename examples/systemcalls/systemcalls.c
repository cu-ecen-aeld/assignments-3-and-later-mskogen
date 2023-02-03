#include "systemcalls.h"

/**
 * @param cmd the command to execute with system()
 * @return true if the command in @param cmd was executed
 *   successfully using the system() call, false if an error occurred,
 *   either in invocation of the system() call, or if a non-zero return
 *   value was returned by the command issued in @param cmd.
*/
bool do_system(const char *cmd)
{
    int wait_val = 0;
    wait_val = system(cmd);

    /* Return an error if the system call failed. */
    if ((wait_val == 0){
        printf("Error shell unavailable\n");
        return false;
    } else if (wait_val == -1)) {
        perror("system() failed: ");
        return false;
    }

    /* 
     * If we made it here the return val is a valid wait status. Check it to 
     * make sure that the command exited properly and did not return a non-zero 
     * value.
     */
    if (WIFEXITED(wait_val)) {
        return (WEXITSTATUS(wait_val) == EXIT_SUCCESS);
    }

    /* If process didn't exit properly, return false*/
    return false;
}

/**
* @param count -The numbers of variables passed to the function. The variables are command to execute.
*   followed by arguments to pass to the command
*   Since exec() does not perform path expansion, the command to execute needs
*   to be an absolute path.
* @param ... - A list of 1 or more arguments after the @param count argument.
*   The first is always the full path to the command to execute with execv()
*   The remaining arguments are a list of arguments to pass to the command in execv()
* @return true if the command @param ... with arguments @param arguments were executed successfully
*   using the execv() call, false if an error occurred, either in invocation of the
*   fork, waitpid, or execv() command, or if a non-zero return value was returned
*   by the command issued in @param arguments with the specified arguments.
*/

bool do_exec(int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    va_end(args);
    command[count] = NULL;

    /* If there is no command then there is nothing to do */
    if (command[0] == NULL) {
        return false;
    }

    int wait_val = 0;
    int ret_val = 0;
    pid_t pid;

    /* Create a child process */
    pid = fork();

    if (pid == -1) {
        /* Failed to create child process */
        perror("fork() failed: ");
        return false;
    } else if (pid == 0) {
        /* Inside child process */
        ret_val = execv(command[0], &command[1]);
        
        /* If process failed to execute return false for error */
        if (ret_val == -1) {
            perror("execv() failed: ");
            exit(EXIT_FAILURE);
        }
    }

    /* Parent does cleanup of process, waits for child process to exit */
    if (wait(&wait_val) == -1) {
        perror("wait() failed: ");
        return false;
    }

    /* 
     * If we made it here the return val is a valid wait status. Check it to 
     * make sure that the command exited properly and did not return a non-zero 
     * value.
     */
    if (WIFEXITED(wait_val)) {
        return (WEXITSTATUS(wait_val) == EXIT_SUCCESS);
    }

    return false;
}

/**
* @param outputfile - The full path to the file to write with command output.
*   This file will be closed at completion of the function call.
* All other parameters, see do_exec above
*/
bool do_exec_redirect(const char *outputfile, int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;
    va_end(args);

    /* If there is no command then there is nothing to do */
    if (command[0] == NULL) {
        return false;
    }

    int wait_val = 0;
    int ret_val = 0;
    int errors = 0;
    int fd = -1;
    pid_t pid;

    fd = open("do_exec_redirect_log.txt", (O_WRONLY | O_TRUNC | O_CREAT), 0644);    

    /* Output errors to stdout if log file creation failed */
    if (fd < 0) {
        perror("open() failed: ");
    }

    /* Create a child process */
    pid = fork();

    switch (pid) {
        case -1:
            /* Failed to create child process */
            perror("fork() failed: ");
            errors++;
            break;
        case 0:
            /* Inside child process */
            ret_val = execv(command[0], &command[1]);
            
            /* If process failed to execute return false for error */
            if (ret_val == -1) {
                perror("execv() failed: ");
                errors++;
                exit(EXIT_FAILURE);
            }
            break;
        default:
            /* Parent process, finish processing out of switch block */
    }

    /* Parent does cleanup of process, waits for child process to exit */
    if (wait(&wait_val) == -1) {
        perror("wait() failed: ");
        errors++;
    }

    /* Done with log now, close file */
    if (fd >= 0) {
        close(fd);
    }

    /* 
    * If we made it here the return val is a valid wait status. Check it to 
    * make sure that the command exited properly and did not return a non-zero 
    * value.
    */
    if (WIFEXITED(wait_val) && (errors == 0)) {
        return (WEXITSTATUS(wait_val) == EXIT_SUCCESS);
    }

    return false;
}
