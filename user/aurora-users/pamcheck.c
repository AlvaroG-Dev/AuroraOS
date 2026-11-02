// user/aurora-users/pamcheck.c
//
// [U8.2] Comprueba una clave contra la pila PAM del servicio dado (por defecto
// "other"): pamcheck [-s servicio] <usuario> <clave>. Sale con 0 si autentica.
// Sirve de prueba de PAM y de ejemplo para quien porte un greeter/polkit.
// Se declara a mano la API de libpam (no hacen falta las cabeceras).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct pam_handle pam_handle_t;
struct pam_message { int msg_style; const char *msg; };
struct pam_response { char *resp; int resp_retcode; };
struct pam_conv {
  int (*conv)(int, const struct pam_message **, struct pam_response **, void *);
  void *appdata_ptr;
};
int pam_start(const char *, const char *, const struct pam_conv *, pam_handle_t **);
int pam_authenticate(pam_handle_t *, int);
int pam_acct_mgmt(pam_handle_t *, int);
int pam_end(pam_handle_t *, int);
const char *pam_strerror(pam_handle_t *, int);

#define PAM_PROMPT_ECHO_OFF 1
#define PAM_PROMPT_ECHO_ON 2

static int conv(int n, const struct pam_message **msg, struct pam_response **resp, void *data) {
  struct pam_response *r = calloc((size_t)n, sizeof(*r));
  if (!r)
    return 19; // PAM_BUF_ERR
  for (int i = 0; i < n; i++)
    if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
      r[i].resp = strdup((const char *)data);
  *resp = r;
  return 0;
}

int main(int argc, char **argv) {
  const char *svc = "other";
  int i = 1;
  if (argc > 2 && !strcmp(argv[1], "-s")) {
    svc = argv[2];
    i = 3;
  }
  if (argc - i != 2) {
    fprintf(stderr, "uso: pamcheck [-s servicio] usuario clave\n");
    return 2;
  }
  struct pam_conv c = {conv, argv[i + 1]};
  pam_handle_t *h = NULL;
  int rc = pam_start(svc, argv[i], &c, &h);
  if (rc != 0) {
    fprintf(stderr, "pam_start: %d\n", rc);
    return 3;
  }
  rc = pam_authenticate(h, 0);
  if (rc == 0)
    rc = pam_acct_mgmt(h, 0);
  printf("pamcheck: %s (%s)\n", rc == 0 ? "OK" : "FALLO", pam_strerror(h, rc));
  pam_end(h, rc);
  return rc == 0 ? 0 : 1;
}
