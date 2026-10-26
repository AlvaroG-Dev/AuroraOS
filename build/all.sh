cd /home/alvaro/aurora-os-copy
git add -A
git status --short | head -40
git commit -q -F - <<'MSG'
Binarios regenerados, artefactos de build y ajustes locales pendientes

Recompilacion tras los cambios de entorno/terminal/xterm: binarios de
user/, user/musl/ y precompiled/, ficheros de build/ y los cambios locales
pendientes (kernel/gfx/compositor.c, build_busybox.sh, vdso, libs).

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
MSG
git log --oneline -3
git push 2>&1 | tail -6
git status -sb | head -1
