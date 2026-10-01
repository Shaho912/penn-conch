#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Job.h"
#include "parser.h"
#include "redirect.h"

#include <sys/types.h>  // Provides the pid_t data type // pid_t and execvp lib
#include <sys/wait.h>   // waitpid
#include <unistd.h>

#include <errno.h>
#include <signal.h>

#ifndef PROMPT
#define PROMPT "penn-shell> "
#endif

volatile sig_atomic_t got_sigint = 0;  // flag type for signal handlers

volatile double sigchld_arrival_ns = 0;
void sigchld_handler(int sig) {
    sigchld_flag = 1;
    sigchld_arrival_ns = now_ns();  // clock_gettime is async-signal-safe per POSIX
}

void sigint_handler(int sig) {
  got_sigint = 1;
}

#define STDIN_FILENO 0   // stdin fileno
#define STDOUT_FILENO 1  // stdout fileno
#define STDERR_FILENO 2  // stderr fileno

// shared job control state
static shell_ctx g_ctx;

static char* build_string(struct parsed_command* cmd) {
  size_t tot_len = 0;
  for (size_t i = 0; i < cmd->num_commands;
       i++) {  // count how many chars in one job
    for (char** cmd_ind = cmd->commands[i]; *cmd_ind != NULL; cmd_ind++) {
      tot_len += strlen(*cmd_ind) + 1;
    }
    if (i < cmd->num_commands - 1) {
      tot_len += 2;  // 2 more spaces allocated for pipe symbol
    }
  }
  char* str_ptr = (char*)malloc((sizeof(char) * tot_len) +
                                1);  // allocate enough space for job's string
  if (str_ptr == NULL) {
    return NULL;
  }

  size_t pos = 0;
  for (size_t i = 0; i < cmd->num_commands;
       i++) {  // copy job's commands into a single string
    for (char** cmd_ind = cmd->commands[i]; *cmd_ind != NULL; cmd_ind++) {
      size_t cmd_len = strlen(*cmd_ind);
      for (size_t j = 0; j < cmd_len; j++) {
        str_ptr[pos++] = (*cmd_ind)[j];
      }
      str_ptr[pos++] = ' ';
    }
    if (i < cmd->num_commands - 1) {
      str_ptr[pos++] = '|';
      str_ptr[pos++] = ' ';
    }
  }

  if (pos > 0 && str_ptr[pos - 1] == ' ') {
    pos--;  // trim trailing space
  }
  str_ptr[pos] = '\0';

  return str_ptr;
}
// stash a launched job in the queue, only when interactive
static void record_job(struct parsed_command* cmd,
                       pid_t pgid,
                       const pid_t* pids,
                       size_t forked,
                       job_state state) {
  if (!g_ctx.interactive) {
    return;
  }
  char* job_str = build_string(cmd);
  if (job_str != NULL) {
    jobs_add(&g_ctx, pgid, pids, forked, state, job_str);
  }
}

static int timed_pipe(int pfd[2]) {
  double start = profile_mode ? now_ns() : 0;
  int result = pipe(pfd);
  if (profile_mode) {
    g_stats.pipe_setup_ns_total += now_ns() - start;
    g_stats.pipe_setup_count++;
  }
  return result;
}

static pid_t timed_fork(void) {
  double start = profile_mode ? now_ns() : 0;
  pid_t pid = fork();
  if (profile_mode && pid > 0) {
    g_stats.fork_exec_ns_total += now_ns() - start;
    g_stats.fork_exec_count++;
  }
  return pid;
}

