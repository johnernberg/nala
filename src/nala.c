#include <libgen.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <ctype.h>
#include <errno.h>
#include <sys/time.h>
#include <getopt.h>
#include "subprocess.h"
#include "traceback.h"
#include "nala.h"
#include "diff/diff.h"
#include "hexdump/hexdump.h"
#include "utils.h"
#include "hf.h"

#define ANSI_COLOR_RED "\x1b[31m"
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_YELLOW "\x1b[33m"
#define ANSI_COLOR_BLUE "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN "\x1b[36m"

#define ANSI_BOLD "\x1b[1m"
#define ANSI_RESET "\x1b[0m"

#define COLOR(color, ...) ANSI_RESET ANSI_COLOR_##color __VA_ARGS__ ANSI_RESET

#define BOLD(...) ANSI_RESET ANSI_BOLD __VA_ARGS__ ANSI_RESET

#define COLOR_BOLD(color, ...)                                          \
    ANSI_RESET ANSI_COLOR_##color ANSI_BOLD __VA_ARGS__ ANSI_RESET

struct job_request_t {
    struct nala_test_t *test_p;
    int number;
};

struct job_response_t {
    struct nala_test_t *test_p;
    struct nala_test_t test;
};

struct tests_t {
    struct nala_test_t *head_p;
    struct nala_test_t *tail_p;
};

struct capture_output_t {
    bool running;
    char **output_pp;
    size_t length;
    int original_fd;
    FILE *temporary_file_p;
    FILE *original_file_p;
    FILE *stdout_p;
};

struct with_message_t {
    char *message_p;
    struct with_message_t *next_p;
    struct with_message_t *prev_p;
};

struct with_message_stack_t {
    struct with_message_t *head_p;
    struct with_message_t *tail_p;
};

struct auto_free_t {
    void *buf_p;
    struct auto_free_t *next_p;
};

struct auto_free_list_t {
    struct auto_free_t *head_p;
    struct auto_free_t *tail_p;
};

static struct auto_free_list_t auto_free_list;

static struct nala_test_t *current_test_p = NULL;

static struct tests_t tests = {
    .head_p = NULL,
    .tail_p = NULL
};

static struct capture_output_t capture_stdout;
static struct capture_output_t capture_stderr;
static struct with_message_stack_t with_message_stack;

__attribute__ ((weak)) void nala_assert_all_mocks_completed(void)
{
}

__attribute__ ((weak)) void nala_reset_all_mocks(void)
{
}

__attribute__ ((weak)) void nala_suspend_all_mocks(void)
{
}

__attribute__ ((weak)) void nala_resume_all_mocks(void)
{
}

__attribute__ ((weak)) char *nala_format_mock_traceback(
    const char *message_p,
    struct nala_traceback_t *traceback_p)
{
    (void)message_p;
    (void)traceback_p;

    return (strdup(""));
}

__attribute__ ((weak)) bool nala_mock_current_is_set(void)
{
    return (false);
}

__attribute__ ((weak)) int nala_print_call_mask = 0;
__attribute__ ((weak)) const char *nala_mock_func_p = NULL;
__attribute__ ((weak)) const char *nala_mock_param_p = NULL;
__attribute__ ((weak)) struct nala_traceback_t *nala_mock_traceback_p = NULL;

/* Command line options. */
static bool continue_on_failure = false;
static const char *report_json_file_p = "report.json";
static int number_of_jobs = 1;

static const char *get_node(void)
{
    static char buf[128];

    return (nala_hf_get_hostname(&buf[0], sizeof(buf), "*** unknown ***"));
}

static const char *get_user(void)
{
    static char buf[128];

    return (nala_hf_get_username(&buf[0], sizeof(buf), "*** unknown ***"));
}

static const char *format_timespan(float elapsed_time_ms)
{
    static char buf[128];

    return (nala_hf_format_timespan(&buf[0],
                                    sizeof(buf),
                                    (unsigned long long)elapsed_time_ms));
}

static void color_start(FILE *file_p, const char *color_p)
{
    fprintf(file_p, "%s%s%s", ANSI_RESET, color_p, ANSI_BOLD);
}

static void color_reset(FILE *file_p)
{
    fprintf(file_p, "%s", ANSI_RESET);
}

static char *format_suite_prefix(struct nala_test_t *test_p,
                                 char *buf_p,
                                 size_t size)
{
    size_t length;

    strncpy(buf_p, test_p->file_p, size);
    buf_p = basename(buf_p);
    length = strlen(buf_p);

    if (length < 2) {
        return ("");
    }

    buf_p[length - 2] = '\0';

    if (strcmp(buf_p, "main") == 0) {
        return ("");
    }

    strcat(buf_p, "::");

    return (buf_p);
}

static char *full_test_name(struct nala_test_t *test_p)
{
    static char buf[512];
    char suite[512];
    char *suite_p;

    suite_p = format_suite_prefix(test_p, &suite[0], sizeof(suite));
    snprintf(&buf[0], sizeof(buf), "%s%s", suite_p, test_p->name_p);

    return (&buf[0]);
}

static void capture_output_init(struct capture_output_t *self_p,
                                FILE *file_p)
{
    self_p->output_pp = NULL;
    self_p->length = 0;
    self_p->original_file_p = file_p;
    self_p->stdout_p = fdopen(dup(fileno(file_p)), "w");
}

static void capture_output_destroy(struct capture_output_t *self_p)
{
    if (self_p->output_pp != NULL) {
        if (*self_p->output_pp != NULL) {
            free(*self_p->output_pp);
        }

        self_p->output_pp = NULL;
    }

    if (self_p->stdout_p != NULL) {
        fflush(self_p->stdout_p);
        fclose(self_p->stdout_p);
    }
}

static void capture_output_redirect(struct capture_output_t *self_p)
{
    fflush(self_p->original_file_p);

    self_p->temporary_file_p = tmpfile();
    self_p->original_fd = dup(fileno(self_p->original_file_p));

    while ((dup2(fileno(self_p->temporary_file_p),
                 fileno(self_p->original_file_p)) == -1)
           && (errno == EINTR));
}

static void capture_output_restore(struct capture_output_t *self_p)
{
    fflush(self_p->original_file_p);

    while ((dup2(self_p->original_fd, fileno(self_p->original_file_p)) == -1)
           && (errno == EINTR));

    close(self_p->original_fd);
}

static void capture_output_start(struct capture_output_t *self_p,
                                 char **output_pp)
{
    self_p->output_pp = output_pp;
    self_p->length = 0;
    self_p->running = true;
    capture_output_redirect(self_p);
}

static void capture_output_stop(struct capture_output_t *self_p)
{
    size_t nmembers;

    if (!self_p->running) {
        return;
    }

    self_p->running = false;
    capture_output_restore(self_p);

    self_p->length = (size_t)ftell(self_p->temporary_file_p);
    fseek(self_p->temporary_file_p, 0, SEEK_SET);
    *self_p->output_pp = malloc(self_p->length + 1);

    if (*self_p->output_pp == NULL) {
        printf("Failed to allocate memory.\n");
        exit(1);
    }

    if (self_p->length > 0) {
        nmembers = fread(*self_p->output_pp,
                         self_p->length,
                         1,
                         self_p->temporary_file_p);

        if (nmembers != 1) {
            printf("Failed to read capture output.\n");
            exit(1);
        }
    }

    (*self_p->output_pp)[self_p->length] = '\0';
    fclose(self_p->temporary_file_p);

    printf("%s", *self_p->output_pp);
}

static void with_message_stack_init(struct with_message_stack_t *self_p)
{
    self_p->head_p = NULL;
    self_p->tail_p = NULL;
}

static void with_message_stack_push(struct with_message_stack_t *self_p,
                                    char *message_p)
{
    struct with_message_t *item_p;

    item_p = malloc(sizeof(*item_p));
    item_p->message_p = message_p;
    item_p->next_p = NULL;

    if (self_p->head_p == NULL) {
        self_p->head_p = item_p;
    } else {
        item_p->prev_p = self_p->tail_p;
        self_p->tail_p->next_p = item_p;
    }

    self_p->tail_p = item_p;
}

static void with_message_stack_pop(struct with_message_stack_t *self_p)
{
    struct with_message_t *item_p;

    item_p = self_p->tail_p;

    if (item_p == self_p->head_p) {
        self_p->head_p = NULL;
    } else {
        self_p->tail_p = item_p->prev_p;
        self_p->tail_p->next_p = NULL;
    }

    free(item_p->message_p);
    free(item_p);
}

