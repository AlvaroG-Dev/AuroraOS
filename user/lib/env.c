// user/lib/env.c
#include "env.h"
#include "malloc.h"
#include "string.h"

#define ENV_MAX 64

// Almacenamiento propio. environ apunta aquí, no al stack. Los
// punteros iniciales pueden apuntar a strings del stack (los del
// crt0), y como no los liberamos, es seguro.
static char *env_ptrs[ENV_MAX];
char **environ = env_ptrs;

void env_init(char **initial_envp) {
  int i = 0;
  if (initial_envp) {
    while (initial_envp[i] && i < ENV_MAX - 1) {
      env_ptrs[i] = initial_envp[i];
      i++;
    }
  }
  env_ptrs[i] = NULL;
}

char *getenv(const char *name) {
  if (!name || !name[0])
    return NULL;
  size_t n = strlen(name);
  for (int i = 0; environ[i]; i++) {
    if (strncmp(environ[i], name, n) == 0 && environ[i][n] == '=')
      return environ[i] + n + 1;
  }
  return NULL;
}

int setenv(const char *name, const char *value, int overwrite) {
  if (!name || !name[0] || !value)
    return -1;
  size_t nlen = strlen(name);
  for (size_t i = 0; i < nlen; i++)
    if (name[i] == '=')
      return -1;

  for (int i = 0; environ[i]; i++) {
    if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=') {
      if (!overwrite)
        return 0;
      size_t vlen = strlen(value);
      char *nv = (char *)malloc(nlen + 1 + vlen + 1);
      if (!nv)
        return -1;
      memcpy(nv, name, nlen);
      nv[nlen] = '=';
      memcpy(nv + nlen + 1, value, vlen + 1);
      environ[i] = nv; // leak bounded: no liberamos el viejo
      return 0;
    }
  }

  int count = 0;
  while (environ[count])
    count++;
  if (count >= ENV_MAX - 1)
    return -1;

  size_t vlen = strlen(value);
  char *nv = (char *)malloc(nlen + 1 + vlen + 1);
  if (!nv)
    return -1;
  memcpy(nv, name, nlen);
  nv[nlen] = '=';
  memcpy(nv + nlen + 1, value, vlen + 1);
  environ[count] = nv;
  environ[count + 1] = NULL;
  return 0;
}

int unsetenv(const char *name) {
  if (!name || !name[0])
    return -1;
  size_t nlen = strlen(name);
  for (int i = 0; environ[i]; i++) {
    if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=') {
      for (int j = i; environ[j]; j++)
        environ[j] = environ[j + 1];
      return 0;
    }
  }
  return 0;
}