#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "container.h"
#include "filesystem.h"
#include "logger.h"
#include "monitor.h"
#include "network.h"
#include "namespace.h"
#include "resource.h"
#include "scheduler.h"

#define STACK_SIZE (1024 * 1024)
#define METADATA_FILE "containers.meta"
#define METADATA_FILE_TMP "containers.meta.tmp"
#define DEFAULT_CONTAINER_COMMAND "/bin/sh"
#define DETAIL_KEY_WIDTH 12
#define DETAIL_VALUE_WIDTH 60
#define INVENTORY_COLUMN_COUNT 8
#define STATS_COLUMN_COUNT 9

static const int DETAIL_WIDTHS[] = {DETAIL_KEY_WIDTH, DETAIL_VALUE_WIDTH};
static const int INVENTORY_WIDTHS[INVENTORY_COLUMN_COUNT] = {14, 12, 8, 7, 10, 18, 16, 22};
static const int STATS_WIDTHS[STATS_COLUMN_COUNT] = {14, 7, 5, 8, 7, 8, 8, 5, 18};
static const char *const INVENTORY_HEADERS[INVENTORY_COLUMN_COUNT] = {
    "ID", "NAME", "STATE", "PID", "HOST", "ROOTFS", "COMMAND", "LIMITS"
};
static const char *const STATS_HEADERS[STATS_COLUMN_COUNT] = {
    "ID", "PID", "STATE", "CPU(s)", "CPU(%)", "RSS(MB)", "VSZ(MB)", "THR", "COMMAND"
};

static Container *head = NULL;
static Container *tail = NULL;
static int next_sequence = 1;
static volatile sig_atomic_t g_interrupt_requested = 0;

static const char *safe_text(const char *text) {
    return (text != NULL) ? text : "";
}

static void print_cli_repeat(const char *glyph, int count) {
    for (int i = 0; i < count; i++) {
        fputs(glyph, stdout);
    }
}

static int table_inner_width(const int *widths, size_t count) {
    int total = 0;

    for (size_t i = 0; i < count; i++) {
        total += widths[i] + 2;
    }

    if (count > 0) {
        total += (int)(count - 1);
    }

    return total;
}

static void print_cli_full_border(int inner_width, const char *left, const char *right) {
    fputs(left, stdout);
    print_cli_repeat("─", inner_width + 2);
    fputs(right, stdout);
    printf("\n");
}

static void print_cli_full_row(int inner_width, const char *text) {
    printf("│ %-*.*s │\n",
           inner_width,
           inner_width,
           safe_text(text));
}

static void print_cli_table_border(const int *widths,
                                   size_t count,
                                   const char *left,
                                   const char *middle,
                                   const char *right) {
    fputs(left, stdout);

    for (size_t i = 0; i < count; i++) {
        print_cli_repeat("─", widths[i] + 2);
        fputs((i + 1 < count) ? middle : right, stdout);
    }

    printf("\n");
}

static void print_cli_table_row(const char *const *values, const int *widths, size_t count) {
    fputs("│", stdout);

    for (size_t i = 0; i < count; i++) {
        printf(" %-*.*s │",
               widths[i],
               widths[i],
               safe_text((values != NULL) ? values[i] : NULL));
    }

    printf("\n");
}

static void print_detail_box_header(const char *title) {
    int inner_width = table_inner_width(DETAIL_WIDTHS, sizeof(DETAIL_WIDTHS) / sizeof(DETAIL_WIDTHS[0]));

    print_cli_full_border(inner_width, "╭", "╮");
    print_cli_full_row(inner_width, title);
    print_cli_table_border(DETAIL_WIDTHS,
                           sizeof(DETAIL_WIDTHS) / sizeof(DETAIL_WIDTHS[0]),
                           "├",
                           "┬",
                           "┤");
}

static void print_detail_row(const char *label, const char *value) {
    const char *values[] = {label, value};

    print_cli_table_row(values,
                        DETAIL_WIDTHS,
                        sizeof(DETAIL_WIDTHS) / sizeof(DETAIL_WIDTHS[0]));
}

static void print_detail_box_footer(void) {
    print_cli_table_border(DETAIL_WIDTHS,
                           sizeof(DETAIL_WIDTHS) / sizeof(DETAIL_WIDTHS[0]),
                           "╰",
                           "┴",
                           "╯");
}

static void print_table_message(const int *widths, size_t count, const char *message) {
    int inner_width = table_inner_width(widths, count);

    print_cli_full_row(inner_width, message);
}

static void print_inventory_table_header(void) {
    print_cli_table_border(INVENTORY_WIDTHS, INVENTORY_COLUMN_COUNT, "╭", "┬", "╮");
    print_cli_table_row(INVENTORY_HEADERS, INVENTORY_WIDTHS, INVENTORY_COLUMN_COUNT);
    print_cli_table_border(INVENTORY_WIDTHS, INVENTORY_COLUMN_COUNT, "├", "┼", "┤");
}