static bool with_message_stack_is_empty(struct with_message_stack_t *self_p)
{
    return (self_p->head_p == NULL);
}

static void with_message_stack_print(struct with_message_stack_t *self_p)
{
    struct with_message_t *item_p;
    char *prefix_p;

    prefix_p = "  Messages: ";
    item_p = self_p->head_p;

    while (item_p != NULL) {
        printf("%s"COLOR_BOLD(RED, "%s\n"), prefix_p, item_p->message_p);
        prefix_p = "            ";
        item_p = item_p->next_p;
    }
}

static void auto_free_list_init(struct auto_free_list_t *self_p)
{
    self_p->head_p = NULL;
    self_p->tail_p = NULL;
}

static void auto_free_list_append(struct auto_free_list_t *self_p,
                                  void *buf_p)
{
    struct auto_free_t *item_p;

    item_p = malloc(sizeof(*item_p));
    ASSERT_NE(item_p, NULL);
    item_p->buf_p = buf_p;
    item_p->next_p = NULL;

    if (self_p->head_p == NULL) {
        self_p->head_p = item_p;
    } else {
        self_p->tail_p->next_p = item_p;
    }

    self_p->tail_p = item_p;
}

static void auto_free_list_destroy(struct auto_free_list_t *self_p)
{
    struct auto_free_t *next_p;
    struct auto_free_t *item_p;

    next_p = self_p->head_p;

    while (next_p != NULL) {
        item_p = next_p;
        next_p = next_p->next_p;
        free(item_p->buf_p);
        free(item_p);
    }
}

FILE *nala_get_stdout(void)
{
    if (capture_stdout.running) {
        return (capture_stdout.stdout_p);
    } else {
        return (stdout);
    }
}

static float timeval_to_ms(struct timeval *timeval_p)
{
    float res;

    res = (float)timeval_p->tv_usec;
    res /= 1000;
    res += (float)(1000 * timeval_p->tv_sec);

    return (res);
}

static void print_test_failure_report_begin(void)
{
    printf("\n"
           "――――――――――――――――――――――――――――――――――――――――"
           "――――――――――――――――――――――――――――――――――――――――"
           "\n"
           "\n");
}

static void print_test_failure_report_end(void)
{
    printf("\n");
}

static void print_signal_failure(struct nala_test_t *test_p)
{
    print_test_failure_report_begin();
    printf("  Test:  " COLOR_BOLD(CYAN, "%s\n"), full_test_name(current_test_p));
    printf("  Error: " COLOR_BOLD(RED, "Terminated by signal %d (%s).\n"),
           test_p->run.signal_number,
           strsignal(test_p->run.signal_number));
    printf("\n  Run '" COLOR(GREEN, "make gdb TEST=^%s\\$") "' to debug with GDB.\n",
           full_test_name(current_test_p));
    print_test_failure_report_end();
}

static const char *test_result(struct nala_test_t *test_p, bool color)
{
    const char *result_p;

    if (test_p->run.executed) {
        if (test_p->run.exit_code == 0) {
            if (color) {
                result_p = COLOR_BOLD(GREEN, "PASSED");
            } else {
                result_p = "PASSED";
            }
        } else {
            if (color) {
                result_p = COLOR_BOLD(RED, "FAILED");
            } else {
                result_p = "FAILED";
            }
        }
    } else {
        if (color) {
            result_p = COLOR_BOLD(YELLOW, "SKIPPED");
        } else {
            result_p = "SKIPPED";
        }
    }

    return (result_p);
}

static void print_test_result(struct nala_test_t *test_p)
{
    printf("%s %s (" COLOR_BOLD(YELLOW, "%s") ")",
           test_result(test_p, true),
           full_test_name(test_p),
           format_timespan(test_p->run.elapsed_time_ms));

    if (test_p->run.signal_number != -1) {
        printf(" (signal: %d)", test_p->run.signal_number);
    }

    printf("\n");
    fflush(stdout);
}

static void print_summary(struct nala_test_t *test_p,
                          float elapsed_time_ms)
{
    int total;
    int passed;
    int failed;
    int skipped;

    total = 0;
    passed = 0;
    failed = 0;
    skipped = 0;

    while (test_p != NULL) {
        total++;

        if (test_p->run.executed) {
            if (test_p->run.exit_code == 0) {
                passed++;
            } else {
                failed++;
            }
        } else {
            skipped++;
        }

        test_p = test_p->next_p;
    }

    printf("\nTests: ");

    if (failed > 0) {
        printf(COLOR_BOLD(RED, "%d failed") ", ", failed);
    }

    if (passed > 0) {
        printf(COLOR_BOLD(GREEN, "%d passed") ", ", passed);
    }

    if (skipped > 0) {
        printf(COLOR_BOLD(YELLOW, "%d skipped") ", ", skipped);
    }

    printf("%d total\n", total);
    printf("Time: " COLOR_BOLD(YELLOW, "%s") "\n",
           format_timespan(elapsed_time_ms));
}

static void write_report_json(struct nala_test_t *test_p)
{
    FILE *file_p;

    file_p = fopen(report_json_file_p, "w");

    if (file_p == NULL) {
        fprintf(stderr,
                "error: Failed to open JSON report '%s' with '%s'.\n",
                report_json_file_p,
                strerror(errno));
        exit(1);
    }

    fprintf(file_p,
            "{\n"
            "    \"name\": \"-\",\n"
            "    \"date\": \"-\",\n"
            "    \"node\": \"%s\",\n"
            "    \"user\": \"%s\",\n"
            "    \"testcases\": [\n",
            get_node(),
            get_user());

    while (test_p != NULL) {
        fprintf(file_p,
                "        {\n"
                "            \"name\": \"%s\",\n"
                "            \"description\": [],\n"
                "            \"result\": \"%s\",\n"
                "            \"execution_time\": \"%s\"\n"
                "        }%s\n",
                full_test_name(test_p),
                test_result(test_p, false),
                format_timespan(test_p->run.elapsed_time_ms),
                (test_p->next_p != NULL ? "," : ""));
        test_p = test_p->next_p;
    }

    fprintf(file_p,
            "    ]\n"
            "}\n");
    fclose(file_p);
}

static void test_entry(void *arg_p)
{
    struct nala_test_t *test_p;

    test_p = (struct nala_test_t *)arg_p;
    capture_output_init(&capture_stdout, stdout);
    capture_output_init(&capture_stderr, stderr);
    with_message_stack_init(&with_message_stack);
    auto_free_list_init(&auto_free_list);
    nala_reset_all_mocks();
    test_p->func();
    nala_exit(0);
}

static int run_test(struct nala_test_t *test_p)
{
    int res;
    struct timeval start_time;
    struct timeval end_time;
    struct timeval elapsed_time;
    struct nala_subprocess_result_t *result_p;

    res = 0;
    gettimeofday(&start_time, NULL);
    current_test_p = test_p;
    test_p->before_fork_func();

    result_p = nala_subprocess_call(test_entry, test_p);

    test_p->run.executed = true;
    test_p->run.exit_code = result_p->exit_code;
    test_p->run.signal_number = result_p->signal_number;
    nala_subprocess_result_free(result_p);

    if (test_p->run.exit_code != 0) {
        res = 1;
    }

    gettimeofday(&end_time, NULL);
    timersub(&end_time, &start_time, &elapsed_time);
    test_p->run.elapsed_time_ms = timeval_to_ms(&elapsed_time);

    if (test_p->run.signal_number != -1) {
        print_signal_failure(test_p);
    }

    print_test_result(test_p);

    return (res);
}

static int run_tests_sequentially(struct nala_test_t *test_p)
{
    int res;
    int exit_code;

    exit_code = 0;

    while (test_p != NULL) {
        res = run_test(test_p);

        if (res != 0) {
            exit_code = res;

            if (!continue_on_failure) {
                break;
            }
        }

        test_p = test_p->next_p;
    }

    return (exit_code);
}

