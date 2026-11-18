#!/usr/bin/env python3
"""Impede flash direto acidental, que sobrescreveria o MultiLoader."""
import sys
print("\nERRO: Upload DESATIVADO para proteger o MultiLoader da TTGO VGA32.")
print("Use PlatformIO Build (Ctrl+Alt+B) e obtenha .pio/build/ttgo-vga32/firmware.bin.")
print("Carregue esse binario SOMENTE pelo seu fluxo MultiLoader/OTA ja existente.")
sys.exit(2)
