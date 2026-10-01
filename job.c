#include "./Job.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { DECIMAL_BASE = 10 };
enum { NS_PER_SEC = 1000000000 };

volatile sig_atomic_t sigchld_flag = 0;  // special flag for sigchld

int profile_mode = 0;
profile_stats g_stats = {0};

// cleanup fn for a job in the vector
static void job_dtor(ptr_t elem) {
  job* the_job = (job*)elem;
  free(the_job->pids);
  free(the_job->text);
  free(the_job);
}

// glue "prefix: text" together by hand since no snprintf
static char* join_note(const char* prefix, const char* text) {
  size_t pre_len = strlen(prefix);
  size_t txt_len = strlen(text);
  char* out = (char*)malloc(pre_len + strlen(": ") + txt_len + 1);
  if (out == NULL) {
    return NULL;
  }
  size_t pos = 0;
  for (size_t i = 0; i < pre_len; i++) {
    out[pos++] = prefix[i];
  }
  out[pos++] = ':';
  out[pos++] = ' ';
  for (size_t i = 0; i < txt_len; i++) {
    out[pos++] = text[i];
  }
  out[pos] = '\0';
  return out;
}

void ctx_init(shell_ctx* ctx) {
  ctx->jobs = vec_new(0, job_dtor);
  ctx->notes = vec_new(0, free);
  ctx->shell_pgid = getpgrp();
  ctx->interactive = isatty(STDIN_FILENO);
  ctx->stop_seq = 0;
}

void ctx_destroy(shell_ctx* ctx) {
  vec_destroy(&ctx->jobs);
  vec_destroy(&ctx->notes);
}

jid_t jobs_next_id(shell_ctx* ctx) {
  jid_t max_id = 0;
  for (size_t i = 0; i < vec_len(&ctx->jobs); i++) {
    job* the_job = (job*)vec_get(&ctx->jobs, i);
    if (the_job->id > max_id) {
      max_id = the_job->id;
    }
  }
  return max_id + 1;
}

job* jobs_find_by_id(shell_ctx* ctx, jid_t id) {
  for (size_t i = 0; i < vec_len(&ctx->jobs); i++) {
    job* the_job = (job*)vec_get(&ctx->jobs, i);
    if (the_job->id == id) {
      return the_job;
    }
  }
  return NULL;
}

job* jobs_find_by_pid(shell_ctx* ctx, pid_t pid) {
  for (size_t i = 0; i < vec_len(&ctx->jobs); i++) {
    job* the_job = (job*)vec_get(&ctx->jobs, i);
    for (size_t j = 0; j < the_job->num_pids; j++) {
      if (the_job->pids[j] == pid) {
        return the_job;
      }
    }
  }
  return NULL;
}

job* jobs_current(shell_ctx* ctx) {
  job* best = NULL;
  // most recently stopped job wins if theres one
  for (size_t i = 0; i < vec_len(&ctx->jobs); i++) {
    job* the_job = (job*)vec_get(&ctx->jobs, i);
    if (the_job->state != JOB_STOPPED) {
      continue;
    }
    if (best == NULL || the_job->stop_seq > best->stop_seq) {
      best = the_job;
    }
  }
  if (best != NULL) {
    return best;
  }
  // else just the newest one (biggest id)
  for (size_t i = 0; i < vec_len(&ctx->jobs); i++) {
    job* the_job = (job*)vec_get(&ctx->jobs, i);
    if (best == NULL || the_job->id > best->id) {
      best = the_job;
    }
  }
  return best;
}

job* jobs_add(shell_ctx* ctx,
              pid_t pgid,
              const pid_t* pids,
              size_t num_pids,
              job_state state,
              char* text) {
  job* the_job = (job*)malloc(sizeof(job));
  if (the_job == NULL) {
    free(text);
    return NULL;
  }
  the_job->pids = (pid_t*)malloc(sizeof(pid_t) * num_pids);
  if (the_job->pids == NULL) {
    free(the_job);
    free(text);
    return NULL;
  }
  for (size_t i = 0; i < num_pids; i++) {
    the_job->pids[i] = pids[i];
  }
  the_job->num_pids = num_pids;
  the_job->num_alive = num_pids;
  the_job->pgid = pgid;
  the_job->state = state;
  the_job->text = text;
  the_job->id = jobs_next_id(ctx);
  the_job->stop_seq = 0;
  if (state == JOB_STOPPED) {
    the_job->stop_seq = ++ctx->stop_seq;
  }
  vec_push_back(&ctx->jobs, the_job);
  return the_job;
}

void jobs_remove(shell_ctx* ctx, job* target) {
  for (size_t i = 0; i < vec_len(&ctx->jobs); i++) {
    if ((job*)vec_get(&ctx->jobs, i) == target) {
      vec_erase(&ctx->jobs, i);
      return;
    }
  }
}

void note_add(shell_ctx* ctx, const char* prefix, const char* text) {
  char* line = join_note(prefix, text);
  if (line != NULL) {
    vec_push_back(&ctx->notes, line);
  }
}

void notes_flush(shell_ctx* ctx) {
  for (size_t i = 0; i < vec_len(&ctx->notes); i++) {
    fprintf(stderr, "%s\n", (char*)vec_get(&ctx->notes, i));
  }
  vec_clear(&ctx->notes);
}

void reap_status(shell_ctx* ctx, pid_t pid, int status) {
  job* the_job = jobs_find_by_pid(ctx, pid);
  if (the_job == NULL) {
    return;
  }
  if (WIFSTOPPED(status)) {
    the_job->state = JOB_STOPPED;
    the_job->stop_seq = ++ctx->stop_seq;
    note_add(ctx, "Stopped", the_job->text);
  } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
    if (the_job->num_alive > 0) {
      the_job->num_alive--;
    }
    if (the_job->num_alive == 0) {
      note_add(ctx, "Finished", the_job->text);
      jobs_remove(ctx, the_job);
    }
  }
}