static int run_tests_in_parallel_child(struct nala_test_t *test_p,
                                       int request_fds[2],
                                       int response_fds[2])
{
    int previous_number;
    ssize_t size;
    int exit_code;
    struct job_request_t request;
    struct job_response_t response;
    int res;

    close(request_fds[1]);
    close(response_fds[0]);

    exit_code = 0;
    previous_number = 0;

    while (true) {
        size = read(request_fds[0], &request, sizeof(request));

        if (size == 0) {
            break;
        }

        if (size != sizeof(request)) {
            perror("request read");
            exit(1);
        }

        while (previous_number < request.number) {
            previous_number++;
            test_p = test_p->next_p;
        }

        res = run_test(test_p);

        if (res != 0) {
            exit_code = res;
        }

        response.test_p = request.test_p;
        response.test = *test_p;

        size = write(response_fds[1], &response, sizeof(response));

        if (size != sizeof(response)) {
            perror("response write");
            exit(1);
        }
    }

    close(request_fds[0]);
    close(response_fds[1]);

    return (exit_code);
}

static void write_job_request(int fd,
                              struct nala_test_t *test_p,
                              int number)
{
    struct job_request_t request;
    ssize_t size;

    request.test_p = test_p;
    request.number = number;

    size = write(fd, &request, sizeof(request));

    if (size != sizeof(request)) {
        perror("request write");
        exit(1);
    }
}

static int read_job_response(int fd)
{
    struct job_response_t response;
    ssize_t size;
    struct nala_test_t *test_p;

    size = read(fd, &response, sizeof(response));

    if (size == 0) {
        return (1);
    }

    if (size != sizeof(response)) {
        perror("response read");
        exit(1);
    }

    /* Copy test result to this process's list. */
    test_p = response.test_p;
    test_p->run = response.test.run;

    return (test_p->run.exit_code);
}

static int run_tests_in_parallel(struct nala_test_t *test_p)
{
    int res;
    int request_fds[2];
    int response_fds[2];
    int i;
    pid_t *pids_p;
    int status;
    int exit_code;
    int number_of_outstanding_requests;
    int number;

    res = pipe(&request_fds[0]);

    if (res != 0) {
        fprintf(stderr, "error: request pipe open\n");
        exit(1);
    }

    res = pipe(&response_fds[0]);

    if (res != 0) {
        fprintf(stderr, "error: response pipe open\n");
        exit(1);
    }

    /* Create workers. */
    pids_p = alloca((size_t)number_of_jobs * sizeof(*pids_p));

    for (i = 0; i < number_of_jobs; i++) {
        pids_p[i] = fork();

        if (pids_p[i] < 0) {
            fprintf(stderr, "error: Failed to create worker process\n");
            exit(1);
        } else if (pids_p[i] == 0) {
            exit(run_tests_in_parallel_child(test_p,
                                             request_fds,
                                             response_fds));
        }
    }

    close(response_fds[1]);

    /* Keep the workers busy. */
    number_of_outstanding_requests = 0;
    number = 0;

    while (test_p != NULL) {
        if (number_of_outstanding_requests == number_of_jobs) {
            res = read_job_response(response_fds[0]);
            number_of_outstanding_requests--;

            if ((res != 0) && !continue_on_failure) {
                break;
            }
        }

        write_job_request(request_fds[1], test_p, number);
        number_of_outstanding_requests++;
        number++;
        test_p = test_p->next_p;
    }

    close(request_fds[1]);

    /* Read remaining responses. */
    while (number_of_outstanding_requests > 0) {
        (void)read_job_response(response_fds[0]);
        number_of_outstanding_requests--;
    }

    close(request_fds[0]);
    close(response_fds[0]);

    /* Wait for all workers to exit. */
    exit_code = 0;

    for (i = 0; i < number_of_jobs; i++) {
        waitpid(pids_p[i], &status, 0);

        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) != 0) {
                exit_code = 1;
            }
        }

        if (WIFSIGNALED(status)) {
            exit_code = 1;
        }
    }

    return (exit_code);
}

static int run_tests(struct nala_test_t *tests_p)
{
    int exit_code;
    struct timeval start_time;
    struct timeval end_time;
    struct timeval elapsed_time;
    struct nala_test_t *test_p;

    test_p = tests_p;

    while (test_p != NULL) {
        test_p->run.executed = false;
        test_p = test_p->next_p;
    }

    test_p = tests_p;
    gettimeofday(&start_time, NULL);

    if (number_of_jobs == 1) {
        exit_code = run_tests_sequentially(test_p);
    } else {
        exit_code = run_tests_in_parallel(test_p);
    }

    gettimeofday(&end_time, NULL);
    timersub(&end_time, &start_time, &elapsed_time);
    print_summary(tests_p, timeval_to_ms(&elapsed_time));
    fflush(stdout);
    write_report_json(tests_p);

    return (exit_code);
}

bool nala_check_string_equal(const char *actual_p, const char *expected_p)
{
    bool are_equal;

    if (actual_p == expected_p) {
        return (true);
    }

    nala_suspend_all_mocks();
    are_equal = ((actual_p != NULL)
                 && (expected_p != NULL)
                 && (strcmp(actual_p, expected_p) == 0));
    nala_resume_all_mocks();

    return (are_equal);
}

const char *nala_format(const char *format_p, ...)
{
    va_list vl;
    size_t size;
    char *buf_p;
    FILE *file_p;

    nala_suspend_all_mocks();
    file_p = open_memstream(&buf_p, &size);
    color_start(file_p, ANSI_COLOR_RED);
    va_start(vl, format_p);
    vfprintf(file_p, format_p, vl);
    va_end(vl);
    color_reset(file_p);
    fputc('\0', file_p);
    fclose(file_p);
    nala_resume_all_mocks();

    return (buf_p);
}

static const char *display_inline_diff(FILE *file_p,
                                       const struct NalaDiff *inline_diff,
                                       size_t lines,
                                       const char *string,
                                       size_t *line_number,
                                       bool use_original)
{
    struct NalaDiffChunk *inline_chunk = &inline_diff->chunks[0];
    size_t line_index = 0;
    size_t index = 0;

    for (size_t i = 0; i < lines; i++) {
        const char *next = nala_next_line(string);
        size_t line_length = (size_t)(next - string);

        char line_prefix[64];

        if (use_original) {
            snprintf(line_prefix,
                     sizeof(line_prefix),
                     "- " BOLD("%ld"),
                     *line_number);
            fprintf(file_p, " %19s" " |  ", line_prefix);
        } else {
            snprintf(line_prefix,
                     sizeof(line_prefix),
                     COLOR(RED, "+ ") COLOR_BOLD(RED, "%ld"),
                     *line_number);
            fprintf(file_p, " %37s" COLOR(RED, " |  "), line_prefix);
        }

        while (index - line_index < line_length) {
            size_t chunk_end =
                use_original ? inline_chunk->original_end : inline_chunk->modified_end;

            size_t start = index - line_index;
            size_t end = nala_min_size_t(chunk_end - line_index, line_length);

            size_t characters = end - start;

            if (inline_chunk->type == NALA_DIFF_CHUNK_TYPE_MATCHED) {
                fprintf(file_p, "%.*s", (int)characters, string + index - line_index);
            } else if (characters > 0) {
                if (use_original) {
                    fprintf(file_p,
                            BOLD("%.*s"),
                            (int)characters,
                            string + index - line_index);
                } else {
                    fprintf(file_p,
                            COLOR_BOLD(RED, "%.*s"),
                            (int)characters,
                            string + index - line_index);
                }
            }

            index += characters;

            if (index >= chunk_end) {
                inline_chunk++;
            }
        }

        fprintf(file_p, "\n");

        if (!use_original) {
            (*line_number)++;
        }

        string = next + 1;
        line_index += line_length + 1;
        index = line_index;
    }

    return string;
}

