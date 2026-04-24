/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 * COMPLETE IMPLEMENTATION
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* global supervisor context pointer for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ----------------------------------------------------------------
 * BOUNDED BUFFER - producer/consumer for log chunks
 * ---------------------------------------------------------------- */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));
    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); return rc; }
    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/* PRODUCER: container reader threads call this to add a log chunk */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* wait while buffer is full, unless shutting down */
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    /* insert at tail */
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    /* wake up the logging consumer thread */
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* CONSUMER: logging thread calls this to get the next log chunk */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* wait while buffer is empty, unless shutting down */
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    /* if shutting down and nothing left, signal done */
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    /* remove from head */
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    /* wake up any waiting producers */
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ----------------------------------------------------------------
 * LOGGING THREAD - consumes log chunks and writes to files
 * ---------------------------------------------------------------- */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    char log_path[PATH_MAX];
    int fd;

    /* make sure logs directory exists */
    mkdir(LOG_DIR, 0755);

    while (1) {
        /* block until a log chunk is available */
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0)
            break; /* shutdown signal */

        /* build log file path: logs/<container_id>.log */
        snprintf(log_path, sizeof(log_path), "%s/%s.log",
                 LOG_DIR, item.container_id);

        /* append the chunk to the log file */
        fd = open(log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }

    return NULL;
}

/* ----------------------------------------------------------------
 * LOG READER THREAD ARG - one per container pipe
 * ---------------------------------------------------------------- */
typedef struct {
    int pipe_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} log_reader_arg_t;

/* reads from container pipe and pushes chunks into bounded buffer */
static void *log_reader_thread(void *arg)
{
    log_reader_arg_t *larg = (log_reader_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    while (1) {
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, larg->container_id,
                CONTAINER_ID_LEN - 1);

        n = read(larg->pipe_fd, item.data, LOG_CHUNK_SIZE - 1);
        if (n <= 0)
            break;

        item.length = (size_t)n;
        bounded_buffer_push(larg->log_buffer, &item);
    }

    close(larg->pipe_fd);
    free(larg);
    return NULL;
}

/* ----------------------------------------------------------------
 * CHILD FUNCTION - runs inside the container (after clone)
 * ---------------------------------------------------------------- */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    char *argv_exec[4];

    /* redirect stdout and stderr to the log pipe */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    /* set hostname to container id for UTS namespace */
    sethostname(cfg->id, strlen(cfg->id));

    /* chroot into the container's rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    /* change directory to / inside the container */
    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    /* mount /proc so ps and other tools work inside container */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        /* ignore if already mounted */
    }

    /* set nice value for scheduling priority */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* execute the command inside the container */
    argv_exec[0] = "/bin/sh";
    argv_exec[1] = "-c";
    argv_exec[2] = cfg->command;
    argv_exec[3] = NULL;

    execv("/bin/sh", argv_exec);
    perror("execv");
    return 1;
}

/* ----------------------------------------------------------------
 * MONITOR HELPERS
 * ---------------------------------------------------------------- */
int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd,
                            const char *container_id,
                            pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ----------------------------------------------------------------
 * CONTAINER METADATA HELPERS
 * ---------------------------------------------------------------- */
static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0)
            return c;
        c = c->next;
    }
    return NULL;
}

static container_record_t *add_container(supervisor_ctx_t *ctx,
                                         const char *id,
                                         pid_t pid,
                                         unsigned long soft,
                                         unsigned long hard)
{
    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) return NULL;
    strncpy(rec->id, id, CONTAINER_ID_LEN - 1);
    rec->host_pid = pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = soft;
    rec->hard_limit_bytes = hard;
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, id);

    /* prepend to list */
    rec->next = ctx->containers;
    ctx->containers = rec;
    return rec;
}

/* ----------------------------------------------------------------
 * SIGNAL HANDLERS
 * ---------------------------------------------------------------- */
static void sigchld_handler(int sig)
{
    (void)sig;
    int status;
    pid_t pid;

    /* reap all exited children to avoid zombies */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;

        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->state = CONTAINER_EXITED;
                    c->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    c->state = CONTAINER_KILLED;
                    c->exit_signal = WTERMSIG(status);
                }
                /* unregister from kernel monitor */
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd,
                                            c->id, pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

/* ----------------------------------------------------------------
 * SPAWN CONTAINER - used by supervisor to launch a container
 * ---------------------------------------------------------------- */
static pid_t spawn_container(supervisor_ctx_t *ctx,
                             const char *id,
                             const char *rootfs,
                             const char *command,
                             int nice_value,
                             unsigned long soft,
                             unsigned long hard)
{
    int pipefd[2];
    pid_t pid;
    char *stack;
    child_config_t *cfg;

    /* create pipe: container stdout/stderr -> supervisor */
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return -1;
    }

    /* allocate stack for clone */
    stack = malloc(STACK_SIZE);
    if (!stack) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    /* fill child config */
    cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        free(stack);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    strncpy(cfg->id, id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, rootfs, PATH_MAX - 1);
    strncpy(cfg->command, command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value = nice_value;
    cfg->log_write_fd = pipefd[1]; /* child writes to this end */

    /* clone with PID, UTS, and mount namespaces */
    pid = clone(child_fn,
                stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                cfg);

    /* parent closes write end of pipe */
    close(pipefd[1]);
    free(stack);

    if (pid < 0) {
        perror("clone");
        free(cfg);
        close(pipefd[0]);
        return -1;
    }

    free(cfg);

    /* start a log reader thread for this container's pipe */
    log_reader_arg_t *larg = calloc(1, sizeof(*larg));
    if (larg) {
        larg->pipe_fd = pipefd[0];
        strncpy(larg->container_id, id, CONTAINER_ID_LEN - 1);
        larg->log_buffer = &ctx->log_buffer;
        pthread_t tid;
        pthread_create(&tid, NULL, log_reader_thread, larg);
        pthread_detach(tid);
    } else {
        close(pipefd[0]);
    }

    /* add metadata */
    pthread_mutex_lock(&ctx->metadata_lock);
    add_container(ctx, id, pid, soft, hard);
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* register with kernel memory monitor */
    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, id, pid, soft, hard);

    fprintf(stdout, "Started container id=%s pid=%d\n", id, pid);
    return pid;
}