static void print_inventory_table_footer(void) {
    print_cli_table_border(INVENTORY_WIDTHS, INVENTORY_COLUMN_COUNT, "╰", "┴", "╯");
}

static void print_stats_footer(void) {
    print_cli_table_border(STATS_WIDTHS, STATS_COLUMN_COUNT, "╰", "┴", "╯");
}

void container_request_interrupt(void) {
    g_interrupt_requested = 1;
}

int container_consume_interrupt(void) {
    if (g_interrupt_requested) {
        g_interrupt_requested = 0;
        return 1;
    }
    return 0;
}

static const char *state_to_string(ContainerState state) {
    switch (state) {
        case STATE_CREATED:
            return "CREATED";
        case STATE_RUNNING:
            return "RUNNING";
        case STATE_STOPPED:
            return "STOPPED";
        default:
            return "UNKNOWN";
    }
}

static ContainerState string_to_state(const char *value) {
    if (strcmp(value, "RUNNING") == 0) {
        return STATE_RUNNING;
    }
    if (strcmp(value, "STOPPED") == 0) {
        return STATE_STOPPED;
    }
    return STATE_CREATED;
}

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static void trim_newline(char *line) {
    if (line == NULL) {
        return;
    }
    line[strcspn(line, "\r\n")] = '\0';
}

static void append_container(Container *container) {
    container->next = NULL;
    if (tail == NULL) {
        head = tail = container;
        return;
    }
    tail->next = container;
    tail = container;
}

static void remove_container(Container *container) {
    Container *prev = NULL;

    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        if (cursor != container) {
            prev = cursor;
            continue;
        }

        if (prev == NULL) {
            head = cursor->next;
        } else {
            prev->next = cursor->next;
        }

        if (tail == cursor) {
            tail = prev;
        }
        cursor->next = NULL;
        return;
    }
}

static Container *find_container(const char *id) {
    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        if (strcmp(cursor->id, id) == 0) {
            return cursor;
        }
    }
    return NULL;
}

static void free_container_stack(Container *container) {
    if (container->stack != NULL) {
        free(container->stack);
        container->stack = NULL;
    }
}

static int save_metadata(void) {
    FILE *file = fopen(METADATA_FILE_TMP, "w");
    if (file == NULL) {
        printf("[error] failed to write container metadata\n\n");
        log_event_type("ERROR", "metadata save failed: could not open temp file");
        return -1;
    }

    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        fprintf(file, "%s\t%s\t%d\t%s\t%s\t%s\t%s\t%u\t%u\t%u\n",
                cursor->id,
                cursor->name,
                (int)cursor->pid,
                cursor->hostname,
                cursor->rootfs,
                state_to_string(cursor->state),
                cursor->command_line,
                cursor->resource_limits.cpu_seconds,
                cursor->resource_limits.memory_mb,
                cursor->resource_limits.max_processes);
    }

    if (fclose(file) != 0) {
        printf("[error] failed to flush container metadata\n\n");
        log_event_type("ERROR", "metadata save failed: fclose");
        return -1;
    }

    if (rename(METADATA_FILE_TMP, METADATA_FILE) != 0) {
        printf("[error] failed to finalize container metadata\n\n");
        log_event_type("ERROR", "metadata save failed: rename");
        return -1;
    }

    return 0;
}

static void update_next_sequence(const char *id) {
    int value = 0;

    if (sscanf(id, "container-%d", &value) == 1 && value >= next_sequence) {
        next_sequence = value + 1;
    }
}

static int is_pid_alive(pid_t pid) {
    if (pid <= 0) {
        return 0;
    }

    if (kill(pid, 0) == 0) {
        return 1;
    }

    return errno == EPERM;
}

static void format_wait_status(int status, char *buffer, size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    if (WIFEXITED(status)) {
        snprintf(buffer, buffer_size, "exited code=%d", WEXITSTATUS(status));
        return;
    }
    if (WIFSIGNALED(status)) {
        snprintf(buffer, buffer_size, "killed signal=%d", WTERMSIG(status));
        return;
    }

    snprintf(buffer, buffer_size, "status=%d", status);
}

static void log_container_exit_status(const char *container_id, const char *prefix, int status) {
    char reason[64];

    if (container_id == NULL || prefix == NULL) {
        return;
    }

    format_wait_status(status, reason, sizeof(reason));
    if (WIFSIGNALED(status)) {
        int signal_number = WTERMSIG(status);
        if (signal_number == SIGXCPU) {
            log_event_type("RESOURCE_LIMIT_HIT", "%s %s (%s)", container_id, prefix, reason);
            return;
        }
        log_event_type("CONTAINER_STOPPED", "%s %s (%s)", container_id, prefix, reason);
        return;
    }

    log_event_type("CONTAINER_STOPPED", "%s %s (%s)", container_id, prefix, reason);
}