static void print_string_diff(FILE *file_p,
                              const char *original,
                              const char *modified)
{
    fprintf(file_p, "  Diff:\n\n");

    struct NalaDiff diff = nala_diff_lines(original, modified);

    size_t line_number = 1;

    for (size_t chunk_index = 0; chunk_index < diff.size; chunk_index++) {
        struct NalaDiffChunk *chunk = &diff.chunks[chunk_index];

        size_t original_lines = chunk->original_end - chunk->original_start;
        size_t modified_lines = chunk->modified_end - chunk->modified_start;

        if (chunk->type == NALA_DIFF_CHUNK_TYPE_MATCHED) {
            for (size_t i = 0; i < original_lines; i++) {
                const char *original_next = nala_next_line(original);
                const char *modified_next = nala_next_line(modified);

                if (original_lines < 7 || (i < 2 && chunk_index > 0) ||
                    (original_lines - i < 3 && chunk_index < diff.size - 1)) {
                    fprintf(file_p, COLOR(MAGENTA, "%8zu"), line_number);
                    fprintf(file_p, " |  %.*s\n", (int)(original_next - original), original);
                } else if (i == 2) {
                    fprintf(file_p, "   :\n");
                }

                line_number++;
                original = original_next + 1;
                modified = modified_next + 1;
            }
        } else if (chunk->type == NALA_DIFF_CHUNK_TYPE_REPLACED) {
            const char *original_end = nala_next_lines(original, original_lines);
            const char *modified_end = nala_next_lines(modified, modified_lines);

            size_t original_length = (size_t)(original_end - original);
            size_t modified_length = (size_t)(modified_end - modified);

            struct NalaDiff inline_diff =
                nala_diff_strings_lengths(original,
                                          original_length,
                                          modified,
                                          modified_length);

            original = display_inline_diff(file_p,
                                           &inline_diff,
                                           original_lines,
                                           original,
                                           &line_number,
                                           true);
            modified = display_inline_diff(file_p,
                                           &inline_diff,
                                           modified_lines,
                                           modified,
                                           &line_number,
                                           false);

            free(inline_diff.chunks);
        } else if (chunk->type == NALA_DIFF_CHUNK_TYPE_DELETED) {
            for (size_t i = 0; i < original_lines; i++) {
                const char *original_next = nala_next_line(original);

                char line_prefix[64];
                snprintf(line_prefix,
                         sizeof(line_prefix),
                         COLOR(RED, "- ") COLOR_BOLD(RED, "%ld"),
                         line_number);

                fprintf(file_p, " %37s", line_prefix);
                fprintf(file_p, COLOR(RED, " |  ") COLOR_BOLD(RED, "%.*s\n"),
                        (int)(original_next - original),
                        original);

                original = original_next + 1;
            }
        } else if (chunk->type == NALA_DIFF_CHUNK_TYPE_ADDED) {
            for (size_t i = 0; i < modified_lines; i++) {
                const char *modified_next = nala_next_line(modified);

                char line_prefix[64];
                snprintf(line_prefix,
                         sizeof(line_prefix),
                         COLOR(RED, "+ ") COLOR_BOLD(RED, "%ld"),
                         line_number);

                fprintf(file_p, " %37s", line_prefix);
                fprintf(file_p, COLOR(RED, " |  ") COLOR_BOLD(RED, "%.*s\n"),
                        (int)(modified_next - modified),
                        modified);

                line_number++;
                modified = modified_next + 1;
            }
        }
    }

    free(diff.chunks);
    fprintf(file_p, "\n");
}

static char *make_string_readable_length(const char *string_p, size_t length)
{
    size_t size;
    char *buf_p;
    FILE *file_p;
    size_t i;

    file_p = open_memstream(&buf_p, &size);

    for (i = 0; i < length; i++) {
        if (isprint(string_p[i])) {
            fputc(string_p[i], file_p);
        } else if (string_p[i] == '\r') {
            fprintf(file_p, "\\r");
        } else if (string_p[i] == '\t') {
            fprintf(file_p, "\\t");
        } else if (string_p[i] == '\n') {
            fprintf(file_p, "\\n");
            fputc(string_p[i], file_p);
        } else {
            fprintf(file_p, "\\x%02x", (string_p[i]) & 0xff);
        }
    }

    fputc('\0', file_p);
    fclose(file_p);

    return (buf_p);
}

static char *make_string_readable(const char *string_p)
{
    return (make_string_readable_length(string_p, strlen(string_p)));
}

static bool is_printable_memory(const char *buf_p, size_t size)
{
    size_t i;

    for (i = 0; i < size; i++) {
        if (!(isprint(buf_p[i]) || isspace(buf_p[i]))) {
            return (false);
        }
    }

    return (true);
}

static bool is_printable_string(const char *buf_p, size_t length)
{
    if (!is_printable_memory(buf_p, length)) {
        return (false);
    }

    return (buf_p[length] == '\0');
}

const char *nala_format_string(const char *prefix_p,
                               const char *actual_p,
                               const char *expected_p)
{
    size_t size;
    char *buf_p;
    FILE *file_p;

    nala_suspend_all_mocks();

    file_p = open_memstream(&buf_p, &size);

    if (actual_p == NULL) {
        fprintf(file_p,
                COLOR_BOLD(RED, "%s Actual string is NULL.\n"),
                prefix_p);
    } else if (expected_p == NULL) {
        fprintf(file_p,
                COLOR_BOLD(RED, "%s Expected string is NULL.\n"),
                prefix_p);
    } else {
        color_start(file_p, ANSI_COLOR_RED);
        fprintf(file_p, "%s", prefix_p);
        fprintf(file_p, " See diff for details.\n");
        color_reset(file_p);
        actual_p = make_string_readable(actual_p);
        expected_p = make_string_readable(expected_p);
        print_string_diff(file_p, expected_p, actual_p);
        free((void *)actual_p);
        free((void *)expected_p);
    }

    fputc('\0', file_p);
    fclose(file_p);

    nala_resume_all_mocks();

    return (buf_p);
}

static void print_with_line_prefix(FILE *file_p,
                                   const char *prefix_p,
                                   const char *string_p)
{
    fprintf(file_p, "%s", prefix_p);

    while (*string_p != '\0') {
        fputc(string_p[0], file_p);

        if (*string_p == '\n') {
            fprintf(file_p, "%s", prefix_p);
        }

        string_p++;
    }
}

static const char *nala_format_substring(const char *prefix_p,
                                         const char *haystack_p,
                                         const char *needle_p)
{
    size_t size;
    char *buf_p;
    FILE *file_p;

    file_p = open_memstream(&buf_p, &size);

    if (haystack_p == NULL) {
        fprintf(file_p, COLOR_BOLD(RED, "The haystack is NULL.\n"));
    } else if (needle_p == NULL) {
        fprintf(file_p, COLOR_BOLD(RED, "The needle is NULL.\n"));
    } else {
        color_start(file_p, ANSI_COLOR_RED);
        fprintf(file_p, "%s", prefix_p);
        fprintf(file_p, " See below for details.\n");
        color_reset(file_p);
        fprintf(file_p, "  Haystack:\n\n");
        haystack_p = make_string_readable(haystack_p);
        print_with_line_prefix(file_p, "    ", haystack_p);
        free((void *)haystack_p);
        fprintf(file_p, "\n\n");
        fprintf(file_p, "  Needle:\n\n");
        needle_p = make_string_readable(needle_p);
        print_with_line_prefix(file_p, "    ", needle_p);
        free((void *)needle_p);
        fprintf(file_p, "\n");
    }

    fputc('\0', file_p);
    fclose(file_p);

    return (buf_p);
}

const char *nala_format_memory(const char *prefix_p,
                               const void *actual_p,
                               const void *expected_p,
                               size_t size)
{
    size_t file_size;
    char *buf_p;
    FILE *file_p;
    char *actual_hexdump_p;
    char *expected_hexdump_p;

    nala_suspend_all_mocks();

    file_p = open_memstream(&buf_p, &file_size);

    if (actual_p == NULL) {
        fprintf(file_p,
                COLOR_BOLD(RED, "%sActual memory pointer is NULL.\n"),
                prefix_p);
    } else if (expected_p == NULL) {
        fprintf(file_p,
                COLOR_BOLD(RED, "%sExpected memory pointer is NULL.\n"),
                prefix_p);
    } else {
        fprintf(file_p,
                COLOR_BOLD(RED, "%sMemory mismatch. See diff for details.\n"),
                prefix_p);
        actual_hexdump_p = nala_hexdump(actual_p, size, 16);
        expected_hexdump_p = nala_hexdump(expected_p, size, 16);
        print_string_diff(file_p, expected_hexdump_p, actual_hexdump_p);
        free(actual_hexdump_p);
        free(expected_hexdump_p);
    }

    fputc('\0', file_p);
    fclose(file_p);

    nala_resume_all_mocks();

    return (buf_p);
}