static void run_pipeline(struct parsed_command* cmd,
                         pid_t shell_pgid,
                         int interactive) {
  size_t num_cmds = cmd->num_commands;
  pid_t pids[num_cmds];
  size_t forked = 0;
  int in_fd = STDIN_FILENO;
  pid_t pgid = 0;

  int real_bg = cmd->is_background && interactive;
  for (size_t i = 0; i < num_cmds; i++) {
    int pfd[2];
    if (i < num_cmds - 1 && timed_pipe(pfd) == -1) {
      perror("pipe");
      break;
    }
    if (i < num_cmds - 1 && pipe(pfd) == -1) {
      perror("pipe");
      break;
    }
    pid_t pid = timed_fork();
    if (pid == 0) {  // run
      signal(SIGINT, SIG_DFL);
      signal(SIGTTOU, SIG_DFL);
      signal(SIGTTIN, SIG_DFL);
      signal(SIGTSTP, SIG_DFL);

      setpgid(0,
              pgid);  // make child leader of a new pgrp if its the first proc
      // read from the previous stage
      if (in_fd != STDIN_FILENO) {
        dup2(in_fd, STDIN_FILENO);
        close(in_fd);
      }

      // write to the next stage
      if (i < num_cmds - 1) {
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
      }

      setup_redirections(cmd, i);
      execvp(cmd->commands[i][0], cmd->commands[i]);
      perror("execvp");
      _exit(EXIT_FAILURE);
    } else if (pid > 0) {
      if (pgid == 0) {
        pgid = pid;  // first child's pid becomes pgid
        if (!real_bg && interactive) {
          tcsetpgrp(STDIN_FILENO,
                    pgid);  // give terminal control to the new job's group
        }
      }
      setpgid(pid, pgid);  // put child into its own group again to avoid race

      pids[forked++] = pid;

      // close ends the parent no longer needs
      if (in_fd != STDIN_FILENO) {
        close(in_fd);
      }

      if (i < num_cmds - 1) {
        close(pfd[1]);
        in_fd = pfd[0];
      }
    } else {
      perror("fork");

      if (i < num_cmds - 1) {
        close(pfd[0]);
        close(pfd[1]);
      }

      break;
    }
  }

  int status = 0;
  int signaled = 0;
  int any_stopped = 0;

  for (size_t i = 0; i < forked; i++) {
    if (!real_bg) {
      while (waitpid(pids[i], &status, WUNTRACED) == -1 && errno == EINTR) {
        // drain any unrelated bg reaps
                           // before waiting on pid
        reap_pending_async(&g_ctx);
      }
      sigchld_flag = 0;
      if (WIFSIGNALED(status)) {
        signaled = 1;
      }
      if (WIFSTOPPED(status)) {
        any_stopped = 1;
      }
    }
  }
  if (!real_bg) {
    if (any_stopped) {
      fprintf(stderr, "\n");
      char* stop_str = build_string(cmd);
      if (stop_str != NULL) {
        fprintf(stderr, "Stopped: %s\n", stop_str);
        free(stop_str);
      }
      record_job(cmd, pgid, pids, forked, JOB_STOPPED);
    }
    if (interactive) {
      tcsetpgrp(STDIN_FILENO, shell_pgid);
    }

  } else {
    char* run_str = build_string(cmd);
    if (run_str != NULL) {
      fprintf(stderr, "Running: %s\n", run_str);
      free(run_str);
    }
    record_job(cmd, pgid, pids, forked, JOB_RUNNING);
  }

  if (signaled) {
    fprintf(stderr, "\n");
  }
}

int main(int argc, char* argv[]) {
  char* line_ptr = NULL;
  size_t len = 0;
  ssize_t num_read;
  struct parsed_command* cmd = NULL;
  int err;
  const char* prompt = PROMPT;

  pid_t shell_pgid = getpgrp();

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--profile") == 0) {
      profile_mode = 1;
    }
  }
  int interactive = isatty(STDIN_FILENO);
  if (interactive) {  // if !interactive, follow default disposition
    struct sigaction sigint_action = {0};
    sigint_action.sa_handler = sigint_handler;
    sigemptyset(&sigint_action.sa_mask);
    sigaction(SIGINT, &sigint_action,
              NULL);  // handle these signals only for shell
    sigaction(SIGTSTP, &sigint_action, NULL);
    sigaction(SIGQUIT, &sigint_action, NULL);
    signal(
        SIGTTOU,
        SIG_IGN);  // completely ignore sigttou so shell can reclaim term ctrl
  }

  // install sigchld handler
  struct sigaction sigchld_action = {0};
  sigchld_action.sa_handler = sigchld_handler;
  sigemptyset(&sigchld_action.sa_mask);
  sigaction(SIGCHLD, &sigchld_action, NULL);
  ctx_init(&g_ctx);

  while (1) {
    notes_flush(&g_ctx);  // dump held notifs before prompting
    if (interactive && write(STDERR_FILENO, prompt, strlen(prompt)) == -1) {
      perror("write");
      break;
    }
    num_read = getline(&line_ptr, &len, stdin);
    int getline_errno = errno;

  
      // reaping is driven by SIGCHLD instead of polled every
      // loop iteration
    reap_pending_async(&g_ctx);
    

    if (num_read == -1) {
      free(line_ptr);
      line_ptr = NULL;
      len = 0;
      if (feof(stdin)) {
        break;
      }
      if (getline_errno == EINTR) {  // if Ctrl+C is entered, re-prompt
        clearerr(stdin);
        fprintf(stderr, "\n");
        continue;
      }
      errno = getline_errno;
      perror("getline");
      break;
    }
    if (num_read == 0 || line_ptr[num_read - 1] != '\n') {
      fprintf(stderr, "\n");
      clearerr(stdin);  // flush std in crtl+d was pressed
      // Ctrl-D flushes a partial line so newline needs to be added
      fprintf(stderr, "\n");
    }

    err = parse_command(line_ptr, &cmd);
    if (err != 0) {
      fprintf(stderr, "invalid: ");
      print_parser_errcode(stderr, err);
      free(line_ptr);
      line_ptr = NULL;
      len = 0;
      continue;
    }
    if (cmd->num_commands == 0) {
      free(cmd);  // do nothing on an empty command line, reprompt
      free(line_ptr);
      line_ptr = NULL;
      len = 0;
      continue;
    }

    if (!run_builtin(&g_ctx, cmd)) {
      run_pipeline(cmd, shell_pgid, interactive);
    }

    free(cmd);
    free(line_ptr);
    len = 0;
    line_ptr =
        NULL;  // getline requires this to be NULL or a valid malloc'd ptr
  }
  ctx_destroy(&g_ctx);
  return 0;
}