static int sync_container_state(Container *container) {
    int state_changed = 0;
    int status = 0;
    pid_t result = 0;

    if (container->state != STATE_RUNNING || container->pid <= 0) {
        return 0;
    }

    result = waitpid(container->pid, &status, WNOHANG);
    if (result > 0) {
        pid_t old_pid = container->pid;

        container->state = STATE_STOPPED;
        container->pid = -1;
        free_container_stack(container);
        container_scheduler_on_stopped(old_pid);
        log_container_exit_status(container->id, "reaped by manager", status);
        return 1;
    }

    if (result == 0) {
        return 0;
    }

    if (errno == ECHILD && !is_pid_alive(container->pid)) {
        container->state = STATE_STOPPED;
        container->pid = -1;
        free_container_stack(container);
        log_event_type("CONTAINER_STOPPED", "%s marked stopped after recovery check", container->id);
        state_changed = 1;
    }

    return state_changed;
}

static void poll_states(void) {
    int changed = 0;

    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        changed |= sync_container_state(cursor);
    }

    if (changed) {
        save_metadata();
    }
}

void container_refresh_state(void) {
    poll_states();
}

static int normalize_resource_limits(ResourceConfig *limits) {
    if (limits == NULL) {
        return -1;
    }

    return 0;
}

static int move_terminal_foreground(pid_t pgrp) {
    void (*previous_sigttou)(int) = SIG_ERR;
    void (*previous_sigttin)(int) = SIG_ERR;
    void (*previous_sigtstp)(int) = SIG_ERR;

    if (!isatty(STDIN_FILENO)) {
        return 0;
    }

    previous_sigttou = signal(SIGTTOU, SIG_IGN);
    previous_sigttin = signal(SIGTTIN, SIG_IGN);
    previous_sigtstp = signal(SIGTSTP, SIG_IGN);

    if (tcsetpgrp(STDIN_FILENO, pgrp) != 0) {
        int saved_errno = errno;

        if (previous_sigttou != SIG_ERR) {
            signal(SIGTTOU, previous_sigttou);
        }
        if (previous_sigttin != SIG_ERR) {
            signal(SIGTTIN, previous_sigttin);
        }
        if (previous_sigtstp != SIG_ERR) {
            signal(SIGTSTP, previous_sigtstp);
        }
        errno = saved_errno;
        return -1;
    }

    if (previous_sigttou != SIG_ERR) {
        signal(SIGTTOU, previous_sigttou);
    }
    if (previous_sigttin != SIG_ERR) {
        signal(SIGTTIN, previous_sigttin);
    }
    if (previous_sigtstp != SIG_ERR) {
        signal(SIGTSTP, previous_sigtstp);
    }

    return 0;
}