const char *nala_format_file(const char *prefix_p,
                             const char *actual_p,
                             const void *actual_buf_p,
                             size_t actual_size,
                             const char *expected_p,
                             const void *expected_buf_p,
                             size_t expected_size)
{
    size_t file_size;
    char *buf_p;
    FILE *file_p;
    char *actual_string_p;
    char *expected_string_p;

    nala_suspend_all_mocks();

    file_p = open_memstream(&buf_p, &file_size);

    if (actual_buf_p == NULL) {
        fprintf(file_p,
                COLOR_BOLD(RED, "%sFile mismatch. Failed to read '%s'.\n"),
                prefix_p,
                actual_p);
    } else if (expected_buf_p == NULL) {
        fprintf(file_p,
                COLOR_BOLD(RED, "%sFile mismatch. Failed to read '%s'.\n"),
                prefix_p,
                expected_p);
    } else {
        fprintf(file_p,
                COLOR_BOLD(RED, "%sFile mismatch. See diff for details.\n"),
                prefix_p);

        if (is_printable_memory(actual_buf_p, actual_size)
            && is_printable_memory(expected_buf_p, expected_size)) {
            actual_string_p = make_string_readable_length(actual_buf_p,
                                                          actual_size);
            expected_string_p = make_string_readable_length(expected_buf_p,
                                                            expected_size);
        } else {
            actual_string_p = nala_hexdump(actual_buf_p, actual_size, 16);
            expected_string_p = nala_hexdump(expected_buf_p, expected_size, 16);
        }

        print_string_diff(file_p, expected_string_p, actual_string_p);
        free(actual_string_p);
        free(expected_string_p);
    }

    fputc('\0', file_p);
    fclose(file_p);

    nala_resume_all_mocks();

    return (buf_p);
}

static bool nala_check_substring(const char *haystack_p, const char *needle_p)
{
    if ((haystack_p == NULL) || (needle_p == NULL)) {
        return (false);
    }

    return (strstr(haystack_p, needle_p) != NULL);
}

bool nala_check_memory(const void *actual_p, const void *expected_p, size_t size)
{
    bool are_equal;

    if ((actual_p == NULL) || (expected_p == NULL)) {
        return (false);
    }

    nala_suspend_all_mocks();
    are_equal = (memcmp(actual_p, expected_p, size) == 0);
    nala_resume_all_mocks();

    return (are_equal);
}

static bool traceback_skip_filter(void *arg_p, const char *line_p)
{
    (void)arg_p;

    if (strstr(line_p, " nala.c:") != NULL) {
        return (true);
    }

    if (strstr(line_p, "/nala.c:") != NULL) {
        return (true);
    }

    if (strstr(line_p, "??") != NULL) {
        return (true);
    }

    return (false);
}

void nala_test_failure(const char *message_p)
{
    const char *traceback_p;
    int width;

    nala_suspend_all_mocks();
    nala_capture_output_stop();
    capture_output_destroy(&capture_stdout);
    capture_output_destroy(&capture_stderr);
    print_test_failure_report_begin();

    if (with_message_stack_is_empty(&with_message_stack)) {
        width = 6;
    } else {
        width = 9;
    }

    printf("  Test: " COLOR_BOLD(CYAN, "%*s%s\n"),
           width - 5,
           "",
           full_test_name(current_test_p));
    with_message_stack_print(&with_message_stack);

    if (nala_mock_current_is_set()) {
        printf("  Error: "COLOR_BOLD(RED, "%*sMocked %s(%s): %s\n"),
               width - 6,
               "",
               nala_mock_func_p,
               nala_mock_param_p,
               message_p);
        traceback_p = nala_format_mock_traceback(strdup(""), nala_mock_traceback_p);
        printf("%s", traceback_p);
        free((void *)traceback_p);
    } else {
        printf("  Error: %*s%s", width - 6, "", message_p);
    }

    printf("\n");
    nala_traceback_print("  ",
                         "Assert traceback (most recent call first):",
                         traceback_skip_filter,
                         NULL);
    print_test_failure_report_end();
    free((void *)message_p);
    exit(1);
}

void nala_capture_output_start(char **output_pp, char **errput_pp)
{
    nala_suspend_all_mocks();
    capture_output_start(&capture_stdout, output_pp);
    capture_output_start(&capture_stderr, errput_pp);
    nala_resume_all_mocks();
}

void nala_capture_output_stop(void)
{
    nala_suspend_all_mocks();
    capture_output_stop(&capture_stdout);
    capture_output_stop(&capture_stderr);
    nala_resume_all_mocks();
}

void nala_register_test(struct nala_test_t *test_p)
{
    if (tests.head_p == NULL) {
        tests.head_p = test_p;
    } else {
        tests.tail_p->next_p = test_p;
    }

    tests.tail_p = test_p;
}

int nala_run_tests(void)
{
    return (run_tests(tests.head_p));
}

static void print_usage_and_exit(const char *program_name_p, int exit_code)
{
    printf("usage: %s [-h] [-v] [-c] [-a] [-r] [-f] [-j] [<test-pattern>]\n"
           "\n"
           "Run tests.\n"
           "\n"
           "positional arguments:\n"
           "  test-pattern                  Only run tests matching given pattern. "
           "'^' matches\n"
           "                                the beginning and '$' matches the end "
           "of the test\n"
           "                                name.\n"
           "\n"
           "optional arguments:\n"
           "  -h, --help                    Show this help message and exit.\n"
           "  -v, --version                 Print version information.\n"
           "  -c, --continue-on-failure     Continue on test failure.\n"
           "  -a, --print-all-calls         Print all calls to ease debugging.\n"
           "  -r, --report-json-file        JSON test report file (default: "
           "report.json).\n"
           "  -f, --print-test-file-func    Print file:function for exactly "
           "one test.\n"
           "  -j, --jobs                    Run given number of tests in "
           "parallel\n"
           "                                (default: 1).\n",
           program_name_p);
    exit(exit_code);
}

static void print_version_and_exit(void)
{
    printf("%s\n", NALA_VERSION);
    exit(0);
}

static bool is_test_match(struct nala_test_t *test_p, const char *full_pattern_p)
{
    const char *full_test_name_p;
    size_t full_test_name_length;
    size_t pattern_length;
    size_t offset;
    bool match_beginning;
    bool match_end;
    char *pattern_p;

    pattern_length = strlen(full_pattern_p);

    if (pattern_length == 0) {
        return (true);
    }

    match_beginning = (full_pattern_p[0] == '^');
    match_end = (full_pattern_p[pattern_length - 1] == '$');
    pattern_p = alloca(pattern_length + 1);
    strcpy(pattern_p, full_pattern_p);

    if (match_beginning) {
        pattern_p++;
        pattern_length--;
    }

    if (match_end) {
        pattern_length--;
    }

    full_test_name_p = full_test_name(test_p);
    full_test_name_length = strlen(full_test_name_p);

    if (pattern_length > full_test_name_length) {
        return (false);
    }

    if (match_beginning || match_end) {
        if ((pattern_length == 0) && match_beginning && match_end) {
            return (false);
        }

        if (match_beginning) {
            if (strncmp(full_test_name_p, pattern_p, pattern_length) != 0) {
                return (false);
            }
        }

        if (match_end) {
            offset = (full_test_name_length - pattern_length);

            if (strncmp(&full_test_name_p[offset],
                        pattern_p,
                        pattern_length) != 0) {
                return (false);
            }
        }
    } else if (strstr(full_test_name_p, pattern_p) == NULL) {
        return (false);
    }

    return (true);
}

static void filter_tests(const char *test_pattern_p)
{
    struct nala_test_t *test_p;

    test_p = tests.head_p;
    tests.head_p = NULL;
    tests.tail_p = NULL;

    while (test_p != NULL) {
        if (is_test_match(test_p, test_pattern_p)) {
            nala_register_test(test_p);
        }

        test_p = test_p->next_p;
    }

    if (tests.tail_p != NULL) {
        tests.tail_p->next_p = NULL;
    }
}

static struct nala_test_t *find_test(const char *test_pattern_p)
{
    struct nala_test_t *test_p;
    struct nala_test_t *found_p;

    test_p = tests.head_p;
    found_p = NULL;

    while (test_p != NULL) {
        if (is_test_match(test_p, test_pattern_p)) {
            if (found_p == NULL) {
                found_p = test_p;
            } else {
                fprintf(stderr,
                        "error: '%s' matches more than one test.\n",
                        test_pattern_p);

                return (NULL);
            }
        }

        test_p = test_p->next_p;
    }

    if (found_p == NULL) {
        fprintf(stderr,
                "error: '%s' does not match any test.\n",
                test_pattern_p);

        return (NULL);
    }

    return (found_p);
}

