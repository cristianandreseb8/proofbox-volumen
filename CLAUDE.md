# ProofBox — módulo Volumen

Mide cuánto sube una masa (ToF), su temperatura (DS18B20) y su conductividad
(dos electrodos), y lo publica por MQTT. La app web lo lee en vivo y registra
el histórico en Supabase.

Dos piezas y nada más: `ProofBox_Volumen.ino` (firmware) e `index.html` (app).
La app se despliega sola en GitHub Pages al pushear a `main`.

---

## Hardware

**Placa: ESP32 clásica (DevKit / WROOM-32).** No es la S3 — eso fue un desvío
que se revirtió. Los GPIO 34/35/36/39 existen aquí y son de solo entrada.

| Señal | GPIO |
|---|---|
| VL53L0X SDA / SCL | 21 / 22 |
| DS18B20 datos | 27 |
| Electrodo 1 (excitación A) | 25 |
| Electrodo 2 (excitación B, vía R) | 26 |
| Conductividad (medida, ADC1) | 34 |
| LED de placa | 2 |

ToF y DS18B20 se alimentan de **3V3** (nunca 5V) con GND común. Los electrodos
no llevan alimentación: los propios GPIO dan la corriente alternando polaridad.

### Dos resistencias de 4k7, y no son intercambiables de sitio

- **En serie**, entre GPIO 26 y el punto de medida de los electrodos.
- **En paralelo**, del cable de datos del DS18B20 (GPIO 27) a 3V3.

Poner la del DS18B20 en serie con el cable rojo ahoga la alimentación del
sensor y deja el bus sin pull-up. Da `DS18B20 no encontrado` y cuesta horas de
encontrar. Si pasa: un sketch que haga `OneWire.reset()` por pin lo diagnostica
en segundos — `reposo=LOW` significa que falta el pull-up.

---

## Compilar y grabar

```bash
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
"$CLI" --config-file ~/.arduinoIDE/arduino-cli.yaml compile -b esp32:esp32:esp32 <sketchdir>
"$CLI" --config-file ~/.arduinoIDE/arduino-cli.yaml upload -b esp32:esp32:esp32:UploadSpeed=115200 -p /dev/cu.usbserial-0001 <sketchdir>
```

**Grabar SIEMPRE a 115200.** A 921600 falla con `No more data to read from the
serial port`: el circuito de auto-reset de esta placa es flojo. Ese mismo
defecto hace que los reset por software de esptool la dejen colgada — si se
queda muda, el reset fiable es regrabar, o que el usuario desenchufe el USB.

El nombre de la carpeta debe coincidir con el del `.ino`, así que se compila
copiándolo a un directorio temporal con el nombre correcto.

### Leer el puerto serie sin volverse loco

En macOS, `stty` y luego `cat` **pierde el baudio** al cerrarse y reabrirse el
puerto: se lee un flujo de 115200 a 9600 y sale basura que parece una placa
rota. Hay que mantener el descriptor abierto:

```bash
exec 3<>/dev/cu.usbserial-0001
stty -f /dev/cu.usbserial-0001 115200 raw -echo
cat <&3 > salida.log
```

Esto ya provocó una sesión entera diagnosticando un fallo inexistente.

---

## Decisiones de diseño, y por qué

**La conductividad es relativa, no absoluta.** Se publica en µS y kΩ, pero lo
que vale es el % contra una base que marca el usuario. No conocemos la
constante de celda (separación y superficie mojada de los electrodos), así que
el valor absoluto no es comparable con nada. Mover los electrodos cambia el
número sin que cambie la masa.

**`Rx = R · v2/v1`.** Al medir en las dos polaridades y dividir, la tensión de
alimentación se cancela: no hace falta conocer Vdd ni calibrar el ADC, solo que
sea lineal. La alternancia de polaridad evita electrólisis y que los electrodos
se coman. En reposo los pines quedan en alta impedancia.

