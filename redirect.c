// redirect.c
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "redirect.h"

static const mode_t FILE_MODE = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

static void redirect_stdin(const char* path) {
  // open file and save to variable
  int file = open(path, O_RDONLY);

  // exit if open file fails
  if (file == -1) {
    perror(path);
    _exit(EXIT_FAILURE);
  }

  int res = dup2(file, STDIN_FILENO);
  if (res == -1) {
    perror("dup2");
    _exit(EXIT_FAILURE);
  }

  close(file);
}

static void redirect_stdout(const char* path, bool append) {
  int file =
      open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), FILE_MODE);
  if (file == -1) {
    perror(path);
    _exit(EXIT_FAILURE);
  }

  // same logic as redirect_stdin
  int res = dup2(file, STDOUT_FILENO);
  if (res == -1) {
    perror("dup2");
    _exit(EXIT_FAILURE);
  }

  close(file);
}

void setup_redirections(const struct parsed_command* cmd, size_t stage) {
  // only applies to the first command in a pipeline
  if (stage == 0 && cmd->stdin_file != NULL) {
    redirect_stdin(cmd->stdin_file);
  }

  // only apply < or << to the last command in a pipeline
  if (cmd->stdout_file != NULL && stage == cmd->num_commands - 1) {
    redirect_stdout(cmd->stdout_file, cmd->is_file_append);
  }
}