static int wait_for_container_process(Container *container, int quiet) {
    int status = 0;
    pid_t shell_pgrp = getpgrp();
    int terminal_moved = 0;

    if (container == NULL || container->state != STATE_RUNNING || container->pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    if (move_terminal_foreground(container->pid) == 0) {
        terminal_moved = 1;
    }

    if (waitpid(container->pid, &status, 0) < 0) {
        if (terminal_moved) {
            (void)move_terminal_foreground(shell_pgrp);
        }
        if (errno == ECHILD && !is_pid_alive(container->pid)) {
            status = 0;
        } else {
            return -1;
        }
    }

    if (terminal_moved) {
        (void)move_terminal_foreground(shell_pgrp);
    }

    log_container_exit_status(container->id, "finished", status);
    container->pid = -1;
    container->state = STATE_STOPPED;
    free_container_stack(container);

    if (save_metadata() != 0) {
        return -1;
    }

    if (!quiet) {
        printf("[manager] %s finished\n\n", container->id);
    }

    return 0;
}

static int stop_container_process(Container *container, int quiet) {
    if (container->state != STATE_RUNNING || container->pid <= 0) {
        if (!quiet) {
            printf("[manager] %s is not running\n\n", container->id);
        }
        return -1;
    }

    if (kill(container->pid, SIGKILL) != 0 && errno != ESRCH) {
        if (!quiet) {
            printf("[error] failed to stop %s\n\n", container->id);
        }
        log_event_type("ERROR", "failed to stop %s (pid %d)", container->id, container->pid);
        return -1;
    }

    if (waitpid(container->pid, NULL, 0) < 0 && errno != ECHILD) {
        if (!quiet) {
            printf("[error] failed to reap %s\n\n", container->id);
        }
        log_event_type("ERROR", "failed to reap %s (pid %d)", container->id, container->pid);
        return -1;
    }

    log_event_type("CONTAINER_STOPPED", "%s stopped (pid %d)", container->id, container->pid);
    container_scheduler_on_stopped(container->pid);
    container->pid = -1;
    container->state = STATE_STOPPED;
    free_container_stack(container);
    save_metadata();

    if (!quiet) {
        printf("[manager] %s stopped\n\n", container->id);
    }

    return 0;
}

static void print_container_banner(const Container *container, pid_t namespace_pid) {
    char pid_text[16];
    char pid_value[48];
    char limits_text[128];

    if (container->pid > 0) {
        snprintf(pid_text, sizeof(pid_text), "%d", (int)container->pid);
    } else {
        copy_string(pid_text, sizeof(pid_text), "-");
    }

    printf("\n");
    print_detail_box_header("Container Summary");
    print_detail_row("id", container->id);
    print_detail_row("state", state_to_string(container->state));
    print_detail_row("name", container->name);
    print_detail_row("hostname", container->hostname);
    if (namespace_pid > 0) {
        snprintf(pid_value, sizeof(pid_value), "host pid %s | ns pid %d", pid_text, (int)namespace_pid);
        print_detail_row("pid", pid_value);
    } else {
        print_detail_row("pid", pid_text);
    }
    print_detail_row("rootfs", container->rootfs);
    print_detail_row("command", container->command_line);
    resource_format_limits(&container->resource_limits, limits_text, sizeof(limits_text));
    print_detail_row("limits", limits_text);
    print_detail_row("isolate", namespace_profile());
    print_detail_row("fs mode", filesystem_profile());
    print_detail_row("net mode", network_profile());
    print_detail_row("res mode", resource_profile());
    print_detail_box_footer();
    printf("\n");
}

static void print_start_error(const Container *container, int error_number) {
    char message[256];

    namespace_format_start_error(error_number, message, sizeof(message));
    printf("[error] failed to start %s with isolation setup\n", container->id);
    printf("[hint] rootfs: %s\n", container->rootfs);
    printf("[hint] %s\n\n", message);
}

static void format_start_error_message(int error_number, char *buffer, size_t buffer_size) {
    namespace_format_start_error(error_number, buffer, buffer_size);
}

int container_manager_init(void) {
    FILE *file = fopen(METADATA_FILE, "r");
    char line[1024];
    int restored = 0;

    if (file == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *fields[10];
        char *token = NULL;
        Container *container = NULL;
        int index = 0;

        trim_newline(line);
        token = strtok(line, "\t");
        while (token != NULL && index < 10) {
            fields[index++] = token;
            token = strtok(NULL, "\t");
        }

        if (index != 6 && index != 7 && index != 10) {
            continue;
        }

        container = calloc(1, sizeof(*container));
        if (container == NULL) {
            fclose(file);
            printf("[error] out of memory while restoring containers\n\n");
            return -1;
        }

        copy_string(container->id, sizeof(container->id), fields[0]);
        copy_string(container->name, sizeof(container->name), fields[1]);
        container->pid = (pid_t)atoi(fields[2]);
        copy_string(container->hostname, sizeof(container->hostname), fields[3]);
        copy_string(container->rootfs, sizeof(container->rootfs), fields[4]);
        container->state = string_to_state(fields[5]);
        if (index >= 7) {
            copy_string(container->command_line, sizeof(container->command_line), fields[6]);
        } else {
            copy_string(container->command_line, sizeof(container->command_line), DEFAULT_CONTAINER_COMMAND);
        }
        if (index >= 10) {
            container->resource_limits.cpu_seconds = (unsigned int)strtoul(fields[7], NULL, 10);
            container->resource_limits.memory_mb = (unsigned int)strtoul(fields[8], NULL, 10);
            container->resource_limits.max_processes = (unsigned int)strtoul(fields[9], NULL, 10);
        } else {
            memset(&container->resource_limits, 0, sizeof(container->resource_limits));
        }
        (void)normalize_resource_limits(&container->resource_limits);
        container->stack = NULL;

        if (container->state == STATE_RUNNING && !is_pid_alive(container->pid)) {
            container->state = STATE_STOPPED;
            container->pid = -1;
        }

        append_container(container);
        update_next_sequence(container->id);
        restored++;
    }

    fclose(file);
    poll_states();

    if (restored > 0) {
        printf("[manager] restored %d container record(s) from %s\n\n",
               restored,
               METADATA_FILE);
        log_event_type("MANAGER", "restored %d container record(s)", restored);
    }

    return 0;
}

int container_create(const ContainerSpec *spec, char *out_id, size_t out_id_size) {
    Container *container = calloc(1, sizeof(*container));
    char requested_rootfs[CONTAINER_ROOTFS_LEN];
    int saved_errno = 0;

    if (container == NULL) {
        printf("[error] out of memory\n\n");
        return -1;
    }

    snprintf(container->id, sizeof(container->id), "container-%04d", next_sequence++);

    if (spec != NULL && spec->name != NULL && spec->name[0] != '\0') {
        copy_string(container->name, sizeof(container->name), spec->name);
    } else {
        copy_string(container->name, sizeof(container->name), container->id);
    }

    if (spec != NULL && spec->hostname != NULL && spec->hostname[0] != '\0') {
        copy_string(container->hostname, sizeof(container->hostname), spec->hostname);
    } else {
        copy_string(container->hostname, sizeof(container->hostname), container->name);
    }

    if (spec != NULL && spec->rootfs != NULL && spec->rootfs[0] != '\0') {
        copy_string(requested_rootfs, sizeof(requested_rootfs), spec->rootfs);
    } else {
        snprintf(requested_rootfs, sizeof(requested_rootfs), "./rootfs/%s", container->id);
    }

    if (spec != NULL && spec->command_line != NULL && spec->command_line[0] != '\0') {
        copy_string(container->command_line, sizeof(container->command_line), spec->command_line);
    } else {
        copy_string(container->command_line, sizeof(container->command_line), DEFAULT_CONTAINER_COMMAND);
    }

    memset(&container->resource_limits, 0, sizeof(container->resource_limits));
    if (spec != NULL) {
        container->resource_limits = spec->resource_limits;
        (void)normalize_resource_limits(&container->resource_limits);
    }

    if (filesystem_prepare_rootfs(requested_rootfs, container->rootfs, sizeof(container->rootfs)) != 0) {
        saved_errno = errno;
        printf("[error] failed to prepare rootfs for %s\n", container->id);
        printf("[hint] %s\n\n", strerror(saved_errno));
        free(container);
        return -1;
    }

    container->pid = -1;
    container->state = STATE_CREATED;
    append_container(container);

    if (save_metadata() != 0) {
        remove_container(container);
        free(container);
        return -1;
    }

    if (out_id != NULL && out_id_size > 0) {
        copy_string(out_id, out_id_size, container->id);
    }

    printf("[manager] created %s\n", container->id);
    print_container_banner(container, -1);
    log_event_type("CONTAINER_CREATED",
                   "%s name=%s hostname=%s rootfs=%s command=%s cpu=%us mem=%uMB nproc=%u",
                   container->id,
                   container->name,
                   container->hostname,
                   container->rootfs,
                   container->command_line,
                   container->resource_limits.cpu_seconds,
                   container->resource_limits.memory_mb,
                   container->resource_limits.max_processes);
    return 0;
}

void container_scheduler_on_started(pid_t pid) {
    (void)scheduler_add_target(pid);
}

void container_scheduler_on_stopped(pid_t pid) {
    (void)scheduler_remove_target(pid);
}

void container_scheduler_refresh_targets(void) {
    pid_t first = -1;
    int enabled = scheduler_is_enabled();

    scheduler_clear_targets();
    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        if (cursor->state == STATE_RUNNING && cursor->pid > 0) {
            if (first < 0) {
                first = cursor->pid;
            }
            (void)scheduler_add_target(cursor->pid);
        }
    }

    if (enabled && first > 0) {
        /* Start RR from a stable point: stop others and resume the first. */
        for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
            if (cursor->state == STATE_RUNNING && cursor->pid > 0 && cursor->pid != first) {
                (void)kill(cursor->pid, SIGSTOP);
            }
        }
        (void)kill(first, SIGCONT);
    } else if (!enabled) {
        /* Make sure no container is left paused when scheduling is off. */
        for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
            if (cursor->state == STATE_RUNNING && cursor->pid > 0) {
                (void)kill(cursor->pid, SIGCONT);
            }
        }
    }
}