static int print_test_file_func(const char *test_pattern_p)
{
    struct nala_test_t *test_p;

    test_p = find_test(test_pattern_p);

    if (test_p == NULL) {
        return (1);
    }

    printf("%s:%s\n", test_p->file_p, test_p->name_p);

    return (0);
}

static bool mock_traceback_skip_filter(void *arg_p, const char *line_p)
{
    (void)arg_p;

    if (strstr(line_p, "nala.c:") != NULL) {
        return (true);
    }

    if (strstr(line_p, "nala_mocks.c:") != NULL) {
        return (true);
    }

    if (strstr(line_p, "??") != NULL) {
        return (true);
    }

    return (false);
}

char *nala_mock_traceback_format(void **buffer_pp, int depth)
{
    return (nala_traceback_format(buffer_pp,
                                  depth,
                                  "  ",
                                  "Mock traceback (most recent call first):",
                                  mock_traceback_skip_filter,
                                  NULL));
}

#define CHECK_EQ(actual, expected) ((actual) == (expected))

#define CHECK_NE(actual, expected) ((actual) != (expected))

#define CHECK_LT(actual, expected) ((actual) < (expected))

#define CHECK_LE(actual, expected) ((actual) <= (expected))

#define CHECK_GT(actual, expected) ((actual) > (expected))

#define CHECK_GE(actual, expected) ((actual) >= (expected))

#define PRINT_FORMAT(value)                     \
    _Generic((value),                           \
             char: "%c",                        \
             signed char: "%hhd",               \
             unsigned char: "%hhu",             \
             signed short: "%hd",               \
             unsigned short: "%hu",             \
             signed int: "%d",                  \
             unsigned int: "%u",                \
             long int: "%ld",                   \
             unsigned long int: "%lu",          \
             long long int: "%lld",             \
             unsigned long long int: "%llu",    \
             float: "%f",                       \
             double: "%f",                      \
             long double: "%Lf",                \
             const char *: "\"%s\"",            \
             bool: "%d",                        \
             const void *: "%p")

#define PRINT_FORMAT_HEX(value)                 \
    _Generic((value),                           \
             signed char: "%hhx",               \
             unsigned char: "%hhx",             \
             signed short: "%hx",               \
             unsigned short: "%hx",             \
             signed int: "%x",                  \
             unsigned int: "%x",                \
             long int: "%lx",                   \
             unsigned long int: "%lx",          \
             long long int: "%llx",             \
             unsigned long long int: "%llx")

#define ASSERTION(actual, expected, check, format, formatter)   \
    do {                                                        \
        if (!check(actual, expected)) {                         \
            char _nala_assert_format[512];                      \
                                                                \
            nala_suspend_all_mocks();                           \
            snprintf(&_nala_assert_format[0],                   \
                     sizeof(_nala_assert_format),               \
                     format,                                    \
                     PRINT_FORMAT(actual),                      \
                     PRINT_FORMAT(expected));                   \
            nala_test_failure(formatter(_nala_assert_format,    \
                                        actual,                 \
                                        expected));             \
        }                                                       \
    } while (0);

#define ASSERTION_WITH_HEX(actual, expected, check, format, formatter)  \
    do {                                                                \
        if (!check(actual, expected)) {                                 \
            char _nala_assert_format[512];                              \
                                                                        \
            snprintf(&_nala_assert_format[0],                           \
                     sizeof(_nala_assert_format),                       \
                     format,                                            \
                     PRINT_FORMAT(actual),                              \
                     PRINT_FORMAT(expected),                            \
                     PRINT_FORMAT_HEX(actual),                          \
                     PRINT_FORMAT_HEX(expected));                       \
            nala_test_failure(formatter(_nala_assert_format,            \
                                        actual,                         \
                                        expected,                       \
                                        actual,                         \
                                        expected));                     \
        }                                                               \
    } while (0);

#define BINARY_ASSERTION(actual, expected, op)                  \
    switch (op) {                                               \
                                                                \
    case NALA_CHECK_EQ:                                         \
        ASSERTION(actual,                                       \
                  expected,                                     \
                  CHECK_EQ,                                     \
                  "%s is not equal to %s\n",                    \
                  nala_format);                                 \
        break;                                                  \
                                                                \
    case NALA_CHECK_NE:                                         \
        ASSERTION(actual,                                       \
                  expected,                                     \
                  CHECK_NE,                                     \
                  "%s is equal to %s\n",                        \
                  nala_format);                                 \
        break;                                                  \
                                                                \
    case NALA_CHECK_LT:                                         \
        ASSERTION(actual,                                       \
                  expected,                                     \
                  CHECK_LT,                                     \
                  "%s is not less than %s\n",                   \
                  nala_format);                                 \
        break;                                                  \
                                                                \
    case NALA_CHECK_LE:                                         \
        ASSERTION(actual,                                       \
                  expected,                                     \
                  CHECK_LE,                                     \
                  "%s is not less than or equal to %s\n",       \
                  nala_format);                                 \
        break;                                                  \
                                                                \
    case NALA_CHECK_GT:                                         \
        ASSERTION(actual,                                       \
                  expected,                                     \
                  CHECK_GT,                                     \
                  "%s is not greater than %s\n",                \
                  nala_format);                                 \
        break;                                                  \
                                                                \
    case NALA_CHECK_GE:                                         \
        ASSERTION(actual,                                       \
                  expected,                                     \
                  CHECK_GE,                                     \
                  "%s is not greater than or equal to %s\n",    \
                  nala_format);                                 \
        break;                                                  \
                                                                \
    default:                                                    \
        FAIL("Internal nala error.");                           \
        break;                                                  \
    }

#define BINARY_ASSERTION_WITH_HEX(actual, expected, op)                 \
    switch (op) {                                                       \
                                                                        \
    case NALA_CHECK_EQ:                                                 \
        ASSERTION_WITH_HEX(actual,                                      \
                           expected,                                    \
                           CHECK_EQ,                                    \
                           "%s is not equal to %s (0x%s, 0x%s)\n",      \
                           nala_format);                                \
        break;                                                          \
                                                                        \
    case NALA_CHECK_NE:                                                 \
        ASSERTION_WITH_HEX(actual,                                      \
                           expected,                                    \
                           CHECK_NE,                                    \
                           "%s is equal to %s (0x%s, 0x%s)\n",          \
                           nala_format);                                \
        break;                                                          \
                                                                        \
    default:                                                            \
        BINARY_ASSERTION(actual, expected, op);                         \
        break;                                                          \
    }

void nala_assert_char(char actual, char expected, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION(actual, expected, op);
    nala_resume_all_mocks();
}

void nala_assert_schar(signed char actual, signed char expected, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION(actual, expected, op);
    nala_resume_all_mocks();
}

void nala_assert_uchar(unsigned char actual, unsigned char expected, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION(actual, expected, op);
    nala_resume_all_mocks();
}

void nala_assert_short(short actual, short expected, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION_WITH_HEX(actual, expected, op);
    nala_resume_all_mocks();
}

void nala_assert_ushort(unsigned short actual, unsigned short expected, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION_WITH_HEX(actual, expected, op);
    nala_resume_all_mocks();
}

void nala_assert_int(int actual, int expected, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION_WITH_HEX(actual, expected, op);
    nala_resume_all_mocks();
}

void nala_assert_uint(unsigned int actual, unsigned int expected, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION_WITH_HEX(actual, expected, op);
    nala_resume_all_mocks();
}

void nala_assert_long(long actual, long expected, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION_WITH_HEX(actual, expected, op);
    nala_resume_all_mocks();
}

void nala_assert_ulong(unsigned long actual, unsigned long expected, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION_WITH_HEX(actual, expected, op);
    nala_resume_all_mocks();
}

void nala_assert_llong(long long actual, long long expected, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION_WITH_HEX(actual, expected, op);
    nala_resume_all_mocks();
}

void nala_assert_ullong(unsigned long long actual,
                        unsigned long long expected,
                        int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION_WITH_HEX(actual, expected, op);
    nala_resume_all_mocks();
}

void nala_assert_float(float actual, float expected, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION((double)actual, (double)expected, op);
    nala_resume_all_mocks();
}

void nala_assert_double(double actual, double expected, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION(actual, expected, op);
    nala_resume_all_mocks();
}

void nala_assert_ldouble(long double actual, long double expected, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION(actual, expected, op);
    nala_resume_all_mocks();
}

