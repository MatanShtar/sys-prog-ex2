# Design Document — Mini-Bash Shell (`mini_shell.c`)

Systems Programming, Exercise 2

**GitHub Repository:** https://github.com/MatanShtar/sys-prog-ex2
(public; contains the source file `mini_shell.c`, the `Makefile`, and the
compiled executable `mini_shell`.)

## 1. Purpose and Requirements

The goal of this exercise is to implement a minimal command-line interpreter
("Mini-Bash Shell") in C that talks **directly to the Linux kernel through
system calls**, without relying on high-level wrapper functions such as
`system()`. The purpose is to understand, first-hand, how an operating
system manages processes: how a shell creates new processes, how it loads a
different program into a process, and how processes communicate/synchronize
by waiting on each other.

`mini_shell` satisfies the assignment's requirements as follows:

| Requirement | How it is satisfied |
|---|---|
| Infinite prompt/read/parse/execute loop | `main()`'s `for (;;)` loop |
| Fixed prompt string | `PROMPT = "mini-bash$ "` |
| Read one line of input | `getline()` |
| Tokenize on spaces/tabs | `split_into_tokens()` using `strtok_r` with `" \t"` |
| Internal command `exit` | Checked first in the loop; breaks out |
| Internal command `cd` (via `chdir`) | `run_cd()` |
| External command search: `$HOME` then `/bin` | `run_external_command()` + `build_candidate_path()` |
| Must verify the file is executable, not just present | `access(path, X_OK)` |
| Unknown command message | `"[%s]: Unknown Command\n"` to `stderr` |
| Process creation via `fork` | `fork()` in `run_external_command()` |
| Program replacement via `exec` family | `execv()` |
| Parent waits for child | `waitpid(pid, &status, 0)` |
| Report success + return code | `WIFEXITED`/`WEXITSTATUS` printed after `waitpid` |
| `exec` failure reported with `perror()` | `perror("execv")` right after the failed call |
| Efficient buffer use, no unnecessary copies | Single reused `getline()` buffer; `strtok_r` tokenizes in place (no `strdup`/copies) |
| No threads | Only `fork()`-based processes are used |
| No high-level wrappers (`system()`, etc.) | Only raw POSIX calls: `fork`, `execv`, `waitpid`, `chdir`, `access` |

## 2. System Calls Used — Analysis

### `fork()`
```c
pid_t pid = fork();
```
Creates a new process that is a near-exact duplicate of the calling
process (same code, same heap/stack contents at the moment of the call,
same open file descriptor table). It is the *only* way to create a new
process on POSIX systems from inside a running program (there is no
"create and start a new program" call — that is deliberately split into
`fork` + `exec`).

**Return value handling:**
- `-1`: no child was created (e.g. system-wide or per-user process limit
  reached). Handled with `perror("fork")`, and the shell simply gives up on
  running this command and returns to the prompt — it does **not** exit
  the whole shell, since a failure to launch one command should not crash
  the interactive session.
- `0`: this is the **new child process**. It proceeds to call `execv`.
- `> 0`: this is the **original (parent) process**; the value is the
  child's PID, which is retained so it can be passed to `waitpid`.

### `execv(const char *path, char *const argv[])`
```c
execv(full_path, argv);
```
Replaces the calling process's program image (code, data segment, heap,
stack) with the executable found at `path`, while keeping the same PID and
the same open file descriptor table (so the child's stdin/stdout/stderr
stay connected to the terminal the shell was launched from). `argv[]`
becomes the new program's `argv` in its own `main()`; it must be
`NULL`-terminated, which `split_into_tokens()` guarantees.