static int start_container_by_id(const char *id, int schedule_target) {
    Container *container = NULL;
    NamespaceConfig namespace_config;
    NamespaceStartResult start_result;
    int saved_errno = 0;

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: start <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    if (container->state == STATE_RUNNING) {
        printf("[manager] %s is already running\n\n", container->id);
        return -1;
    }

    container->stack = malloc(STACK_SIZE);
    if (container->stack == NULL) {
        printf("[error] out of memory\n\n");
        return -1;
    }

    memset(&namespace_config, 0, sizeof(namespace_config));
    memset(&start_result, 0, sizeof(start_result));
    namespace_config.hostname = container->hostname;
    namespace_config.rootfs = container->rootfs;
    namespace_config.command_line = container->command_line;
    namespace_config.resource_limits = container->resource_limits;

    if (namespace_start_container(&namespace_config,
                                  container->stack,
                                  STACK_SIZE,
                                  &start_result) != 0) {
        char message[256];

        saved_errno = errno;
        print_start_error(container, saved_errno);
        format_start_error_message(saved_errno, message, sizeof(message));
        log_event_type("ERROR", "startup isolation failed for %s: %s", container->id, message);
        free_container_stack(container);
        return -1;
    }

    container->pid = start_result.host_pid;
    container->state = STATE_RUNNING;

    if (save_metadata() != 0) {
        stop_container_process(container, 1);
        return -1;
    }

    printf("[manager] started %s\n", container->id);
    print_container_banner(container, start_result.namespace_pid);
    log_event_type("CONTAINER_STARTED",
                   "%s pid=%d ns_pid=%d isolation=%s command=%s cpu=%us mem=%uMB nproc=%u",
                   container->id,
                   container->pid,
                   start_result.namespace_pid,
                   namespace_profile(),
                   container->command_line,
                   container->resource_limits.cpu_seconds,
                   container->resource_limits.memory_mb,
                   container->resource_limits.max_processes);

    if (schedule_target) {
        container_scheduler_on_started(container->pid);
    }

    return 0;
}