void nala_assert_bool(bool actual, bool expected, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION(actual, expected, op);
    nala_resume_all_mocks();
}

void nala_assert_ptr(const void *actual_p, const void *expected_p, int op)
{
    nala_suspend_all_mocks();
    BINARY_ASSERTION(actual_p, expected_p, op);
    nala_resume_all_mocks();
}

typedef void (*format_array_item_t)(FILE *file_p, const void *value_p);

static char *format_array(const void *buf_p,
                          size_t item_size,
                          size_t size,
                          int i,
                          format_array_item_t format_item)
{
    size_t file_size;
    char *string_p;
    FILE *file_p;
    int length;
    int begin;
    int end;
    const char *c_buf_p;
    const char *delim_p;

    c_buf_p = (const char *)buf_p;
    length = (int)(size / item_size);
    begin = (i - 3);

    if (begin < 0) {
        begin = 0;
    }

    end = (i + 4);

    if (end > length) {
        end = length;
    }

    file_p = open_memstream(&string_p, &file_size);
    fprintf(file_p, "{ ");

    if (begin != 0) {
        fprintf(file_p, "..., ");
    }

    delim_p = "";

    for (i = begin; i < end; i++) {
        fprintf(file_p, "%s", delim_p);
        format_item(file_p, &c_buf_p[i * (int)item_size]);
        delim_p = ", ";
    }

    if (end != length) {
        fprintf(file_p, ", ...");
    }

    fprintf(file_p, " }");
    fputc('\0', file_p);
    fclose(file_p);

    return (string_p);
}

static void assert_array_failure(const void *actual_p,
                                 const void *expected_p,
                                 size_t item_size,
                                 size_t size,
                                 int i,
                                 format_array_item_t format_item)
{
    size_t file_size;
    char *buf_p;
    FILE *file_p;
    char *actual_string_p;
    char *expected_string_p;

    nala_suspend_all_mocks();

    file_p = open_memstream(&buf_p, &file_size);
    fprintf(file_p,
            COLOR_BOLD(RED, "The arrays differ at index %u. See diff for details.\n"),
            (unsigned)i);
    actual_string_p = format_array(actual_p,
                                   item_size,
                                   size,
                                   i,
                                   format_item);
    expected_string_p = format_array(expected_p,
                                     item_size,
                                     size,
                                     i,
                                     format_item);
    print_string_diff(file_p, expected_string_p, actual_string_p);
    free(actual_string_p);
    free(expected_string_p);
    fputc('\0', file_p);
    fclose(file_p);

    nala_resume_all_mocks();

    nala_test_failure(buf_p);
}

static void format_array_item_char(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%hhd", *(char *)value_p);
}

static void format_array_item_schar(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%hhd", *(signed char *)value_p);
}

static void format_array_item_uchar(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%hhu", *(unsigned char *)value_p);
}

static void format_array_item_short(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%hd", *(short *)value_p);
}

static void format_array_item_ushort(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%hu", *(unsigned short *)value_p);
}

static void format_array_item_int(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%d", *(int *)value_p);
}

static void format_array_item_uint(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%u", *(unsigned *)value_p);
}

static void format_array_item_long(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%ld", *(long *)value_p);
}

static void format_array_item_ulong(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%lu", *(unsigned long *)value_p);
}

static void format_array_item_llong(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%lld", *(long long *)value_p);
}

static void format_array_item_ullong(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%llu", *(unsigned long long *)value_p);
}

static void format_array_item_float(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%f", (double)(*(float *)value_p));
}

static void format_array_item_double(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%f", *(double *)value_p);
}

static void format_array_item_ldouble(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%Lf", *(long double *)value_p);
}

static void format_array_item_bool(FILE *file_p, const void *value_p)
{
    fprintf(file_p, "%d", *(bool *)value_p);
}

