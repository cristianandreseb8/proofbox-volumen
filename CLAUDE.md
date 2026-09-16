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

**Una fase solo se da por cerrada si el aplanamiento se sostiene.** La versión
anterior cerraba la logarítmica con UNA muestra por debajo del 20% del pico, y
con el ruido del ToF eso pasa varias veces por hora: había informes que daban
por estacionaria una masa que seguía subiendo. Ahora el pico se toma como
percentil 90 (un salto del suavizado no fija la vara), la calma tiene que durar
una hora con menos de un 3% de crecimiento, y no se afirma nada antes de 3 h
porque el libro sitúa la logarítmica en 4-6 h tras la latencia. El prompt lleva
además la regla de que `log_end_h: null` no es un dato que falte, es la
afirmación de que la fase no ha terminado.

**Tres gestos empiezan una hoja: Start reading, Mark start o el temporizador.**
Cualquiera de ellos abre el primer paso ("Step 1"), reclama el aparato y pone en
marcha el registro — sin objetivo, que es una decisión aparte. A partir de ahí
todo lo que viene son Next steps. Exigir una etapa abierta *y* propiedad del
aparato dejaba sin registrar una masa que ya estaba subiendo.

**cm y × son el mismo objetivo dicho de dos maneras.** El sensor mide
centímetros; el panadero lee el múltiplo en el bote graduado. Puestos los dos
forman una equivalencia —"2,5 cm en este bote son 2,7×"— y con ella se interpola
el × mientras sube: 0 cm es 1×, el objetivo en cm es tu ×. Es tu bote el que
manda, no la razón de distancias del sensor, que no sabe cómo de ancho es. La
barra enseña `2.5cm = 2.7×` y debajo lo crecido en las dos unidades. Hubo una
versión que los hacía excluyentes; era peor.

**Cambiar el objetivo repinta en el acto.** El porcentaje se recalculaba en el
siguiente status del ESP32; con el aparato apagado no llega ninguno y parecía
que el botón no había hecho nada. `repaint()` reusa el último status guardado.

**Inicio y objetivo se corrigen diciéndolo.** El diálogo tiene campos, pero la
vía normal es escribir la frase ("le dije 2,5cm pero era 2,1") y que se aplique
sola, diciendo qué entendió. Hubo una versión que solo rellenaba los campos y
esperaba un Save: parecía que el botón no hacía nada.
Las horas van siempre en la del panadero — se le pasa su zona y se le prohíbe
UTC, porque una confirmación en UTC junto a un campo en local parece un error.

**Inicio y objetivo se pueden corregir con la masa subiendo.** Todo lo que se
enseña se deriva de `baseDist`, `baseAt` y el objetivo, así que cambiarlos
reajusta también el tramo ya recorrido — que es justo lo que se pide cuando te
das cuenta a mitad de que marcaste 2,5 cm en vez de 2. Mover la hora de inicio
recoge la altura de partida del histórico en ese momento, no la de ahora.

**El objetivo en cm se mide en cm.** No se traduce a ratio: es la marca que
hiciste en el bote, y el porcentaje sale de `cm/goalCm`.

**Las fotos del informe van donde el texto habla de ellas.** El modelo deja
`<figure data-photo="N">` en su sitio y la app lo rellena; lo que no colocó se
agrupa al final. Una foto a tres pantallas de su párrafo no ilustra nada.

**Una fila del histórico es de la hoja solo si es posterior a su marca de
inicio.** El registro de la nube escribe cada minuto pase lo que pase, así que
`proofbox_readings` tiene datos del aparato siempre. Sin filtrar por `baseAt`,
una hoja recién creada nacía con puntos en el gráfico y en el informe que no
eran de su masa. Los tramos de pasos anotados a mano se descuentan también, y
`sensorRows()` usa exactamente el mismo criterio que el gráfico: si dan números
distintos, es un fallo.

**Las tarjetas no se esconden nunca.** Temperatura y conductividad son lo que
marca el instrumento y existen igual en un paso anotado a mano; la de
crecimiento es donde vive el botón de marcar inicio, así que ocultarla dejaba a
una hoja recién creada sin manera de empezar. Lo que depende de la naturaleza de
la etapa es el **registro**, no la vista: en un paso manual se ve lo que el
aparato lee en ese momento y no se guarda ni una fila. Hubo una versión que las
escondía y era desconcertante.

**El estado sincronizado lleva texto; las fotos van aparte.** `proofbox_state`
guarda las hojas enteras en una fila, y meter ahí una foto en base64 es escribir
megas en cada pulsación. Los bytes viven en Storage bajo `<id>.jpg` y en la hoja
solo viaja el id. Al pintar se mira primero IndexedDB —instantáneo— y si no está
se baja y se cachea. Las fotos anteriores a esto solo existen en el aparato que
las sacó: `backfillPhotos()` las sube la próxima vez que esa app se abre.

**El cero de la masa es de la hoja, no del aparato.** El ESP32 guarda uno solo,
así que mientras los números salieran de él marcar el inicio en una hoja borraba
el de la otra. Cada hoja guarda su `baseDist` y calcula con la misma fórmula del
firmware (`ratio = d0/d`). Así se pueden llevar varias lecturas del mismo bote a
la vez desde ángulos distintos. Al aparato se le sigue mandando la orden, pero
solo para sus propias alarmas.

**El porcentaje grande es contra TU ratio.** El 2,7× leído en el bote graduado
gana al objetivo del sensor, que solo sabe de distancias. El del sensor queda de
reserva para cuando no hay uno tuyo.

**El arrastre de las tarjetas cancela el `pointerdown`** de todo lo que no esté
en `isInteractive()`. Una `<img>` no estaba, y por eso las miniaturas de fotos
no se podían abrir en escritorio — en el móvil el arrastre ni empieza, así que
el fallo parecía inexistente. Si algo nuevo dentro de una tarjeta no responde al
clic, es esa lista.

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
- **Supabase:** proyecto `blqcppmjejtnvoqlhshp`. Tablas `proofbox_config`,
  `proofbox_readings` y `proofbox_state`, edge functions `proofbox-logger` y
  `claude-proxy`. Bucket público `proofbox-photos` (3 MB, solo jpeg/png/webp)
  con políticas de lectura, escritura y borrado para `anon`.
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

## Cuidado al probar

`proofbox_state` es **una sola fila compartida** (`device_id` fijo) y la preview
local la lee y la escribe igual que el móvil. Guardar durante una prueba pisa
las hojas reales: gana el último que escribe y no hay historial. Ya se perdió
una hoja así. Si hay que probar con hojas de mentira, anular antes `pushState`,
`pullState` y `fetchHistory`, y no llamar a `saveSessions()` hasta haberlas
quitado.

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
