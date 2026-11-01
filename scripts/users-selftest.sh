#!/bin/bash
# Se ejecuta DENTRO de Aurora (como root) desde scripts/users-test.sh.
ok=0; bad=0
t() { # nombre, esperado(regex), comando...
  local name=$1 want=$2; shift 2
  local out; out=$("$@" 2>&1 </dev/null); local rc=$?
  if echo "$out" | grep -Eq "$want"; then ok=$((ok+1)); echo "[users] PASS $name"
  else bad=$((bad+1)); echo "[users] FAIL $name :: rc=$rc out=$(echo "$out" | head -3 | tr '\n' '|')"; fi
}
sh_() { bash -c "$1" 2>&1; }
t useradd 'creado testu' aurora-useradd -m -G wheel testu
t id_testu 'testu' id testu
t home_creado 'testu' bash -c 'ls -ld /data/home/testu /home/testu 2>/dev/null | head -1'
t passwd_root 'actualizada' bash -c 'printf "secret\nsecret\n" | passwd testu'
t passwd_alvaro 'actualizada' bash -c 'printf "pw\npw\n" | passwd alvaro'
t shadow_0600 'rw-------' bash -c 'ls -l /etc/shadow'
t su_root_a_testu 'uid=[0-9]+\(testu\)' su testu -c id
t su_sin_clave_falla 'incorrecta|fallida|Login' bash -c 'echo mala | su testu -c "echo X; su alvaro -c id"; true'
t su_con_clave 'uid=1000' bash -c 'su testu -c "echo pw | su alvaro -c id"'
t passwd_propio 'actualizada' bash -c 'su testu -c "printf \"secret\nnew1\nnew1\n\" | passwd"'
t passwd_actual_mala 'incorrecta' bash -c 'su testu -c "printf \"MALA\nx\nx\n\" | passwd" ; true'
t passwd_ajeno_no 'solo root' bash -c 'su testu -c "printf \"x\nx\n\" | passwd alvaro"; true'
t sudo_wheel 'uid=0' bash -c 'su testu -c "sudo id"'
t sudo_no_wheel 'no pertenece' bash -c 'useradd plain >/dev/null 2>&1; su plain -c "sudo id"; true'
t login_force 'testu' bash -c 'echo id | aurora-login -f testu'
t no_lee_shadow 'denegado|Permission|denied|No such' bash -c 'su testu -c "cat /etc/shadow"; true'
t chown_cmd '1000 1000' bash -c 'mkdir -p /data/pr && touch /data/pr/f && chown 1000:1000 /data/pr/f && ls -ln /data/pr/f | awk "{print \$3, \$4}"'
t sync_cmd 'ok' bash -c 'sync && echo ok'
t run_user_dir '1000 1000 drwx------|alvaro' bash -c 'ls -ldn /run/user/1000 | awk "{print \$3, \$4, \$1}"; ls -ld /run/user/1000'
t xdg_su 'run/user/1000' bash -c 'su alvaro -c "echo \$XDG_RUNTIME_DIR"'
t run0_atravesable 'drwx--x--x' bash -c 'ls -ld /run/user/0'
t input_grupo '104' bash -c 'ls -ln /dev/input/event0'
t grupo_video 'video' bash -c 'su alvaro -c "id -Gn"'
t touch_ajeno 'denied|Permission|permitido|not permitted' bash -c 'su alvaro -c "touch /etc/hosts" 2>&1; true'
t link_tmp 'ok' bash -c 'su alvaro -c "echo x > /tmp/lk1 && ln /tmp/lk1 /tmp/lk2 && cat /tmp/lk2 && echo ok" 2>&1'
echo "[users] RESULT pass=$ok fail=$bad"