#define ASSERT_ARRAY_TYPE(type, actual_p, expected_p, item_size, size)  \
    size_t i;                                                           \
                                                                        \
    nala_suspend_all_mocks();                                           \
                                                                        \
    for (i = 0; i < size / item_size; i++) {                            \
        if (actual_p[i] != expected_p[i]) {                             \
            assert_array_failure(actual_p,                              \
                                 expected_p,                            \
                                 item_size,                             \
                                 size,                                  \
                                 (int)i,                                \
                                 format_array_item_ ## type);           \
        }                                                               \
    }                                                                   \
                                                                        \
    nala_resume_all_mocks();

void nala_assert_array_char(const char *actual_p,
                             const char *expected_p,
                             size_t item_size,
                             size_t size)
{
    ASSERT_ARRAY_TYPE(char, actual_p, expected_p, item_size, size);
}

void nala_assert_array_schar(const signed char *actual_p,
                              const signed char *expected_p,
                              size_t item_size,
                              size_t size)
{
    ASSERT_ARRAY_TYPE(schar, actual_p, expected_p, item_size, size);
}

void nala_assert_array_uchar(const unsigned char *actual_p,
                              const unsigned char *expected_p,
                              size_t item_size,
                              size_t size)
{
    ASSERT_ARRAY_TYPE(uchar, actual_p, expected_p, item_size, size);
}

void nala_assert_array_short(const short *actual_p,
                              const short *expected_p,
                              size_t item_size,
                              size_t size)
{
    ASSERT_ARRAY_TYPE(short, actual_p, expected_p, item_size, size);
}

void nala_assert_array_ushort(const unsigned short *actual_p,
                               const unsigned short *expected_p,
                               size_t item_size,
                               size_t size)
{
    ASSERT_ARRAY_TYPE(ushort, actual_p, expected_p, item_size, size);
}

void nala_assert_array_int(const int *actual_p,
                            const int *expected_p,
                            size_t item_size,
                            size_t size)
{
    ASSERT_ARRAY_TYPE(int, actual_p, expected_p, item_size, size);
}

void nala_assert_array_uint(const unsigned int *actual_p,
                             const unsigned int *expected_p,
                             size_t item_size,
                             size_t size)
{
    ASSERT_ARRAY_TYPE(uint, actual_p, expected_p, item_size, size);
}

void nala_assert_array_long(const long *actual_p,
                             const long *expected_p,
                             size_t item_size,
                             size_t size)
{
    ASSERT_ARRAY_TYPE(long, actual_p, expected_p, item_size, size);
}

void nala_assert_array_ulong(const unsigned long *actual_p,
                              const unsigned long *expected_p,
                              size_t item_size,
                              size_t size)
{
    ASSERT_ARRAY_TYPE(ulong, actual_p, expected_p, item_size, size);
}

void nala_assert_array_llong(const long long *actual_p,
                              const long long *expected_p,
                              size_t item_size,
                              size_t size)
{
    ASSERT_ARRAY_TYPE(llong, actual_p, expected_p, item_size, size);
}

void nala_assert_array_ullong(const unsigned long long *actual_p,
                               const unsigned long long *expected_p,
                               size_t item_size,
                               size_t size)
{
    ASSERT_ARRAY_TYPE(ullong, actual_p, expected_p, item_size, size);
}

void nala_assert_array_float(const float *actual_p,
                              const float *expected_p,
                              size_t item_size,
                              size_t size)
{
    ASSERT_ARRAY_TYPE(float, actual_p, expected_p, item_size, size);
}

void nala_assert_array_double(const double *actual_p,
                               const double *expected_p,
                               size_t item_size,
                               size_t size)
{
    ASSERT_ARRAY_TYPE(double, actual_p, expected_p, item_size, size);
}

void nala_assert_array_ldouble(const long double *actual_p,
                                const long double *expected_p,
                                size_t item_size,
                                size_t size)
{
    ASSERT_ARRAY_TYPE(ldouble, actual_p, expected_p, item_size, size);
}

void nala_assert_array_bool(const bool *actual_p,
                             const bool *expected_p,
                             size_t item_size,
                             size_t size)
{
    ASSERT_ARRAY_TYPE(bool, actual_p, expected_p, item_size, size);
}

void nala_assert_array(const void *actual_p,
                       const void *expected_p,
                       size_t item_size,
                       size_t size)
{
    const char *c_actual_p;
    const char *c_expected_p;
    size_t i;
    char buf[512];

    nala_suspend_all_mocks();

    c_actual_p = (const char *)actual_p;
    c_expected_p = (const char *)expected_p;

    for (i = 0; i < size; i += item_size) {
        if (memcmp(&c_actual_p[i], &c_expected_p[i], item_size) != 0) {
            snprintf(&buf[0],
                     sizeof(buf),
                     "The arrays differ at index %u. ",
                     (unsigned)(i / item_size));
            nala_test_failure(nala_format_memory(&buf[0],
                                                 &c_actual_p[i],
                                                 &c_expected_p[i],
                                                 item_size));
        }
    }

    nala_resume_all_mocks();
}

void nala_assert_string(const char *actual_p, const char *expected_p, int op)
{
    nala_suspend_all_mocks();

    switch (op) {

    case NALA_CHECK_EQ:
        if (!nala_check_string_equal(actual_p, expected_p)) {
            nala_test_failure(nala_format_string("The strings are not equal.",
                                                 actual_p,
                                                 expected_p));
        }

        break;

    case NALA_CHECK_NE:
        if (nala_check_string_equal(actual_p, expected_p)) {
            nala_test_failure(nala_format("\"%s\" == \"%s\"\n",
                                          actual_p,
                                          expected_p));
        }

        break;

    default:
        FAIL("Internal nala error.");
        break;
    }

    nala_resume_all_mocks();
}

void nala_assert_substring(const char *haystack_p, const char *needle_p)
{
    nala_suspend_all_mocks();

    if (!nala_check_substring(haystack_p, needle_p)) {
        nala_test_failure(
            nala_format_substring(
                "The haystack doesn't contain the needle.",
                haystack_p,
                needle_p));
    }

    nala_resume_all_mocks();
}

void nala_assert_not_substring(const char *haystack_p, const char *needle_p)
{
    nala_suspend_all_mocks();

    if (nala_check_substring(haystack_p, needle_p)) {
        nala_test_failure(
            nala_format_substring(
                "The haystack contains the needle.",
                haystack_p,
                needle_p));
    }

    nala_resume_all_mocks();
}

void nala_assert_memory(const void *actual_p, const void *expected_p, size_t size)
{
    if (!nala_check_memory(actual_p, expected_p, size)) {
        nala_test_failure(nala_format_memory("", actual_p, expected_p, size));
    }
}

static bool check_string_or_memory(const void *actual_p,
                                   const void *expected_p,
                                   size_t size)
{
    size_t length;

    if ((actual_p == NULL) || (expected_p == NULL)) {
        return (false);
    }

    length = strnlen(actual_p, size);

    if (length < size) {
        if (memcmp(actual_p, expected_p, length + 1) != 0) {
            return (false);
        }
    }

    return (memcmp(actual_p, expected_p, size) == 0);
}

static bool are_strings(const char **actual_pp, const char *expected_p, size_t size)
{
    size_t length;
    char *actual_p;

    if ((expected_p == NULL) || (*actual_pp == NULL)) {
        return (false);
    }

    if (!is_printable_string(expected_p, size - 1)) {
        return (false);
    }

    length = strnlen(*actual_pp, size);

    if (length == size) {
        actual_p = malloc(length + 5);
        memcpy(actual_p, *actual_pp, length);
        strcpy(&actual_p[length], " ...");

        if (!is_printable_string(actual_p, length + 4)) {
            free(actual_p);

            return (false);
        }

        *actual_pp = actual_p;

        return (true);
    } else {
        return (is_printable_string(*actual_pp, length));
    }
}

void nala_assert_string_or_memory(const void *actual_p,
                                  const void *expected_p,
                                  size_t size)
{
    nala_suspend_all_mocks();

    if (!check_string_or_memory(actual_p, expected_p, size)) {
        if (are_strings((const char **)&actual_p, expected_p, size)) {
            nala_test_failure(nala_format_string("The strings are not equal.",
                                                 actual_p,
                                                 expected_p));
        } else {
            nala_test_failure(nala_format_memory("", actual_p, expected_p, size));
        }
    }

    nala_resume_all_mocks();
}

void nala_assert_file_eq(const char *actual_p, const char *expected_p)
{
    void *actual_buf_p;
    size_t actual_size;
    void *expected_buf_p;
    size_t expected_size;

    nala_suspend_all_mocks();

    actual_buf_p = nala_hf_file_read_all(actual_p, &actual_size);
    expected_buf_p = nala_hf_file_read_all(expected_p, &expected_size);

    if ((actual_buf_p == NULL)
        || (expected_buf_p == NULL)
        || (actual_size != expected_size)
        || !nala_check_memory(actual_buf_p, expected_buf_p, expected_size)) {
        nala_test_failure(nala_format_file("",
                                           actual_p,
                                           actual_buf_p,
                                           actual_size,
                                           expected_p,
                                           expected_buf_p,
                                           expected_size));
    }

    free(actual_buf_p);
    free(expected_buf_p);

    nala_resume_all_mocks();
}

void nala_assert_true(bool actual)
{
    if (!actual) {
        nala_test_failure(nala_format("False is not true.\n"));
    }
}

void nala_assert_false(bool actual)
{
    if (actual) {
        nala_test_failure(nala_format("True is not false.\n"));
    }
}

void nala_assert(bool cond)
{
    if (!cond) {
        nala_test_failure(nala_format("The condition is not true.\n"));
    }
}

void nala_fail(const char *message_p)
{
    nala_suspend_all_mocks();
    char message[strlen(message_p) + 2];
    strcpy(&message[0], message_p);
    strcat(&message[0], "\n");
    nala_test_failure(nala_format(&message[0]));
}

void nala_with_message_push(const char *format_p, ...)
{
    char *message_p;
    FILE *file_p;
    size_t size;
    va_list vl;

    nala_suspend_all_mocks();
    file_p = open_memstream(&message_p, &size);
    va_start(vl, format_p);
    vfprintf(file_p, format_p, vl);
    va_end(vl);
    fputc('\0', file_p);
    fclose(file_p);
    with_message_stack_push(&with_message_stack, message_p);
    nala_resume_all_mocks();
}

void nala_with_message_pop(void)
{
    nala_suspend_all_mocks();
    with_message_stack_pop(&with_message_stack);
    nala_resume_all_mocks();
}

void *nala_alloc(size_t size)
{
    void *buf_p;

    nala_suspend_all_mocks();
    buf_p = malloc(size);
    ASSERT_NE(buf_p, NULL);
    nala_auto_free(buf_p);
    nala_resume_all_mocks();

    return (buf_p);
}

void nala_auto_free(void *buf_p)
{
    nala_suspend_all_mocks();
    auto_free_list_append(&auto_free_list, buf_p);
    nala_resume_all_mocks();
}

void nala_exit(int status)
{
    (void)status;

    nala_suspend_all_mocks();
    nala_assert_all_mocks_completed();
    capture_output_destroy(&capture_stdout);
    capture_output_destroy(&capture_stderr);
    auto_free_list_destroy(&auto_free_list);
    exit(0);
}

int main(int argc, char *argv[])
{
    static struct option long_options[] = {
        { "help",                 no_argument,       NULL, 'h' },
        { "version",              no_argument,       NULL, 'v' },
        { "continue-on-failure",  no_argument,       NULL, 'c' },
        { "print-all-calls",      no_argument,       NULL, 'a' },
        { "report-json-file",     required_argument, NULL, 'r' },
        { "print-test-file-func", required_argument, NULL, 'f' },
        { "jobs",                 required_argument, NULL, 'j' },
        { NULL,                   no_argument,       NULL, 0 }
    };
    int option;

    /* Do not print function calls outside tests. */
    nala_suspend_all_mocks();

    while (1) {
        option = getopt_long(argc, argv, "hvcar:f:j:", &long_options[0], NULL);

        if (option == -1) {
            break;
        }

        switch (option) {

        case 'h':
            print_usage_and_exit(argv[0], 0);
            break;

        case 'v':
            print_version_and_exit();
            break;

        case 'c':
            continue_on_failure = true;
            break;

        case 'a':
            nala_print_call_mask = 0xff;
            break;

        case 'r':
            report_json_file_p = optarg;
            break;

        case 'f':
            return (print_test_file_func(optarg));

        case 'j':
            number_of_jobs = atoi(optarg);

            if (number_of_jobs < 1) {
                printf("error: At least one job is required, %d given.\n",
                       number_of_jobs);
                exit(1);
            }

            break;

        default:
            print_usage_and_exit(argv[0], 1);
            break;
        }
    }

    if (optind < argc) {
        filter_tests(argv[optind]);
    }

    return (nala_run_tests());
}
