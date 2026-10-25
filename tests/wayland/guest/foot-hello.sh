#!/bin/bash
# foot con un comando fijo: comprueba pty -> render sin depender del shell.
exec /usr/bin/foot -- /bin/sh -c 'echo HELLO-FOOT; echo line2; sleep 120'
