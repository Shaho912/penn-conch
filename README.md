# Penn Conch

A Unix shell written in C — multi-process pipelines, job control, terminal signal
handling, and asynchronous zombie reaping via `SIGCHLD`. Originally built as a
systems programming project at Penn (CIS 5480), extended independently afterward
with performance instrumentation and a move to signal-driven reaping as the shell's
only reaping model.

Contributors: Shaho Solaman, Arkan Kautsar (original project)

## Features

- Interactive core loop: prompt, line reading, parsing, `fork`/`execvp`/`waitpid` execution
- Ctrl-C and Ctrl-D handling, including correct edge cases around interrupted reads
- I/O redirection (`<`, `>`, `>>`)
- Multi-process pipelines of arbitrary length, with all stages forked in parallel
- Process group isolation (`setpgid`) and terminal control (`tcsetpgrp`) for proper
  foreground/background job semantics
- Full terminal signal handling (`SIGINT`, `SIGTSTP`, `SIGQUIT` caught and relayed;
  `SIGTTOU` ignored; `SIGTTIN` left at default so background reads stop correctly)
- Job control: `jobs`, `bg`, `fg` builtins with deferred status notifications
  (`Running:`, `Stopped:`, `Restarting:`, `Finished:`)
- Non-interactive mode (no prompt, default signal dispositions, scripts behave like
  a normal non-interactive shell)
- Asynchronous, signal-driven zombie reaping — no polling. A `SIGCHLD` handler sets
  a flag; all real reaping happens safely outside the handler, guarded with
  `sigprocmask`
- Built-in performance instrumentation (`--profile` / `stats` builtin) measuring
  fork latency, pipe setup cost, and signal-to-reap latency

## Building and running

```bash
make
./penn-conch
./penn-conch --profile   # enables timing instrumentation, queryable via `stats`
```

## Design notes

### Asynchronous reaping
A minimal `SIGCHLD` handler sets a `volatile sig_atomic_t` flag and records an
arrival timestamp — nothing else happens inside the handler, since it must stay
async-signal-safe. All real work — draining exited/stopped children via
`waitpid(-1, WNOHANG | WUNTRACED)` and updating the job queue — happens in
`reap_pending_async`, called from the main loop and from every blocking
foreground wait whenever it's interrupted. `SIGCHLD` is blocked via `sigprocmask`
for the duration of each drain, so the handler can't fire mid-update.

### Performance instrumentation
`--profile` enables nanosecond-precision timing (`clock_gettime(CLOCK_MONOTONIC)`)
around three things: `fork()` latency, `pipe()` setup cost, and the time between a
`SIGCHLD` signal arriving and the corresponding reap completing. Results are
queryable on demand via the `stats` builtin, which prints running averages.

Building this surfaced a real measurement bug worth noting: every child exit
generates `SIGCHLD`, including ones already reaped directly by a successful
foreground `waitpid` call. Without clearing the handler's flag after such a
direct reap, a stale, already-handled signal could later be misattributed to an
unrelated async reap — inflating the latency measurement by however long the
user took to type their next command. The fix clears the flag immediately after
any direct, successful foreground reap, so the latency metric only ever reflects
genuine signal-interruption-to-drain time.

## Known limitations
Since `SIGCHLD` can interrupt an idle read, a background job finishing while the
shell is otherwise idle can trigger an unsolicited prompt redraw to show its
notification — a natural consequence of true asynchronous signal delivery.