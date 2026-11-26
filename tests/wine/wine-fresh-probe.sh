#!/bin/sh
# Primera app de Wine sin prefijo: lo crea, aplica el tema y abre la ventana.
WINE_FRESH=1 WAIT_APP=${WAIT_APP:-900} exec sh /usr/bin/wine-dock-probe.sh