**Temperatura compensada a 25 °C.** La conductividad se mueve ~2%/°C; sin
corregir, unos grados de deriva se confunden con un cambio real en la masa. La
base guardada ya está compensada.

**La sesión sobrevive a cortes de luz.** Todo en NVS. Volver a cero es siempre
explícito: Nueva masa, metaReset o calibrate. `setup()` solo auto-calibra si no
había sesión guardada — al revés perdía el avance en cada reinicio.

**La reconexión MQTT no bloquea.** Era un `while` con `delay(5000)` que paraba
el loop entero y producía "sin señal" con el aparato perfectamente vivo.

**Los instrumentos y el registro siguen a la etapa abierta.** Si la etapa en
curso es anotada a mano —o la hoja no es la que tiene el aparato—, las tarjetas
de volumen, temperatura y conductividad se ocultan y no se guarda ni una fila.
Lo que el ToF ve durante un amasado es la mesa, no la masa: enseñarlo invita a
leerlo como dato de esa etapa, y guardarlo ensucia el histórico de la hoja. En
su sitio queda un aviso que dice por qué. El gráfico pequeño, en esa etapa,
dibuja lo que contaste, no lo que se midió.

**La app suaviza solo al dibujar.** Lo que va a Supabase y al CSV es siempre el
dato crudo, así que el suavizado es reversible y no se pierde nada.

**Cada serie del gráfico tiene ventana mínima de eje** (10 °C, 40 puntos de %).
Autoescalar convertía una temperatura estable en una cordillera.

**Las alarmas suenan una vez**, y deciden sobre la media de 15 s, no sobre el
valor instantáneo: el ToF cruza cualquier umbral por ruido varias veces por
hora.

**Deshacer es un intercambio**, no una restauración: volver a pulsarlo rehace.

**El historial de sesión vive en el navegador**, aparte del deshacer del
firmware, para que exista aunque el aparato se reinicie o se reflashee.

---

## Infraestructura

- **GitHub:** `cristianandreseb8/proofbox-volumen`, Pages desde `main`.
- **Supabase:** proyecto `blqcppmjejtnvoqlhshp`. Tablas `proofbox_config` y
  `proofbox_readings`, edge function `proofbox-logger`.
- **MQTT:** `broker.hivemq.com` público. Corta conexiones a su antojo; la app se
  repone sola. Si hace falta fiabilidad de verdad, toca broker propio.
- **Registro:** intervalos ≥60 s los mueve la nube 24/7; <60 s solo mientras la
  app está abierta y en primer plano (el navegador estrangula las pestañas
  ocultas a ~1 tick/minuto).
- **Preview local:** `.claude/launch.json` levanta `python3 -m http.server 8747`.
  Hace falta servidor real — con `file://` el websocket al broker no abre.

`raw` en `proofbox_readings` guarda el JSON de status completo, así que los
campos nuevos de sensores ya están ahí antes de existir como columnas.

---

## Convenciones

- Mensajes de commit en inglés. Interfaz de la app en inglés.
- Comentarios de código en español, siguiendo lo que ya hay en los dos ficheros.
- Los comentarios explican **por qué**, no qué. Varios documentan un fallo real
  que costó encontrar; no los borres al refactorizar.

---

## Pendiente

- **Los electrodos nunca se han verificado en líquido.** Marcan 4.7 kΩ clavado,
  que es exactamente la resistencia en serie — señal de que el cable del GPIO 34
  puede no estar haciendo contacto. Con los electrodos al aire debería decir
  "sin contacto". La prueba es un vaso de agua: si el número no se mueve, revisar
  ese cable.
- **Firmware v1.4 compilado pero sin grabar** (deshacer, `metaSetStartAt`,
  alarmas de una sola vez). Sin él, el botón Undo y Restore no hacen nada.
- La sonda de temperatura está al aire, no en la masa. Da saltos de grados en
  segundos porque el aire no tiene inercia. Lo correcto es una vaina de inox
  clavada en la masa, o la sonda pegada a la pared del bote bajo aislante.
- Módulo de calor/frío, sin empezar.
