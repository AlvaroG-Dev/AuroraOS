// user/lib/env.h
#pragma once
#include <stddef.h>

extern char **environ;

void env_init(char **initial_envp);
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);