/* ----------------------------------------------------------------
 * HANDLE ONE CLIENT REQUEST - called in supervisor event loop
 * ---------------------------------------------------------------- */
static void handle_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;
    ssize_t n;

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));

    n = recv(client_fd, &req, sizeof(req), 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }

    switch (req.kind) {

    case CMD_START: {
        pid_t pid = spawn_container(ctx,
                                    req.container_id,
                                    req.rootfs,
                                    req.command,
                                    req.nice_value,
                                    req.soft_limit_bytes,
                                    req.hard_limit_bytes);
        if (pid < 0) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "Failed to start container %s", req.container_id);
        } else {
            resp.status = 0;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "Started container %s pid=%d", req.container_id, pid);
        }
        break;
    }

    case CMD_RUN: {
        pid_t pid = spawn_container(ctx,
                                    req.container_id,
                                    req.rootfs,
                                    req.command,
                                    req.nice_value,
                                    req.soft_limit_bytes,
                                    req.hard_limit_bytes);
        if (pid < 0) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "Failed to run container %s", req.container_id);
        } else {
            /* wait for container to finish */
            int status = 0;
            waitpid(pid, &status, 0);
            resp.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "Container %s exited with status %d",
                     req.container_id, resp.status);
        }
        break;
    }

    case CMD_PS: {
        char buf[4096];
        int off = 0;
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-16s %-8s %-10s %-12s\n",
                        "ID", "PID", "STATE", "STARTED");
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c && off < (int)sizeof(buf) - 64) {
            off += snprintf(buf + off, sizeof(buf) - off,
                            "%-16s %-8d %-10s %-12ld\n",
                            c->id, c->host_pid,
                            state_to_string(c->state),
                            (long)c->started_at);
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp.status = 0;
        strncpy(resp.message, buf, CONTROL_MESSAGE_LEN - 1);
        break;
    }

    case CMD_LOGS: {
        char log_path[PATH_MAX];
        snprintf(log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req.container_id);
        FILE *f = fopen(log_path, "r");
        if (!f) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "No logs for container %s", req.container_id);
        } else {
            resp.status = 0;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "See %s", log_path);
            fclose(f);
        }
        break;
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        if (c && (c->state == CONTAINER_RUNNING ||
                  c->state == CONTAINER_STARTING)) {
            kill(c->host_pid, SIGTERM);
            c->state = CONTAINER_STOPPED;
            resp.status = 0;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "Stopped container %s", req.container_id);
        } else {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "Container %s not found or not running",
                     req.container_id);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "Unknown command");
        break;
    }

    send(client_fd, &resp, sizeof(resp), 0);
    close(client_fd);
}

/* ----------------------------------------------------------------
 * SUPERVISOR MAIN LOOP
 * ---------------------------------------------------------------- */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* init metadata lock */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { perror("pthread_mutex_init"); return 1; }

    /* init bounded buffer for logging */
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { perror("bounded_buffer_init"); return 1; }

    /* make logs directory */
    mkdir(LOG_DIR, 0755);

    /* open kernel memory monitor device */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: cannot open /dev/container_monitor: %s\n",
                strerror(errno));

    /* create UNIX domain socket for CLI control */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 10) < 0) {
        perror("listen"); return 1;
    }

    /* install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* start logging consumer thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) { perror("pthread_create logger"); return 1; }

    fprintf(stdout,
            "Supervisor started. base-rootfs=%s socket=%s\n",
            rootfs, CONTROL_PATH);
    fflush(stdout);

    /* main event loop: accept CLI connections */
    while (!ctx.should_stop) {
        fd_set rfds;
        struct timeval tv;
        int ret;

        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        ret = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue; /* timeout, loop again */

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_client(&ctx, client_fd);
    }

    fprintf(stdout, "Supervisor shutting down...\n");

    /* stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING)
            kill(c->host_pid, SIGTERM);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* wait a moment for containers to exit */
    sleep(1);

    /* shutdown logging pipeline */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    /* free container records */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    if (ctx.server_fd >= 0) close(ctx.server_fd);
    unlink(CONTROL_PATH);

    fprintf(stdout, "Supervisor exited cleanly.\n");
    return 0;
}

/* ----------------------------------------------------------------
 * CLI CLIENT - sends a request to the running supervisor
 * ---------------------------------------------------------------- */
static int send_control_request(const control_request_t *req)
{
    int sock_fd;
    struct sockaddr_un addr;
    control_response_t resp;

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "Cannot connect to supervisor at %s. Is it running?\n",
                CONTROL_PATH);
        close(sock_fd);
        return 1;
    }

    send(sock_fd, req, sizeof(*req), 0);

    memset(&resp, 0, sizeof(resp));
    recv(sock_fd, &resp, sizeof(resp), 0);
    close(sock_fd);

    printf("%s\n", resp.message);
    return resp.status == 0 ? 0 : 1;
}

/* ----------------------------------------------------------------
 * CLI COMMAND HANDLERS
 * ---------------------------------------------------------------- */
static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ----------------------------------------------------------------
 * MAIN ENTRY POINT
 * ---------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
