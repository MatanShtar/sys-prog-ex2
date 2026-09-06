/*
 * mini_shell.c
 *
 * A minimal command-line interpreter ("Mini-Bash Shell") built directly on
 * top of POSIX system calls. The shell repeatedly prints a prompt, reads a
 * line of input, splits it into a command and arguments, and either runs it
 * itself (an "internal" command: exit / cd) or locates an executable file
 * for it and runs it as a child process (an "external" command).
 *
 * External command lookup order:
 *   1. $HOME/<command>   (must exist AND be executable)
 *   2. /bin/<command>    (must exist AND be executable)
 *   3. otherwise -> "<command>: Unknown Command"
 *
 * Process management for external commands uses the classic Unix trio:
 *   fork()    - create a child process (a near-exact copy of the shell)
 *   execv()   - replace the child's program image with the target program
 *   waitpid() - the parent (shell) blocks until that specific child exits
 *
 * Usage:
 *   ./mini_shell
 *
 * Build:
 *   gcc -Wall -Wextra -std=gnu11 -O2 mini_shell.c -o mini_shell
 */

#include <stdio.h>      /* printf, fprintf, perror, getline */
#include <stdlib.h>     /* malloc/free (via getline), EXIT_SUCCESS */
#include <string.h>     /* strcmp, strtok_r */
#include <unistd.h>     /* fork, execv, chdir, access, X_OK */
#include <sys/wait.h>   /* waitpid, WIFEXITED, WEXITSTATUS, ... */
#include <errno.h>      /* errno */
#include <limits.h>     /* PATH_MAX */

/* Fixed prompt string printed before every read. */
#define PROMPT "mini-bash$ "

/* Field separators for tokenizing a command line, per the assignment:
 * a run of spaces and/or tabs separates tokens. */
#define DELIMITERS " \t"

/* Maximum number of tokens (command + arguments) accepted on one line,
 * including the terminating NULL slot required by execv()'s argv[]. */
#define MAX_ARGS 64

/*
 * split_into_tokens
 *
 * Splits 'line' in place into whitespace-separated tokens and stores
 * pointers to each token inside argv[]. No characters are copied: every
 * argv[i] simply points somewhere inside the original 'line' buffer, with
 * strtok_r() overwriting the separator right after each token with '\0' to
 * terminate it. This is the "avoid unnecessary copies" approach the
 * assignment asks for - one buffer holds the raw line and doubles as
 * storage for every token in it.
 *
 * argv[] is NULL-terminated on return (as execv() requires), so at most
 * max_args - 1 real tokens are stored.
 *
 * Returns the number of tokens found (0 for a blank/whitespace-only line).
 */
static int split_into_tokens(char *line, char *argv[], int max_args)
{
    int argc = 0;
    char *saveptr = NULL; /* strtok_r's internal position, so the function
                            * has no hidden global state (unlike strtok)
                            * and is safe to reason about/reuse per call. */

    char *token = strtok_r(line, DELIMITERS, &saveptr);
    while (token != NULL && argc < max_args - 1) {
        argv[argc++] = token;
        token = strtok_r(NULL, DELIMITERS, &saveptr);
    }
    argv[argc] = NULL;

    return argc;
}

/*
 * build_candidate_path
 *
 * Writes "<dir>/<command>" into full_path (a caller-owned buffer of size
 * full_path_size) and checks with access(..., X_OK) whether that path
 * exists AND is executable by this process - a single system call answers
 * both questions at once ("not enough for it to merely exist", per the
 * assignment). Using access() here (rather than immediately attempting to
 * exec it) is what lets the shell try $HOME first and fall back to /bin
 * without forking a process for a path that will not be usable.
 *
 * Returns 1 if the candidate is usable, 0 otherwise (path too long to fit
 * the buffer, does not exist, or is not executable).
 */
static int build_candidate_path(const char *dir, const char *command,
                                 char *full_path, size_t full_path_size)
{
    int written = snprintf(full_path, full_path_size, "%s/%s", dir, command);

    /* snprintf returns the length the string WOULD have needed; if that is
     * >= the buffer size, the path was truncated and is not trustworthy. */
    if (written < 0 || (size_t)written >= full_path_size) {
        return 0;
    }

    return access(full_path, X_OK) == 0;
}

/*
 * run_external_command
 *
 * Implements the two-stage search (HOME, then /bin) and, if found,
 * launches the program via fork/exec/wait as required by the assignment.
 */