// builtins: jobs, bg, fg
static job* pick_job(shell_ctx* ctx, char* const* args) {
  if (args[1] == NULL) {
    return jobs_current(ctx);
  }
  char* end = NULL;
  long the_id = strtol(args[1], &end, DECIMAL_BASE);
  if (*end != '\0' || the_id <= 0) {
    return NULL;
  }
  return jobs_find_by_id(ctx, (jid_t)the_id);
}

static void builtin_jobs(shell_ctx* ctx) {
  for (size_t i = 0; i < vec_len(&ctx->jobs); i++) {
    job* the_job = (job*)vec_get(&ctx->jobs, i);
    const char* label = the_job->state == JOB_STOPPED ? "stopped" : "running";
    fprintf(stderr, "[%lu] %s (%s)\n", (unsigned long)the_job->id,
            the_job->text, label);
  }
}

static void builtin_bg(shell_ctx* ctx, char* const* args) {
  job* the_job = pick_job(ctx, args);
  if (the_job == NULL || the_job->state == JOB_RUNNING) {
    fprintf(stderr, "bg: invalid job\n");
    return;
  }
  the_job->state = JOB_RUNNING;
  killpg(the_job->pgid, SIGCONT);
  fprintf(stderr, "Running: %s\n", the_job->text);
}

// wait on a job we just brought to the foreground
static void fg_wait(shell_ctx* ctx, job* the_job) {
  int stopped = 0;
  while (the_job->num_alive > 0) {
    int status = 0;
    pid_t done = waitpid(-the_job->pgid, &status, WUNTRACED);
    if (done == -1) {
      if (errno == EINTR) {// if waitpid interrupted drain any unrelated bg
                           // reaps first
        reap_pending_async(ctx);
        continue;
      }
      break;
    }
    sigchld_flag = 0;
    if (WIFSTOPPED(status)) {
      stopped = 1;
      break;
    }
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
      the_job->num_alive--;
    }
  }
  if (ctx->interactive) {
    tcsetpgrp(STDIN_FILENO, ctx->shell_pgid);
  }
  if (stopped) {
    the_job->state = JOB_STOPPED;
    the_job->stop_seq = ++ctx->stop_seq;
    fprintf(stderr, "Stopped: %s\n", the_job->text);
  } else if (the_job->num_alive == 0) {
    jobs_remove(ctx, the_job);
  }
}

static void builtin_fg(shell_ctx* ctx, char* const* args) {
  job* the_job = pick_job(ctx, args);
  if (the_job == NULL) {
    fprintf(stderr, "fg: invalid job\n");
    return;
  }
  if (ctx->interactive) {
    tcsetpgrp(STDIN_FILENO, the_job->pgid);
  }
  if (the_job->state == JOB_STOPPED) {
    fprintf(stderr, "Restarting: %s\n", the_job->text);
    the_job->state = JOB_RUNNING;
    killpg(the_job->pgid, SIGCONT);
  } else {
    fprintf(stderr, "%s\n", the_job->text);
  }
  fg_wait(ctx, the_job);
}

bool run_builtin(shell_ctx* ctx, const struct parsed_command* cmd) {
  char* name = cmd->commands[0][0];
  if (strcmp(name, "jobs") == 0) {
    builtin_jobs(ctx);
    return true;
  }
  if (strcmp(name, "bg") == 0) {
    builtin_bg(ctx, cmd->commands[0]);
    return true;
  }
  if (strcmp(name, "fg") == 0) {
    builtin_fg(ctx, cmd->commands[0]);
    return true;
  }
  if (strcmp(name, "stats") == 0) {
    print_profile_stats();
    return true;
  }
  return false;
}
// safely drains all pending child state changes when SIGCHLD has fired
void reap_pending_async(shell_ctx* ctx) {
  sigset_t block_mask;
  sigset_t orig_mask;
  sigemptyset(&block_mask);
  sigaddset(&block_mask, SIGCHLD);
  sigprocmask(SIG_BLOCK, &block_mask, &orig_mask);

  if (sigchld_flag) {
    double arrival = sigchld_arrival_ns;
    sigchld_flag = 0;
    int status = 0;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
      reap_status(ctx, pid, status);
    }
    if (profile_mode) {
      g_stats.signal_latency_ns_total += now_ns() - arrival;
      g_stats.signal_latency_count++;
    }
  }

  sigprocmask(SIG_SETMASK, &orig_mask, NULL);
}

double now_ns(void) {
  struct timespec time_spec;
  clock_gettime(CLOCK_MONOTONIC, &time_spec);
  return ((double)time_spec.tv_sec * NS_PER_SEC) + (double)time_spec.tv_nsec;
}

void print_profile_stats(void) {
  fprintf(stderr, "--- penn-conch profile ---\n");
  if (g_stats.fork_exec_count > 0) {
    fprintf(stderr, "fork+exec avg: %.0f ns (n=%ld)\n",
      g_stats.fork_exec_ns_total / (double)g_stats.fork_exec_count,
      g_stats.fork_exec_count);
  }
  if (g_stats.pipe_setup_count > 0) {
    fprintf(stderr, "pipe setup avg: %.0f ns (n=%ld)\n",
      g_stats.pipe_setup_ns_total / (double)g_stats.pipe_setup_count,
      g_stats.pipe_setup_count);
  }
  if (g_stats.signal_latency_count > 0) {
    fprintf(stderr, "SIGCHLD->reap latency avg: %.0f ns (n=%ld)\n",
      g_stats.signal_latency_ns_total / (double)g_stats.signal_latency_count,
      g_stats.signal_latency_count);
  }
}
