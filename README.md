# ATLAS-DEL-AIRE

Firmware Arduino/ESP8266 para el panel LED de calidad del aire y utilidad de flasheo para usuarios finales.

## Estructura

- `firmware/panel_stable/panel_stable.ino`: version estable actual del sketch.
- `firmware/releases/atlas-del-aire-latest.bin`: binario listo para distribuir.
- `tools/native_flasher/atlas_flasher.py`: flasher nativo minimo para Windows.

## Flasher

La carpeta de distribucion usada en las pruebas contiene:

- `ATLAS-del-Aire-Flasher.exe`
- `atlas-del-aire-latest.bin`

El flasher:

- detecta puertos serie reales del sistema
- usa un firmware fijo llamado `atlas-del-aire-latest.bin`
- solo deja elegir puerto y actualizar

## Notas

- Los IDs de sensores arrancan vacios por defecto en la configuracion inicial.
- Una vez guardados en WiFiManager, se cargan desde EEPROM en reinicios posteriores.
- El segundo sensor es opcional y no se consulta si no esta configurado.