static void run_external_command(char *argv[], const char *home)
{
    char full_path[PATH_MAX];
    int found = 0;

    /* Stage A: home directory, only if HOME is actually set. */
    if (home != NULL && build_candidate_path(home, argv[0], full_path, sizeof(full_path))) {
        found = 1;
    }
    /* Stage B: /bin. */
    else if (build_candidate_path("/bin", argv[0], full_path, sizeof(full_path))) {
        found = 1;
    }

    /* Stage C: not found anywhere - report per the required format. */
    if (!found) {
        fprintf(stderr, "%s: Unknown Command\n", argv[0]);
        return;
    }

    /* fork(): the kernel makes a near-complete duplicate of this process
     * (same code, same open files, a copy of the address space). It
     * returns twice: 0 in the new child, and the child's PID in the
     * parent. Returning -1 means the kernel could not create the process
     * (e.g. process/resource limits) and no child was created at all. */
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        /* --- Child process ---
         * execv() replaces this process's program image (code, data,
         * stack) with the one at full_path, keeping the same PID and
         * file descriptors. argv[] becomes the new program's argument
         * vector; the NULL sentinel split_into_tokens() left in place
         * tells execv() where the array ends.
         *
         * execv() only RETURNS if it failed (there is no "after exec"
         * for a successful call - the old program image is gone). So
         * reaching the lines below always means an error occurred. */
        execv(full_path, argv);

        perror("execv");
        /* _exit() (not exit()) skips stdio flushing/atexit handlers that
         * belong to the PARENT shell process; this child is a duplicate
         * of that process image and must not re-run its cleanup. Exit
         * code 127 is the conventional "command could not be executed"
         * status used by real shells. */
        _exit(127);
    }

    /* --- Parent process (the shell itself) ---
     * waitpid(pid, &status, 0) blocks until the specific child 'pid'
     * changes state (here: terminates), preventing the shell from
     * printing its next prompt while the command is still running, and
     * reaping the child so it does not linger as a zombie process. */
    int status;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid");
        return;
    }

    if (WIFEXITED(status)) {
        /* Child ran to completion and called exit()/returned from main();
         * WEXITSTATUS extracts its 8-bit return code from 'status'. */
        printf("%s finished successfully (exit code: %d)\n", argv[0], WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        /* Child was killed by a signal instead of exiting normally. */
        printf("%s was terminated by signal %d\n", argv[0], WTERMSIG(status));
    }
}

/*
 * run_cd
 *
 * Internal command: changes the shell's OWN current working directory.
 * This must be done by chdir() inside the shell process itself (never via
 * fork+exec) precisely because a forked child's working-directory change
 * would die with that child and never affect the shell or the commands
 * typed after it.
 *
 * With no argument, defaults to $HOME (standard shell behavior).
 */
static void run_cd(char *argv[], const char *home)
{
    const char *target = argv[1];

    if (target == NULL) {
        if (home == NULL) {
            fprintf(stderr, "cd: HOME environment variable is not set\n");
            return;
        }
        target = home;
    }

    /* chdir() asks the kernel to change this process's current working
     * directory; -1 means it failed (e.g. path does not exist, is not a
     * directory, or permission is denied) and sets errno accordingly. */
    if (chdir(target) == -1) {
        perror("cd");
    }
}

int main(void)
{
    /* getline() manages its own heap buffer for us: 'line' starts as NULL
     * with capacity 0, and getline() (re)allocates it as needed. Because
     * both variables persist across loop iterations, the SAME buffer is
     * reused for every command line typed - getline() only grows it (via
     * realloc) if a longer line arrives, so there is no repeated
     * malloc/free churn per prompt. */
    char *line = NULL;
    size_t line_capacity = 0;

    /* The lookup rule requires knowing the user's home directory; per the
     * assignment's hint, that is the HOME environment variable. Read it
     * once - it will not change for the life of this shell process. */
    const char *home = getenv("HOME");

    char *argv[MAX_ARGS];

    for (;;) {
        printf("%s", PROMPT);
        fflush(stdout); /* stdout is line-buffered/full-buffered depending
                          * on the terminal; without this, the prompt could
                          * stay stuck in the buffer instead of appearing
                          * before we block waiting for input. */

        ssize_t line_length = getline(&line, &line_capacity, stdin);
        if (line_length == -1) {
            /* getline() signals both real errors and end-of-file (e.g. the
             * user pressed Ctrl+D) the same way: -1. feof() disambiguates. */
            if (feof(stdin)) {
                printf("\n");
            } else {
                perror("getline");
            }
            break;
        }

        /* getline() includes the trailing '\n' in the buffer; strip it so
         * it does not become part of the last argument. */
        if (line_length > 0 && line[line_length - 1] == '\n') {
            line[line_length - 1] = '\0';
        }

        int argc = split_into_tokens(line, argv, MAX_ARGS);
        if (argc == 0) {
            continue; /* Blank line: nothing to parse or run. */
        }

        if (strcmp(argv[0], "exit") == 0) {
            break; /* Internal command: stop the loop and end the program. */
        }

        if (strcmp(argv[0], "cd") == 0) {
            run_cd(argv, home);
            continue;
        }

        run_external_command(argv, home);
    }

    free(line); /* Release getline()'s buffer before the process exits. */
    return EXIT_SUCCESS;
}
