#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "container.h"
#include "logger.h"

#define LOG_FILE "container.log"
#define LOG_TIMESTAMP_LEN 32
#define LOG_EVENT_LEN 64
#define LOG_MESSAGE_LEN 768
#define LOG_STATUS_LEN 16
#define LOG_COLUMN_COUNT 7
#define LOG_TIME_WIDTH 19
#define LOG_EVENT_WIDTH 18
#define LOG_ID_WIDTH 14
#define LOG_NAME_WIDTH 12
#define LOG_PID_WIDTH 7
#define LOG_STATUS_WIDTH 10
#define LOG_DETAILS_WIDTH 44

static const int LOG_TABLE_WIDTHS[LOG_COLUMN_COUNT] = {
    LOG_TIME_WIDTH,
    LOG_EVENT_WIDTH,
    LOG_ID_WIDTH,
    LOG_NAME_WIDTH,
    LOG_PID_WIDTH,
    LOG_STATUS_WIDTH,
    LOG_DETAILS_WIDTH,
};

static const char *const LOG_TABLE_HEADERS[LOG_COLUMN_COUNT] = {
    "TIME",
    "EVENT",
    "ID",
    "NAME",
    "PID",
    "STATUS",
    "DETAILS",
};

typedef struct {
    char timestamp[LOG_TIMESTAMP_LEN];
    char event[LOG_EVENT_LEN];
    char message[LOG_MESSAGE_LEN];
    char container_id[CONTAINER_ID_LEN];
    int matches_filter;
} LogRecord;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char name[CONTAINER_NAME_LEN];
    pid_t pid;
    int has_pid;
    char status[LOG_STATUS_LEN];
} LogCatalogEntry;

typedef struct {
    LogCatalogEntry *entries;
    size_t count;
    size_t capacity;
} LogCatalog;

static pthread_mutex_t g_logger_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_event_type_va(const char *event_type, const char *format, va_list args) {
    FILE *file = fopen(LOG_FILE, "a");
    time_t now;
    struct tm *tm_info;
    char time_buffer[32];

    if (file == NULL) {
        return;
    }

    now = time(NULL);
    tm_info = localtime(&now);
    if (tm_info == NULL) {
        fclose(file);
        return;
    }

    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(file, "[%s] %s: ", time_buffer, event_type);
    vfprintf(file, format, args);
    fputc('\n', file);
    fclose(file);
}

void log_event_type(const char *event_type, const char *format, ...) {
    va_list args;

    if (event_type == NULL || event_type[0] == '\0' || format == NULL) {
        return;
    }

    pthread_mutex_lock(&g_logger_lock);
    va_start(args, format);
    log_event_type_va(event_type, format, args);
    va_end(args);
    pthread_mutex_unlock(&g_logger_lock);
}

void log_event(const char *format, ...) {
    va_list args;

    if (format == NULL) {
        return;
    }

    pthread_mutex_lock(&g_logger_lock);
    va_start(args, format);
    log_event_type_va("INFO", format, args);
    va_end(args);
    pthread_mutex_unlock(&g_logger_lock);
}

