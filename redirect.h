// redirect.h
#pragma once
#include <stddef.h>
#include "parser.h"

void setup_redirections(const struct parsed_command* cmd, size_t stage);