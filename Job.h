#ifndef JOB_H_
#define JOB_H_

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "./Vec.h"
#include "./parser.h"

// define new type "job id"
typedef uint64_t jid_t;

// a job is either running or stopped in the queue
typedef enum { JOB_RUNNING, JOB_STOPPED } job_state;

// Represents a job
typedef struct job_st {
  jid_t id;
  pid_t pgid;
  pid_t* pids;  // owned array, size num_pids
  size_t num_pids;
  size_t num_alive;  // procs not reaped yet
  job_state state;
  uint64_t stop_seq;  // bumped when the job stops, for recency
  char* text;         // owned pipeline text, no trailing '&'
} job;

// all the shared shell state in one place
typedef struct shell_ctx_st {
  Vec jobs;   // vec of job*
  Vec notes;  // vec of char*, held notifications
  pid_t shell_pgid;
  bool interactive;
  uint64_t stop_seq;
} shell_ctx;

#include <time.h>

extern int profile_mode;

typedef struct {
    double fork_exec_ns_total;
    long fork_exec_count;
    double pipe_setup_ns_total;
    long pipe_setup_count;
    double signal_latency_ns_total;
    long signal_latency_count;
} profile_stats;

extern profile_stats g_stats;
extern volatile sig_atomic_t sigchld_flag;  // EC flags externed
extern volatile double sigchld_arrival_ns;

void ctx_init(shell_ctx* ctx);
void ctx_destroy(shell_ctx* ctx);

// job queue helpers
jid_t jobs_next_id(shell_ctx* ctx);
job* jobs_find_by_id(shell_ctx* ctx, jid_t id);
job* jobs_find_by_pid(shell_ctx* ctx, pid_t pid);
job* jobs_current(shell_ctx* ctx);
job* jobs_add(shell_ctx* ctx,
              pid_t pgid,
              const pid_t* pids,
              size_t num_pids,
              job_state state,
              char* text);
void jobs_remove(shell_ctx* ctx, job* target);

// held notifs, dumped right before the prompt
void note_add(shell_ctx* ctx, const char* prefix, const char* text);
void notes_flush(shell_ctx* ctx);

// apply one polled status change to its job
void reap_status(shell_ctx* ctx, pid_t pid, int status);

// run jobs/bg/fg, true if it was a builtin
bool run_builtin(shell_ctx* ctx, const struct parsed_command* cmd);

void reap_pending_async(shell_ctx* ctx);

// instrumentation
double now_ns(void);

void print_profile_stats(void);

#endif  // JOB_H_