int container_run(const ContainerSpec *spec, char *out_id, size_t out_id_size) {
    char container_id[CONTAINER_ID_LEN];
    Container *container = NULL;

    if (container_create(spec, container_id, sizeof(container_id)) != 0) {
        return -1;
    }

    if (start_container_by_id(container_id, 0) != 0) {
        return -1;
    }

    if (out_id != NULL && out_id_size > 0) {
        copy_string(out_id, out_id_size, container_id);
    }

    container = find_container(container_id);
    if (container == NULL) {
        errno = ESRCH;
        return -1;
    }

    return wait_for_container_process(container, 0);
}

int container_run_background(const ContainerSpec *spec, char *out_id, size_t out_id_size) {
    char container_id[CONTAINER_ID_LEN];

    if (container_create(spec, container_id, sizeof(container_id)) != 0) {
        return -1;
    }

    if (start_container_by_id(container_id, 1) != 0) {
        return -1;
    }

    if (out_id != NULL && out_id_size > 0) {
        copy_string(out_id, out_id_size, container_id);
    }

    return 0;
}

int container_start(const char *id) {
    return start_container_by_id(id, 1);
}

int container_stop(const char *id) {
    Container *container = NULL;

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: stop <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    return stop_container_process(container, 0);
}

int container_delete(const char *id) {
    Container *container = NULL;

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: delete <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    if (container->state == STATE_RUNNING) {
        printf("[error] stop %s before deleting it\n\n", container->id);
        return -1;
    }

    remove_container(container);
    if (save_metadata() != 0) {
        append_container(container);
        return -1;
    }

    log_event_type("CONTAINER_DELETED", "%s deleted", container->id);
    printf("[manager] deleted %s\n\n", container->id);
    free_container_stack(container);
    free(container);
    return 0;
}

int container_list(void) {
    int count = 0;

    poll_states();

    printf("\n");
    print_detail_box_header("Container Inventory");
    print_detail_row("Isolation", namespace_profile());
    print_detail_row("Filesystem", filesystem_profile());
    print_detail_row("Network", network_profile());
    print_detail_row("Resources", resource_profile());
    print_detail_box_footer();
    print_inventory_table_header();

    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        char pid_text[16];
        if (cursor->pid > 0) {
            snprintf(pid_text, sizeof(pid_text), "%d", (int)cursor->pid);
        } else {
            copy_string(pid_text, sizeof(pid_text), "-");
        }
        char limits_text[128];
        const char *row[INVENTORY_COLUMN_COUNT];

        resource_format_limits(&cursor->resource_limits, limits_text, sizeof(limits_text));
        row[0] = cursor->id;
        row[1] = cursor->name;
        row[2] = state_to_string(cursor->state);
        row[3] = pid_text;
        row[4] = cursor->hostname;
        row[5] = cursor->rootfs;
        row[6] = cursor->command_line;
        row[7] = limits_text;
        print_cli_table_row(row, INVENTORY_WIDTHS, INVENTORY_COLUMN_COUNT);
        count++;
    }

    if (count == 0) {
        print_table_message(INVENTORY_WIDTHS, INVENTORY_COLUMN_COUNT, "no containers found");
    }

    print_inventory_table_footer();
    printf("\n");
    return 0;
}

