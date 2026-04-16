#define _GNU_SOURCE
#include <sys/ioctl.h>
#include "monitor_ioctl.h" 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sched.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <signal.h>

#define SOCKET_PATH "/tmp/engine.sock"
#define STACK_SIZE (1024 * 1024)
#define BUFFER_SIZE 256
#define MAX_LOG_LEN 512
#define MAX_CONTAINERS 10

// --- TASK 3: BOUNDED-BUFFER LOGGING STRUCTURES ---
typedef struct {
    char container_id[64];
    char text[MAX_LOG_LEN];
} log_entry_t;

typedef struct {
    log_entry_t queue[BUFFER_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

bounded_buffer_t log_buffer;

// --- TASK 1: METADATA TRACKING ---
typedef struct {
    char id[64];
    pid_t host_pid;
    char state[32];
    int soft_mib;
    int hard_mib;
    char log_path[256];
} container_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;
pthread_mutex_t metadata_lock = PTHREAD_MUTEX_INITIALIZER;

// --- CONTAINER LAUNCH ARGUMENTS ---
typedef struct {
    char id[64];
    char rootfs[256];
    char cmd[256];
    int pipe_fd;
} child_args_t;

// --- LOGGING THREAD FUNCTIONS ---
void *consumer_thread_func(void *arg) {
    if (arg) {} // Silence unused parameter warning
    while(1) {
        pthread_mutex_lock(&log_buffer.lock);
        while (log_buffer.count == 0) {
            pthread_cond_wait(&log_buffer.not_empty, &log_buffer.lock);
        }

        log_entry_t entry = log_buffer.queue[log_buffer.head];
        log_buffer.head = (log_buffer.head + 1) % BUFFER_SIZE;
        log_buffer.count--;

        pthread_cond_signal(&log_buffer.not_full);
        pthread_mutex_unlock(&log_buffer.lock);

        char filepath[300];
        snprintf(filepath, sizeof(filepath), "%s.log", entry.container_id);
        FILE *fp = fopen(filepath, "a");
        if (fp) {
            fprintf(fp, "%s", entry.text);
            fclose(fp);
        }
    }
    return NULL;
}

void *producer_thread_func(void *arg) {
    child_args_t *cargs = (child_args_t *)arg;
    FILE *stream = fdopen(cargs->pipe_fd, "r");
    if (!stream) {
        free(cargs);
        return NULL;
    }

    char line[MAX_LOG_LEN];
    while (fgets(line, sizeof(line), stream) != NULL) {
        pthread_mutex_lock(&log_buffer.lock);
        while (log_buffer.count == BUFFER_SIZE) {
            pthread_cond_wait(&log_buffer.not_full, &log_buffer.lock);
        }

        snprintf(log_buffer.queue[log_buffer.tail].container_id, sizeof(log_buffer.queue[log_buffer.tail].container_id), "%s", cargs->id);
        snprintf(log_buffer.queue[log_buffer.tail].text, sizeof(log_buffer.queue[log_buffer.tail].text), "%s", line);
        
        log_buffer.tail = (log_buffer.tail + 1) % BUFFER_SIZE;
        log_buffer.count++;

        pthread_cond_signal(&log_buffer.not_empty);
        pthread_mutex_unlock(&log_buffer.lock);
    }
    fclose(stream);
    free(cargs);
    return NULL;
}

// --- CONTAINER NAMESPACE ENTRY POINT ---
int container_main(void *arg) {
    child_args_t *args = (child_args_t *)arg;

    if (dup2(args->pipe_fd, STDOUT_FILENO) < 0) return 1;
    if (dup2(args->pipe_fd, STDERR_FILENO) < 0) return 1;

    if (chroot(args->rootfs) != 0) {
        return 1;
    }
    if (chdir("/") != 0) {
        return 1;
    }
    
    mount("proc", "/proc", "proc", 0, NULL);

    char *exec_args[] = {args->cmd, NULL};
    execvp(exec_args[0], exec_args);
    
    return 1;
}

// --- MAIN FUNCTION ---
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: engine supervisor <base-rootfs> | engine start <id> <rootfs> <cmd> | engine ps\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        printf("[Supervisor] Starting up...\n");

        log_buffer.head = 0; log_buffer.tail = 0; log_buffer.count = 0;
        pthread_mutex_init(&log_buffer.lock, NULL);
        pthread_cond_init(&log_buffer.not_empty, NULL);
        pthread_cond_init(&log_buffer.not_full, NULL);

        pthread_t consumer_tid;
        pthread_create(&consumer_tid, NULL, consumer_thread_func, NULL);

        int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sun_family = AF_UNIX;
        snprintf(server_addr.sun_path, sizeof(server_addr.sun_path), "%s", SOCKET_PATH);
        
        unlink(SOCKET_PATH);
        if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("bind failed");
            return 1;
        }
        if (listen(server_fd, 5) < 0) {
            perror("listen failed");
            return 1;
        }
        printf("[Supervisor] Listening for commands on %s\n", SOCKET_PATH);

        while (1) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) continue;

            char buffer[512] = {0};
            if (read(client_fd, buffer, sizeof(buffer)) <= 0) {
                close(client_fd);
                continue;
            }
            
            if (strncmp(buffer, "ps", 2) == 0) {
                char response[2048] = "CONTAINER ID\tHOST PID\tSTATE\n----------------------------------------\n";
                pthread_mutex_lock(&metadata_lock);
                for (int i = 0; i < container_count; i++) {
                    char line[256];
                    snprintf(line, sizeof(line), "%.63s\t\t%d\t\t%.31s\n", 
                             containers[i].id, containers[i].host_pid, containers[i].state);
                    strncat(response, line, sizeof(response) - strlen(response) - 1);
                }
                pthread_mutex_unlock(&metadata_lock);
                if (write(client_fd, response, strlen(response)) < 0) {}
            }
            else if (strncmp(buffer, "start", 5) == 0) {
                char cmd_type[16], id[64], rootfs[256], exec_cmd[256];
                if (sscanf(buffer, "%15s %63s %255s %255s", cmd_type, id, rootfs, exec_cmd) == 4) {
                    int pipefd[2];
                    if (pipe(pipefd) < 0) {
                        if (write(client_fd, "Error: pipe failed.\n", 20) < 0) {}
                        close(client_fd);
                        continue;
                    }

                    child_args_t *cargs = malloc(sizeof(child_args_t));
                    snprintf(cargs->id, sizeof(cargs->id), "%s", id);
                    snprintf(cargs->rootfs, sizeof(cargs->rootfs), "%s", rootfs);
                    snprintf(cargs->cmd, sizeof(cargs->cmd), "%s", exec_cmd);
                    cargs->pipe_fd = pipefd[1];

                    char *stack = malloc(STACK_SIZE);
                    pid_t pid = clone(container_main, stack + STACK_SIZE, 
                                      CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, cargs);

                    if (pid > 0) {
                        close(pipefd[1]); // Supervisor closes write end

                        child_args_t *pargs = malloc(sizeof(child_args_t));
                        snprintf(pargs->id, sizeof(pargs->id), "%s", id);
                        pargs->pipe_fd = pipefd[0];
                        pthread_t producer_tid;
                        pthread_create(&producer_tid, NULL, producer_thread_func, pargs);

                        pthread_mutex_lock(&metadata_lock);
                        snprintf(containers[container_count].id, sizeof(containers[container_count].id), "%s", id);
                        containers[container_count].host_pid = pid;
                        snprintf(containers[container_count].state, sizeof(containers[container_count].state), "running");
                        container_count++;
                        pthread_mutex_unlock(&metadata_lock);

                        // --- TASK 4: REGISTER PID WITH KERNEL ---
                        int fd = open("/dev/container_monitor", O_WRONLY);
                        if (fd >= 0) {
                            monitor_cmd_t cmd_data;
                            cmd_data.pid = pid;
                            cmd_data.soft_limit = 40; 
                            cmd_data.hard_limit = 64; 
                            ioctl(fd, REGISTER_PROCESS, &cmd_data);
                            close(fd);
                        } else {
                            // Suppress error message cleanly if module isn't loaded yet during user-space testing
                        }

                        char response[256];
                        snprintf(response, sizeof(response), "Success: Container '%s' started with Host PID %d.\n", id, pid);
                        if (write(client_fd, response, strlen(response)) < 0) {}
                    } else {
                        if (write(client_fd, "Error: clone failed.\n", 21) < 0) {}
                    }
                }
            } else {
                if (write(client_fd, "Error: Unknown command.\n", 24) < 0) {}
            }
            close(client_fd);
        }
    } 
    else {
        int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sun_family = AF_UNIX;
        snprintf(server_addr.sun_path, sizeof(server_addr.sun_path), "%s", SOCKET_PATH);

        if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            fprintf(stderr, "[CLI] Error: Could not connect to supervisor. Is it running?\n");
            return 1;
        }

        char command[512] = {0};
        for (int i = 1; i < argc; i++) {
            strncat(command, argv[i], sizeof(command) - strlen(command) - 2);
            strncat(command, " ", sizeof(command) - strlen(command) - 1);
        }

        if (write(sock_fd, command, strlen(command)) < 0) {}

        char response[2048] = {0};
        if (read(sock_fd, response, sizeof(response)) < 0) {}
        printf("%s", response);

        close(sock_fd);
    }

    return 0;
}