const char *logger_path(void) {
    return LOG_FILE;
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

static const char *safe_text(const char *text) {
    return (text != NULL && text[0] != '\0') ? text : "-";
}

static int line_matches_container(const char *line, const char *container_id) {
    if (line == NULL) {
        return 0;
    }
    if (container_id == NULL || container_id[0] == '\0') {
        return 1;
    }
    return strstr(line, container_id) != NULL;
}

static int logger_open_read(FILE **out_file) {
    if (out_file == NULL) {
        errno = EINVAL;
        return -1;
    }

    *out_file = fopen(LOG_FILE, "r");
    if (*out_file == NULL) {
        printf("[error] no logs found at %s\n\n", LOG_FILE);
        return -1;
    }

    return 0;
}

static void logger_print_repeat(const char *glyph, int count) {
    for (int i = 0; i < count; i++) {
        fputs(glyph, stdout);
    }
}

static int logger_table_inner_width(const int *widths, size_t count) {
    int total = 0;

    for (size_t i = 0; i < count; i++) {
        total += widths[i] + 2;
    }

    if (count > 0) {
        total += (int)(count - 1);
    }

    return total;
}

static void logger_print_full_border(int inner_width, const char *left, const char *right) {
    fputs(left, stdout);
    logger_print_repeat("─", inner_width + 2);
    fputs(right, stdout);
    printf("\n");
}

static void logger_print_full_row(int inner_width, const char *text) {
    printf("│ %-*.*s │\n",
           inner_width,
           inner_width,
           safe_text(text));
}

static void logger_print_table_border(const int *widths,
                                      size_t count,
                                      const char *left,
                                      const char *middle,
                                      const char *right) {
    fputs(left, stdout);
    for (size_t i = 0; i < count; i++) {
        logger_print_repeat("─", widths[i] + 2);
        fputs((i + 1 < count) ? middle : right, stdout);
    }
    printf("\n");
}

static void logger_print_table_row(const char *const *values, const int *widths, size_t count) {
    fputs("│", stdout);
    for (size_t i = 0; i < count; i++) {
        printf(" %-*.*s │",
               widths[i],
               widths[i],
               safe_text((values != NULL) ? values[i] : NULL));
    }
    printf("\n");
}

static void logger_print_header(const char *container_id) {
    int inner_width = logger_table_inner_width(LOG_TABLE_WIDTHS, LOG_COLUMN_COUNT);

    logger_print_full_border(inner_width, "╭", "╮");
    logger_print_full_row(inner_width, "Container Logs");
    if (container_id != NULL && container_id[0] != '\0') {
        char filter_row[96];

        snprintf(filter_row, sizeof(filter_row), "Filter: %s", container_id);
        logger_print_full_border(inner_width, "├", "┤");
        logger_print_full_row(inner_width, filter_row);
    }
    logger_print_table_border(LOG_TABLE_WIDTHS, LOG_COLUMN_COUNT, "├", "┬", "┤");
    logger_print_table_row(LOG_TABLE_HEADERS, LOG_TABLE_WIDTHS, LOG_COLUMN_COUNT);
    logger_print_table_border(LOG_TABLE_WIDTHS, LOG_COLUMN_COUNT, "├", "┼", "┤");
}

static void logger_print_footer(void) {
    logger_print_table_border(LOG_TABLE_WIDTHS, LOG_COLUMN_COUNT, "╰", "┴", "╯");
}

static int extract_container_id(const char *text, char *out, size_t out_size) {
    const char *prefix = "container-";
    const size_t prefix_len = strlen(prefix);
    const char *cursor = text;

    if (out_size == 0) {
        return 0;
    }

    out[0] = '\0';
    if (text == NULL) {
        return 0;
    }

    while ((cursor = strstr(cursor, prefix)) != NULL) {
        const char *digits = cursor + prefix_len;
        const char *end = digits;
        size_t len = 0;

        if (!isdigit((unsigned char)*digits)) {
            cursor = digits;
            continue;
        }

        while (isdigit((unsigned char)*end)) {
            end++;
        }

        len = (size_t)(end - cursor);
        if (len >= out_size) {
            len = out_size - 1;
        }
        memcpy(out, cursor, len);
        out[len] = '\0';
        return 1;
    }

    return 0;
}

static int extract_field_text(const char *text, const char *key, char *out, size_t out_size) {
    const char *cursor = NULL;
    const char *end = NULL;
    size_t len = 0;

    if (out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    if (text == NULL || key == NULL) {
        return 0;
    }

    cursor = strstr(text, key);
    if (cursor == NULL) {
        return 0;
    }
    cursor += strlen(key);
    end = cursor;

    while (*end != '\0' && *end != ' ' && *end != '\r' && *end != '\n' && *end != ')') {
        end++;
    }

    len = (size_t)(end - cursor);
    if (len == 0) {
        return 0;
    }

    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, cursor, len);
    out[len] = '\0';
    return 1;
}

static int extract_int_after_key(const char *text, const char *key, int *out_value) {
    const char *cursor = NULL;
    char *end = NULL;
    long value = 0;

    if (text == NULL || key == NULL || out_value == NULL) {
        return 0;
    }

    cursor = strstr(text, key);
    if (cursor == NULL) {
        return 0;
    }
    cursor += strlen(key);
    while (*cursor == ' ') {
        cursor++;
    }

    if (*cursor != '-' && !isdigit((unsigned char)*cursor)) {
        return 0;
    }

    value = strtol(cursor, &end, 10);
    if (end == cursor) {
        return 0;
    }

    *out_value = (int)value;
    return 1;
}

static int extract_pid_from_message(const char *message, int *pid_value) {
    if (extract_int_after_key(message, "pid=", pid_value)) {
        return 1;
    }
    if (extract_int_after_key(message, "(pid ", pid_value)) {
        return 1;
    }
    return 0;
}

static void infer_record_status(const LogRecord *record, char *status, size_t status_size) {
    if (status_size == 0) {
        return;
    }

    status[0] = '\0';
    if (record == NULL) {
        return;
    }

    if (strcmp(record->event, "CONTAINER_CREATED") == 0) {
        copy_string(status, status_size, "CREATED");
    } else if (strcmp(record->event, "CONTAINER_STARTED") == 0) {
        copy_string(status, status_size, "RUNNING");
    } else if (strcmp(record->event, "CONTAINER_STOPPED") == 0 ||
               strcmp(record->event, "RESOURCE_LIMIT_HIT") == 0) {
        copy_string(status, status_size, "STOPPED");
    } else if (strcmp(record->event, "CONTAINER_DELETED") == 0) {
        copy_string(status, status_size, "DELETED");
    } else if (strcmp(record->event, "ERROR") == 0) {
        copy_string(status, status_size, "ERROR");
    } else if (record->container_id[0] == '\0') {
        copy_string(status, status_size, "SYSTEM");
    } else {
        copy_string(status, status_size, "-");
    }
}

static void parse_log_line(const char *line, LogRecord *record) {
    char buffer[1024];
    const char *event_start = NULL;
    const char *message_start = NULL;
    const char *close = NULL;
    const char *separator = NULL;
    size_t len = 0;

    if (record == NULL) {
        return;
    }

    memset(record, 0, sizeof(*record));
    if (line == NULL) {
        return;
    }

    copy_string(buffer, sizeof(buffer), line);
    trim_newline(buffer);

    copy_string(record->timestamp, sizeof(record->timestamp), "-");
    copy_string(record->event, sizeof(record->event), "LOG");
    copy_string(record->message, sizeof(record->message), buffer);

    if (buffer[0] != '[') {
        extract_container_id(record->message, record->container_id, sizeof(record->container_id));
        return;
    }

    close = strstr(buffer, "] ");
    if (close == NULL) {
        extract_container_id(record->message, record->container_id, sizeof(record->container_id));
        return;
    }

    len = (size_t)(close - (buffer + 1));
    if (len >= sizeof(record->timestamp)) {
        len = sizeof(record->timestamp) - 1;
    }
    memcpy(record->timestamp, buffer + 1, len);
    record->timestamp[len] = '\0';

    event_start = close + 2;
    separator = strstr(event_start, ": ");
    if (separator == NULL) {
        copy_string(record->event, sizeof(record->event), event_start);
        record->message[0] = '\0';
        return;
    }

    len = (size_t)(separator - event_start);
    if (len >= sizeof(record->event)) {
        len = sizeof(record->event) - 1;
    }
    memcpy(record->event, event_start, len);
    record->event[len] = '\0';

    message_start = separator + 2;
    copy_string(record->message, sizeof(record->message), message_start);
    extract_container_id(record->message, record->container_id, sizeof(record->container_id));
}

static void logger_free_catalog(LogCatalog *catalog) {
    if (catalog == NULL) {
        return;
    }

    free(catalog->entries);
    catalog->entries = NULL;
    catalog->count = 0;
    catalog->capacity = 0;
}

static LogCatalogEntry *logger_catalog_get(LogCatalog *catalog, const char *container_id, int create) {
    if (catalog == NULL || container_id == NULL || container_id[0] == '\0') {
        return NULL;
    }

    for (size_t i = 0; i < catalog->count; i++) {
        if (strcmp(catalog->entries[i].id, container_id) == 0) {
            return &catalog->entries[i];
        }
    }

    if (!create) {
        return NULL;
    }

    if (catalog->count == catalog->capacity) {
        size_t next_capacity = (catalog->capacity == 0) ? 8 : catalog->capacity * 2;
        LogCatalogEntry *next_entries = realloc(catalog->entries, next_capacity * sizeof(*next_entries));

        if (next_entries == NULL) {
            return NULL;
        }
        catalog->entries = next_entries;
        catalog->capacity = next_capacity;
    }

    memset(&catalog->entries[catalog->count], 0, sizeof(catalog->entries[catalog->count]));
    copy_string(catalog->entries[catalog->count].id,
                sizeof(catalog->entries[catalog->count].id),
                container_id);
    catalog->count++;
    return &catalog->entries[catalog->count - 1];
}

static const LogCatalogEntry *logger_catalog_find(const LogCatalog *catalog, const char *container_id) {
    if (catalog == NULL || container_id == NULL || container_id[0] == '\0') {
        return NULL;
    }

    for (size_t i = 0; i < catalog->count; i++) {
        if (strcmp(catalog->entries[i].id, container_id) == 0) {
            return &catalog->entries[i];
        }
    }

    return NULL;
}

static void logger_catalog_update(LogCatalog *catalog, const LogRecord *record) {
    LogCatalogEntry *entry = NULL;
    char name[CONTAINER_NAME_LEN];
    char status[LOG_STATUS_LEN];
    int pid_value = 0;

    if (catalog == NULL || record == NULL || record->container_id[0] == '\0') {
        return;
    }

    entry = logger_catalog_get(catalog, record->container_id, 1);
    if (entry == NULL) {
        return;
    }

    if (extract_field_text(record->message, "name=", name, sizeof(name))) {
        copy_string(entry->name, sizeof(entry->name), name);
    }

    if (extract_pid_from_message(record->message, &pid_value)) {
        entry->pid = (pid_t)pid_value;
        entry->has_pid = 1;
    }

    infer_record_status(record, status, sizeof(status));
    if (status[0] != '\0' && strcmp(status, "-") != 0) {
        copy_string(entry->status, sizeof(entry->status), status);
    }
}

static void logger_render_record(const LogRecord *record, const LogCatalog *catalog) {
    const LogCatalogEntry *entry = NULL;
    const char *row[LOG_COLUMN_COUNT];
    char pid_text[16];
    char status[LOG_STATUS_LEN];
    int pid_value = 0;

    if (record == NULL) {
        return;
    }

    pid_text[0] = '\0';
    infer_record_status(record, status, sizeof(status));

    if (record->container_id[0] != '\0') {
        entry = logger_catalog_find(catalog, record->container_id);
    }

    if (entry != NULL) {
        if (entry->status[0] != '\0') {
            copy_string(status, sizeof(status), entry->status);
        }
        if (entry->has_pid) {
            snprintf(pid_text, sizeof(pid_text), "%d", (int)entry->pid);
        }
    }

    if (pid_text[0] == '\0' && extract_pid_from_message(record->message, &pid_value)) {
        snprintf(pid_text, sizeof(pid_text), "%d", pid_value);
    }

    row[0] = record->timestamp;
    row[1] = record->event;
    row[2] = record->container_id[0] != '\0' ? record->container_id : "-";
    row[3] = (entry != NULL && entry->name[0] != '\0') ? entry->name : "-";
    row[4] = pid_text[0] != '\0' ? pid_text : "-";
    row[5] = status;
    row[6] = record->message;
    logger_print_table_row(row, LOG_TABLE_WIDTHS, LOG_COLUMN_COUNT);
}

static int logger_read_records(FILE *file,
                               const char *container_id,
                               LogRecord **out_records,
                               size_t *out_count) {
    LogRecord *records = NULL;
    char buffer[1024];
    size_t count = 0;
    size_t capacity = 0;

    if (file == NULL || out_records == NULL || out_count == NULL) {
        errno = EINVAL;
        return -1;
    }

    while (fgets(buffer, sizeof(buffer), file) != NULL) {
        if (count == capacity) {
            size_t next_capacity = (capacity == 0) ? 16 : capacity * 2;
            LogRecord *next_records = realloc(records, next_capacity * sizeof(*next_records));

            if (next_records == NULL) {
                free(records);
                printf("[error] out of memory\n\n");
                return -1;
            }

            records = next_records;
            capacity = next_capacity;
        }

        parse_log_line(buffer, &records[count]);
        records[count].matches_filter = line_matches_container(buffer, container_id);
        count++;
    }

    *out_records = records;
    *out_count = count;
    return 0;
}

static int logger_build_selection(const LogRecord *records,
                                  size_t record_count,
                                  int tail_lines,
                                  size_t **out_indexes,
                                  size_t *out_count) {
    size_t *matches = NULL;
    size_t *selected = NULL;
    size_t match_count = 0;
    size_t selected_count = 0;
    size_t start = 0;

    if (out_indexes == NULL || out_count == NULL) {
        errno = EINVAL;
        return -1;
    }

    *out_indexes = NULL;
    *out_count = 0;

    for (size_t i = 0; i < record_count; i++) {
        if (records[i].matches_filter) {
            match_count++;
        }
    }

    if (match_count == 0) {
        return 0;
    }

    matches = malloc(match_count * sizeof(*matches));
    if (matches == NULL) {
        printf("[error] out of memory\n\n");
        return -1;
    }

    match_count = 0;
    for (size_t i = 0; i < record_count; i++) {
        if (records[i].matches_filter) {
            matches[match_count++] = i;
        }
    }

    if (tail_lines > 0 && match_count > (size_t)tail_lines) {
        start = match_count - (size_t)tail_lines;
    }
    selected_count = match_count - start;

    selected = malloc(selected_count * sizeof(*selected));
    if (selected == NULL) {
        free(matches);
        printf("[error] out of memory\n\n");
        return -1;
    }

    memcpy(selected, matches + start, selected_count * sizeof(*selected));
    free(matches);

    *out_indexes = selected;
    *out_count = selected_count;
    return 0;
}

static int logger_render_records(const LogRecord *records,
                                 size_t record_count,
                                 const size_t *selected_indexes,
                                 size_t selected_count,
                                 const char *container_id,
                                 int keep_open,
                                 int *table_open,
                                 LogCatalog *catalog) {
    size_t selected_cursor = 0;
    int printed = 0;

    if (records == NULL || catalog == NULL) {
        errno = EINVAL;
        return -1;
    }

    for (size_t i = 0; i < record_count; i++) {
        logger_catalog_update(catalog, &records[i]);

        if (selected_cursor >= selected_count || selected_indexes[selected_cursor] != i) {
            continue;
        }

        if (table_open != NULL && !*table_open) {
            logger_print_header(container_id);
            *table_open = 1;
        }

        logger_render_record(&records[i], catalog);
        printed = 1;
        selected_cursor++;
    }

    if (!keep_open && table_open != NULL && *table_open) {
        logger_print_footer();
        printf("\n");
        *table_open = 0;
    }

    return printed;
}

static void logger_print_no_results(const char *container_id) {
    if (container_id != NULL && container_id[0] != '\0') {
        printf("[manager] no logs found for %s\n\n", container_id);
    } else {
        printf("[manager] no logs recorded yet\n\n");
    }
}

static int logger_render_history(const char *container_id, int tail_lines) {
    FILE *file = NULL;
    LogRecord *records = NULL;
    size_t record_count = 0;
    size_t *selected_indexes = NULL;
    size_t selected_count = 0;
    LogCatalog catalog = {0};
    int table_open = 0;
    int printed = 0;

    if (logger_open_read(&file) != 0) {
        return -1;
    }

    if (logger_read_records(file, container_id, &records, &record_count) != 0) {
        fclose(file);
        return -1;
    }
    fclose(file);

    if (logger_build_selection(records, record_count, tail_lines, &selected_indexes, &selected_count) != 0) {
        free(records);
        logger_free_catalog(&catalog);
        return -1;
    }

    printed = logger_render_records(records,
                                    record_count,
                                    selected_indexes,
                                    selected_count,
                                    container_id,
                                    0,
                                    &table_open,
                                    &catalog);

    free(selected_indexes);
    free(records);
    logger_free_catalog(&catalog);

    if (printed <= 0) {
        logger_print_no_results(container_id);
    } else {
        fflush(stdout);
    }

    return 0;
}

int logger_print(const char *container_id) {
    return logger_render_history(container_id, 0);
}

int logger_tail(const char *container_id, int line_count) {
    if (line_count <= 0) {
        printf("[error] usage: logs [-f] [-n N] [id]\n\n");
        return -1;
    }

    return logger_render_history(container_id, line_count);
}

int logger_follow(const char *container_id, int initial_tail_lines) {
    FILE *file = NULL;
    LogRecord *records = NULL;
    size_t record_count = 0;
    size_t *selected_indexes = NULL;
    size_t selected_count = 0;
    LogCatalog catalog = {0};
    long offset = 0;
    int table_open = 0;

    if (logger_open_read(&file) != 0) {
        return -1;
    }

    if (logger_read_records(file, container_id, &records, &record_count) != 0) {
        fclose(file);
        return -1;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        free(records);
        printf("[error] failed to seek logs\n\n");
        return -1;
    }
    offset = ftell(file);
    fclose(file);

    if (logger_build_selection(records,
                               record_count,
                               (initial_tail_lines > 0) ? initial_tail_lines : 0,
                               &selected_indexes,
                               &selected_count) != 0) {
        free(records);
        logger_free_catalog(&catalog);
        return -1;
    }

    printf("[hint] following %s%s%s; press Ctrl+C to stop\n",
           LOG_FILE,
           (container_id != NULL && container_id[0] != '\0') ? " for " : "",
           (container_id != NULL && container_id[0] != '\0') ? container_id : "");

    (void)logger_render_records(records,
                                record_count,
                                selected_indexes,
                                selected_count,
                                container_id,
                                1,
                                &table_open,
                                &catalog);

    free(selected_indexes);
    free(records);

    while (1) {
        struct stat st;

        if (container_consume_interrupt()) {
            if (table_open) {
                logger_print_footer();
            }
            printf("\n");
            logger_free_catalog(&catalog);
            return 0;
        }

        container_refresh_state();

        if (stat(LOG_FILE, &st) == 0) {
            if ((long)st.st_size < offset) {
                offset = 0;
            }

            if ((long)st.st_size > offset) {
                FILE *stream = fopen(LOG_FILE, "r");

                if (stream != NULL) {
                    if (fseek(stream, offset, SEEK_SET) == 0) {
                        char buffer[1024];

                        while (fgets(buffer, sizeof(buffer), stream) != NULL) {
                            LogRecord record;

                            parse_log_line(buffer, &record);
                            record.matches_filter = line_matches_container(buffer, container_id);
                            logger_catalog_update(&catalog, &record);

                            if (record.matches_filter) {
                                if (!table_open) {
                                    logger_print_header(container_id);
                                    table_open = 1;
                                }
                                logger_render_record(&record, &catalog);
                                fflush(stdout);
                            }
                        }
                        offset = ftell(stream);
                    }
                    fclose(stream);
                }
            }
        }

        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 200000000L;
            nanosleep(&ts, NULL);
        }
    }
}