static void print_stats_header(void) {
    char profile_text[160];
    int inner_width = table_inner_width(STATS_WIDTHS, STATS_COLUMN_COUNT);

    printf("\n");
    print_cli_full_border(inner_width, "╭", "╮");
    print_cli_full_row(inner_width, "Container Monitoring");
    print_cli_full_border(inner_width, "├", "┤");
    snprintf(profile_text, sizeof(profile_text), "Monitor profile: %s", monitor_profile());
    print_cli_full_row(inner_width, profile_text);
    print_cli_table_border(STATS_WIDTHS, STATS_COLUMN_COUNT, "├", "┬", "┤");
    print_cli_table_row(STATS_HEADERS, STATS_WIDTHS, STATS_COLUMN_COUNT);
    print_cli_table_border(STATS_WIDTHS, STATS_COLUMN_COUNT, "├", "┼", "┤");
}

static void print_stats_row(const Container *container, const MonitorStats *stats, int has_cpu_pct, double cpu_pct) {
    double rss_mb = 0.0;
    double vsize_mb = 0.0;
    char pid_text[16];
    char state_text[8];
    char cpu_text[16];
    char cpu_pct_text[16];
    char rss_text[16];
    char vsize_text[16];
    char thread_text[16];
    const char *row[STATS_COLUMN_COUNT];

    if (container == NULL || stats == NULL) {
        return;
    }

    rss_mb = (double)stats->rss_bytes / (1024.0 * 1024.0);
    vsize_mb = (double)stats->vsize_bytes / (1024.0 * 1024.0);
    snprintf(pid_text, sizeof(pid_text), "%d", (int)stats->pid);
    snprintf(state_text, sizeof(state_text), "%c", stats->state);
    snprintf(cpu_text, sizeof(cpu_text), "%.2f", stats->cpu_seconds);
    snprintf(rss_text, sizeof(rss_text), "%.1f", rss_mb);
    snprintf(vsize_text, sizeof(vsize_text), "%.1f", vsize_mb);
    snprintf(thread_text, sizeof(thread_text), "%ld", stats->threads);

    if (!has_cpu_pct) {
        snprintf(cpu_pct_text, sizeof(cpu_pct_text), "-");
    } else {
        snprintf(cpu_pct_text, sizeof(cpu_pct_text), "%.1f", cpu_pct);
    }

    row[0] = container->id;
    row[1] = pid_text;
    row[2] = state_text;
    row[3] = cpu_text;
    row[4] = cpu_pct_text;
    row[5] = rss_text;
    row[6] = vsize_text;
    row[7] = thread_text;
    row[8] = container->command_line;
    print_cli_table_row(row, STATS_WIDTHS, STATS_COLUMN_COUNT);
}

int container_stats(const char *id) {
    Container *container = NULL;
    MonitorStats stats;

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: stats <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    if (container->state != STATE_RUNNING || container->pid <= 0) {
        printf("[manager] %s is not running\n\n", container->id);
        return -1;
    }

    if (monitor_read(container->pid, &stats) != 0) {
        printf("[error] failed to read stats for %s (pid %d)\n\n", container->id, (int)container->pid);
        return -1;
    }

    print_stats_header();
    print_stats_row(container, &stats, 0, 0.0);
    print_stats_footer();
    printf("\n");
    return 0;
}

int container_stats_all(void) {
    int any = 0;

    poll_states();
    print_stats_header();

    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        MonitorStats stats;

        if (cursor->state != STATE_RUNNING || cursor->pid <= 0) {
            continue;
        }

        if (monitor_read(cursor->pid, &stats) != 0) {
            continue;
        }

        print_stats_row(cursor, &stats, 0, 0.0);
        any = 1;
    }

    if (!any) {
        print_table_message(STATS_WIDTHS, STATS_COLUMN_COUNT, "no running containers");
    }

    print_stats_footer();
    printf("\n");
    return 0;
}

typedef struct {
    pid_t pid;
    double cpu_seconds;
    unsigned long long wall_ns;
} CpuSample;

static unsigned long long monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

static int find_sample(CpuSample *samples, size_t count, pid_t pid) {
    for (size_t i = 0; i < count; i++) {
        if (samples[i].pid == pid) {
            return (int)i;
        }
    }
    return -1;
}

static void sleep_interval(unsigned int interval_sec) {
    struct timespec ts;
    ts.tv_sec = (time_t)interval_sec;
    ts.tv_nsec = 0;
    while (nanosleep(&ts, &ts) != 0) {
        if (errno == EINTR) {
            return;
        }
        return;
    }
}

static void clear_screen_if_tty(void) {
    if (!isatty(STDOUT_FILENO)) {
        return;
    }

    /* ANSI clear screen + cursor home. */
    fputs("\033[H\033[J", stdout);
}

