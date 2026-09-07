# Systems Programming — Exercise 2: `mini_shell`

A minimal command-line interpreter ("Mini-Bash Shell") built directly on
top of POSIX system calls — `fork`, `execv`, `waitpid`, `chdir`, `access` —
with no use of high-level wrappers such as `system()`, and no threads.

The shell runs an infinite prompt/read/parse/execute loop:

- **Internal commands:** `exit` (ends the shell), `cd` (changes the shell's
  own working directory via `chdir()`).
- **External commands:** looked up first in `$HOME/<command>`, then in
  `/bin/<command>` (each checked for existence *and* execute permission via
  `access(path, X_OK)`); if found, run via `fork()` + `execv()`, with the
  parent shell blocking on `waitpid()` until the child finishes and then
  reporting its exit code. If not found anywhere, prints
  `[<command>]: Unknown Command`.

See [`DESIGN_DOCUMENT.md`](DESIGN_DOCUMENT.md) for the full design
rationale, system-call-by-system-call analysis, program flow, and error
handling.

## Build

```bash
make
```

Requires `gcc` on Linux (or WSL/Ubuntu on Windows, per the assignment).

## Usage

```bash
./mini_shell
```

Example session:

```
mini-bash$ ls -l
...
ls finished successfully (exit code: 0)
mini-bash$ cd /tmp
mini-bash$ doesnotexist
[doesnotexist]: Unknown Command
mini-bash$ exit
```

## Clean

```bash
make clean
```
