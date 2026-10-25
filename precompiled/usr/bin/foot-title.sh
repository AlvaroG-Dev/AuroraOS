#!/bin/bash
# foot que cambia su titulo (OSC 2) tras arrancar: comprueba xdg_toplevel.set_title.
exec /usr/bin/foot -- /bin/sh -c 'sleep 4; printf "\033]2;Titulo cambiado\007"; sleep 120'