int container_stats_watch(const char *id, unsigned int interval_sec) {
    CpuSample prev = {0};
    int has_prev = 0;

    if (interval_sec == 0) {
        printf("[error] usage: stats --watch <sec> [id]\n\n");
        return -1;
    }

    printf("[hint] watching every %us; press Ctrl+C to stop\n", interval_sec);

    while (1) {
        if (container_consume_interrupt()) {
            printf("\n");
            return 0;
        }

        Container *container = NULL;
        MonitorStats stats;
        unsigned long long now = 0;
        double cpu_pct = 0.0;
        int has_cpu_pct = 0;

        poll_states();
        container = find_container(id);
        if (container == NULL) {
            printf("\n[error] container %s not found\n\n", id);
            return -1;
        }

        if (container->state != STATE_RUNNING || container->pid <= 0) {
            printf("\n[manager] %s is not running\n\n", container->id);
            return -1;
        }

        if (monitor_read(container->pid, &stats) != 0) {
            printf("\n[error] failed to read stats for %s (pid %d)\n\n", container->id, (int)container->pid);
            return -1;
        }

        now = monotonic_ns();
        if (has_prev && prev.pid == stats.pid) {
            double delta_cpu = stats.cpu_seconds - prev.cpu_seconds;
            double delta_wall = (double)(now - prev.wall_ns) / 1e9;
            if (delta_wall > 0.0 && delta_cpu >= 0.0) {
                cpu_pct = (delta_cpu / delta_wall) * 100.0;
                has_cpu_pct = 1;
            }
        }

        clear_screen_if_tty();
        printf("[watch] interval=%us (Ctrl+C to stop)\n", interval_sec);
        print_stats_header();
        print_stats_row(container, &stats, has_cpu_pct, cpu_pct);
        print_stats_footer();
        printf("\n");

        prev.pid = stats.pid;
        prev.cpu_seconds = stats.cpu_seconds;
        prev.wall_ns = now;
        has_prev = 1;

        sleep_interval(interval_sec);
    }
}

int container_stats_all_watch(unsigned int interval_sec) {
    CpuSample *prev = NULL;
    size_t prev_count = 0;
    size_t prev_capacity = 0;

    if (interval_sec == 0) {
        printf("[error] usage: stats --watch <sec> [id]\n\n");
        return -1;
    }

    printf("[hint] watching every %us; press Ctrl+C to stop\n", interval_sec);

    while (1) {
        if (container_consume_interrupt()) {
            printf("\n");
            free(prev);
            return 0;
        }

        unsigned long long now = monotonic_ns();
        int any = 0;

        poll_states();
        clear_screen_if_tty();
        printf("[watch] interval=%us (Ctrl+C to stop)\n", interval_sec);
        print_stats_header();

        for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
            MonitorStats stats;
            double cpu_pct = 0.0;
            int has_cpu_pct = 0;
            int index = -1;

            if (cursor->state != STATE_RUNNING || cursor->pid <= 0) {
                continue;
            }

            if (monitor_read(cursor->pid, &stats) != 0) {
                continue;
            }

            index = find_sample(prev, prev_count, stats.pid);
            if (index >= 0) {
                double delta_cpu = stats.cpu_seconds - prev[index].cpu_seconds;
                double delta_wall = (double)(now - prev[index].wall_ns) / 1e9;
                if (delta_wall > 0.0 && delta_cpu >= 0.0) {
                    cpu_pct = (delta_cpu / delta_wall) * 100.0;
                    has_cpu_pct = 1;
                }

                prev[index].cpu_seconds = stats.cpu_seconds;
                prev[index].wall_ns = now;
            } else {
                if (prev_count == prev_capacity) {
                    size_t next_capacity = (prev_capacity == 0) ? 8 : prev_capacity * 2;
                    CpuSample *next = realloc(prev, next_capacity * sizeof(*next));
                    if (next == NULL) {
                        free(prev);
                        printf("\n[error] out of memory\n\n");
                        return -1;
                    }
                    prev = next;
                    prev_capacity = next_capacity;
                }

                prev[prev_count].pid = stats.pid;
                prev[prev_count].cpu_seconds = stats.cpu_seconds;
                prev[prev_count].wall_ns = now;
                prev_count++;
            }

            print_stats_row(cursor, &stats, has_cpu_pct, cpu_pct);
            any = 1;
        }

        if (!any) {
            print_table_message(STATS_WIDTHS, STATS_COLUMN_COUNT, "no running containers");
        }

        print_stats_footer();
        printf("\n");
        sleep_interval(interval_sec);
    }
}

void cleanup_all_containers(void) {
    Container *cursor = head;

    poll_states();

    while (cursor != NULL) {
        if (cursor->state == STATE_RUNNING) {
            stop_container_process(cursor, 1);
        }
        cursor = cursor->next;
    }

    save_metadata();

    cursor = head;
    while (cursor != NULL) {
        Container *next = cursor->next;
        free_container_stack(cursor);
        free(cursor);
        cursor = next;
    }

    head = NULL;
    tail = NULL;

    scheduler_stop();
}
