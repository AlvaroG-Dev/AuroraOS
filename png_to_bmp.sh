#!/bin/bash

echo "=== PNG → BMP 32-bit RGBA ==="
echo

read -p "Ruta del archivo PNG de entrada: " input
read -p "Ruta del archivo BMP de salida: " output

if [ ! -f "$input" ]; then
    echo "❌ Error: no existe el archivo '$input'"
    exit 1
fi

convert "$input" -alpha on -depth 8 -define bmp:format=bmp4 "$output"

if [ $? -eq 0 ]; then
    echo
    echo "✅ Conversión completada:"
    echo "   Entrada: $input"
    echo "   Salida:  $output"
else
    echo
    echo "❌ Error durante la conversión."
    exit 1
fi