**Return value handling:** `execv` **has no successful return** — a
successful call never comes back to the calling code, because that code no
longer exists in memory. Therefore, the only way `execv()` "returns" is on
**failure** (returns `-1`, sets `errno`). This is handled by calling
`perror("execv")` (which prints the kernel's exact reason, e.g. "Permission
denied" or "Exec format error") immediately followed by `_exit(127)` to
terminate the now-broken child process, since it must not be allowed to
fall through and duplicate the parent shell's loop.

### `waitpid(pid_t pid, int *status, int options)`
```c
if (waitpid(pid, &status, 0) == -1) { perror("waitpid"); return; }
```
Called by the **parent only**, with the specific child's PID and `options =
0` (a plain, fully-blocking wait — no `WNOHANG`). This blocks the shell
until that exact child terminates, which is required so the shell does not
print the next prompt (or accept new input) while the previous command is
still running. It also **reaps** the child, removing its zombie entry from
the process table. `status` is filled with an implementation-defined
encoding that must be read through macros:
- `WIFEXITED(status)` / `WEXITSTATUS(status)` — the child called `exit()`
  or returned from `main`; extracts its 0–255 exit code.
- `WIFSIGNALED(status)` / `WTERMSIG(status)` — the child was killed by a
  signal instead.

**Return value handling:** `-1` means `waitpid` itself failed (e.g. `pid`
does not refer to a valid/owned child), reported with `perror("waitpid")`.

### `chdir(const char *path)`
```c
if (chdir(target) == -1) { perror("cd"); }
```
Changes the **calling process's own** current working directory, which is
per-process kernel state inherited by children at `fork()` time but never
propagated back up from child to parent. This is precisely why `cd` must be
implemented as an internal command executed directly by the shell — running
it inside a forked child (as an external command would be) would change
only that child's working directory, which disappears the moment the child
exits, leaving the shell's own directory (and therefore every subsequent
command) unaffected.

**Return value handling:** `-1` (path does not exist, is not a directory,
or permission denied) is reported with `perror("cd")`; the shell continues
running with its working directory unchanged.

### `access(const char *path, int mode)`
```c
return access(full_path, X_OK) == 0;
```
Used with `X_OK` to check, in a single call, whether `path` refers to a
file that exists **and** that this process is permitted to execute. The
assignment explicitly calls out that mere existence is not sufficient
("it's not enough for it to merely exist"), which `access(..., X_OK)`
checks directly rather than requiring a separate existence check (e.g.
`stat`) plus a manual permission-bit comparison.

**Return value handling:** `0` means usable; `-1` means not usable (does
not exist, or exists but is not executable by this process) — in that case
`build_candidate_path()` simply reports "not found" and the caller moves on
to the next search location, with no error message printed (a missing file
in `$HOME` is an expected, ordinary outcome of the search algorithm, not an
error condition).

### `getline(char **lineptr, size_t *n, FILE *stream)`
Not a raw system call (it is a POSIX C library function layered over
`read()`), but it is the mechanism used to satisfy the "Read" step and the
buffer-efficiency requirement — see §4 below.

## 3. Program Flow

```
                         ┌─────────────────────────┐
                         │   print "mini-bash$ "    │
                         └────────────┬─────────────┘
                                      ▼
                         ┌─────────────────────────┐
                         │  getline() from stdin    │──── EOF/error ──► free(line); exit
                         └────────────┬─────────────┘
                                      ▼
                         ┌─────────────────────────┐
                         │ strip trailing '\n'      │
                         │ split_into_tokens()       │
                         └────────────┬─────────────┘
                                      ▼
                              argc == 0 ? ──yes──► (loop back to prompt)
                                      │no
                                      ▼
                         argv[0] == "exit" ? ──yes──► break loop, free(line), return
                                      │no
                                      ▼
                         argv[0] == "cd" ? ──yes──► chdir(argv[1] or HOME) ──► (loop back)
                                      │no
                                      ▼
                    ┌─────────────────────────────────────────┐
                    │ access($HOME/argv[0], X_OK) ?            │
                    └───────────────┬───────────────┬─────────┘
                                 yes│               │no
                                    ▼               ▼
                              full_path found   access(/bin/argv[0], X_OK) ?
                                    │            yes│          │no
                                    │                ▼          ▼
                                    │          full_path found  print "[argv[0]]: Unknown Command"
                                    │                │           (loop back to prompt)
                                    └───────┬────────┘
                                            ▼
                                        fork()
                                    ┌───────┴────────┐
                              child │                │ parent
                                    ▼                ▼
                          execv(full_path, argv)   waitpid(pid, &status, 0)
                          (only returns on error:        │
                           perror + _exit(127))           ▼
                                                  print exit code / signal
                                                           │
                                                           ▼
                                                   (loop back to prompt)
```

## 4. Efficiency: System Call / Copy Count

The design deliberately minimizes both system calls and in-memory copies
per command:

- **Input buffer reuse.** `line`/`line_capacity` are declared once, outside
  the loop, and passed to `getline()` on every iteration. `getline()` only
  calls `realloc()` (internally) when a line longer than the current
  capacity arrives; a session of similarly-sized commands performs **at
  most one allocation for the whole session**, not one per prompt.
- **Zero-copy tokenizing.** `split_into_tokens()` uses `strtok_r` directly
  on the `getline()` buffer. Every `argv[i]` is a pointer *into* that same
  buffer; no token is ever `strdup`'d or copied into a separate string.
- **Search short-circuits.** The `$HOME` → `/bin` search stops at the first
  match (`else if`), so at most **two `access()` calls** are made per
  external command (one if the command is found in `$HOME`), and **zero**
  `fork`/`execv`/`waitpid` calls are wasted looking up a command that turns
  out not to exist anywhere — the existence/permission check happens
  *before* any process is created.
- **Exactly one child per external command.** There is a single `fork()`
  call, a single `execv()` call in the child, and a single blocking
  `waitpid()` call in the parent — no retry loops, no polling
  (`waitpid` is called with `options = 0`, a blocking wait, instead of
  spin-looping on `WNOHANG`).
- **No `system()`.** `system()` would internally `fork()` **and** `exec` a
  full `/bin/sh -c "..."`, i.e. an extra shell process wrapping the real
  target program — double the process-creation cost for the same effect.
  Calling `fork`/`execv` directly on the resolved path avoids that
  entirely.

## 5. Error Handling

Every system call in this program is checked immediately after it
executes, and every failure is reported through `perror()` so the exact
kernel-provided reason (translated from `errno`) reaches the user, per the
assignment's requirement:

| Call | Failure meaning | Handling |
|---|---|---|
| `getline` | read error (not EOF) | `perror("getline")`, loop ends |
| `fork` | process limit / out of resources | `perror("fork")`, command aborted, shell keeps running |
| `execv` | file not actually executable/valid, or vanished after the `access()` check | `perror("execv")` in the child, then `_exit(127)` |
| `waitpid` | invalid/unknown pid | `perror("waitpid")`, command's outcome is simply not reported |
| `chdir` | target missing / not a directory / no permission | `perror("cd")`, working directory left unchanged |
| `access` (both search stages failing) | not an "error" — an expected outcome of the search algorithm | `"[<command>]: Unknown Command"` printed to `stderr`, no process created |

In every failure case, the shell **recovers and returns to the prompt**
rather than terminating — a single bad command, missing file, or invalid
`cd` target must not bring down the interactive session, mirroring how a
real shell behaves.
