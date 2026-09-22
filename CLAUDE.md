# ProofBox — módulo Volumen

Mide cuánto sube una masa (ToF), su temperatura (DS18B20) y su conductividad
(dos electrodos), y lo publica por MQTT. La app web lo lee en vivo y registra
el histórico en Supabase.

Tres piezas: `ProofBox_Volumen.ino` (firmware del sensor), `ProofBox_Cam/`
(firmware de la cámara) e `index.html` (app). La app se despliega sola en
GitHub Pages al pushear a `main`.

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

### Cámara: Seeed XIAO ESP32S3 Sense (placa aparte)

Sensor OV3660. Dos trabajos:

- **En vivo por MQTT, siempre que está enchufada** (`ALWAYS_LIVE`, decisión del
  usuario: el vídeo tiene que estar al abrir la app). JPEG 480×320 a
  `proofboxcam/proofbox-cam01/live`, ~6,5 fps, ~5 KB. La app se suscribe sola al
  abrirse; deja de recibir con la pestaña oculta y reanuda al volver; "Pause
  live" es por aparato y se recuerda. Cuesta calor: cámara y radio sin
  descanso. Con `ALWAYS_LIVE=false` vuelve al modo a demanda: la app ya manda
  latidos a `.../viewer` cada 5 s y la placa corta a los 15 s sin ellos.
  Estado retenido en `.../state` (live / idle / offline como última voluntad).
  Los fotogramas se publican con `beginPublish`, sin búfer grande.
- **Abrir el puerto serie reinicia la placa** (USB nativo del S3): la app verá
  "offline" unos segundos.
- **Disparo a mano** desde la app: `proofboxcam/proofbox-cam01/cmd` con `shot`.
  Se apunta en una bandera y se hace en el `loop`; capturar y subir dentro del
  callback de MQTT bloquearía el cliente varios segundos. La foto entra como una
  de archivo, con su hora.
- **Archivo cada 10 min** a Storage: `cam/proofbox-cam01/shots/<epoch>.jpg`
  (XGA, calidad 10) y la misma como `latest.jpg`. Sin hora NTP no se archiva.
  ~30 KB por foto hoy; a 144 al día el plan gratis aguanta meses.

La cámara se inicia al tamaño MAYOR (XGA) y baja a 480×320 para el vivo:
agrandar en caliente corta las fotos. No hay servidor web en la placa: la app va
por https y el navegador bloquearía un `http://` de la red local.

**El giro por grados es de la app, el volteo del sensor.** El sensor solo sabe
voltear (arriba-abajo, izquierda-derecha); cualquier ángulo —12°, 90°, lo que
haga falta con la cámara sujeta a mano— se hace en la app con `transform`, más
un `scale` calculado para tapar las esquinas vacías. El ángulo vive en
`localStorage` (`pb-cam-rot`), es del aparato y no de la hoja, y **se cuece en
el JPEG al guardar** (`bakeRot`): lo que va a un paso y al informe sale derecho
aunque se mire desde otro sitio. Los ficheros del archivo se quedan en crudo y
se pintan girados.

**Enderezar con Claude** (`straightenWithClaude`). Lo que manda es que la TAPA
quede paralela al suelo. Primero se le pidieron grados a ojo (-8° con un 22%,
torcido); luego puntos —centro de tapa y base— (mejor, pero impreciso). Ahora
marca SEGMENTOS rectos: borde de la tapa, línea de la base, borde del suelo o la
mesa, los dos lados del frasco. El ángulo sale de ellos con geometría
(`angleFromLines`): horizontales a 0°, verticales a 90°, pesando tapa 3, suelo y
base 2, lados 1, por longitud, con media circular del ángulo doble. Qué lado es
arriba lo decide solo el eje base→tapa (`upsideFix`). Después una **segunda
pasada** sobre la imagen ya girada mide lo que falta y lo corrige si son ≤15°. En
la prueba: 115°, con -7,3° de retoque, y la tapa quedó horizontal. Usa el
fotograma del vivo; "What to level" le dice qué buscar.

**En pantalla completa nativa solo se pinta lo que está DENTRO del elemento.**
El diálogo de la tuerca, los avisos y la alarma se abrían detrás y había que
salir para verlos. `adoptOverlays()` los muda dentro de `#cam-full` al abrir y
`releaseOverlays()` los devuelve al cerrar. Cualquier overlay nuevo que deba
verse en pantalla completa va en `FS_OVERLAYS`.

**Cajas de la pantalla completa**: se arrastran desde cualquier parte y se
agrandan por la esquina (0,6× a 4×, escalado entero con `transform` para que un
gráfico agrandado sea el mismo gráfico). Posición y tamaño en fracciones de la
pantalla (`pb-hud-pos`), así sobreviven a girar el móvil; nunca se salen. Se
crean una vez y solo se actualiza su contenido, o el arrastre se rompería a
medio gesto. La caja "Chart" es el MISMO `drawChart` que el de la app — mismas
series y suavizado —, pero SOLO del paso en curso: con la cámara delante
interesa esta subida, no la hoja entera. Antes era un minigráfico de temperatura.

**El giro se comparte entre aparatos** como mensaje retenido en
`proofboxcam/proofbox-cam01/rot` (`{deg, subject}`), no en `proofbox_state`: no
toca la fila compartida y llega al instante a cualquiera que se conecte. En
sandbox no se publica.

**Pantalla completa: la imagen girada se ve ENTERA** (`rotFit`), no ampliada
para tapar esquinas como en la tarjeta: en un móvil alto eso cortaba medio
frasco. La X flota arriba a la derecha sobre todo — antes, con la imagen girada,
la barra quedaba tapada y no había forma de salir. Métricas pequeñas a izquierda
o derecha, elegidas con ⚙: nombre de la hoja, paso, progreso, gráfico de
crecimiento, temperatura, gráfico de temperatura, temporizador, conductividad.

**Zoom en pantalla completa**, como el de Chrome: − y + con pasos fijos (100,
110, 125, 150, 175, 200, 250, 300, 400, 500 %), el % a la vista (tocarlo vuelve a
100 %), teclas + − 0 y pellizco de trackpad (llega como rueda con Ctrl).
Deslizar dos dedos por el trackpad o la rueda sola MUEVEN la imagen — al
principio también ampliaban y no había forma de recentrar. En el móvil, dos
dedos amplían y desplazan; uno arrastra; doble toque 100 ↔ 200 %; los botones
+ − se ocultan con `(hover:none)`. El desplazamiento va antes del giro en el
`transform`, así arrastrar a la derecha mueve a la derecha aunque esté girada.
**Zoom y posición se recuerdan** (`pb-cam-zoom`, la posición en fracciones de la
pantalla) al cerrar, reabrir y recargar. Al abrir, recuperarlos es lo PRIMERO:
cualquier repintado previo recalculaba la fracción con la posición a cero.

**Cuidado con los ajustes del sensor del OV3660.** El firmware 2 llevaba un
`tuneSensor()` generoso —corrección de lente, gamma, escalado (`dcw`), nitidez,
modo de poca luz (`aec2`)— y con eso el sensor dejó de bajar de tamaño para el
vivo: fotogramas de 1024×768 y 75 KB en modo "fluido", `FB-OVF`, y el broker
cortando la conexión en bucle (estado -3), una imagen cada diez segundos. El
firmware 4 solo limita la ganancia. Medido después: Fluid 6,6 fps · 7 KB,
Balanced 2,2 fps · 21 KB, Sharp 1,7 fps · 36 KB, sin un corte. El firmware
imprime cada 50 fotogramas cuánto tarda la captura y el envío, y avisa si el
vivo no bajó de tamaño: es el diagnóstico.

**Calidad del vivo elegible** (Fluid / Balanced / Sharp): 480×320 a ~6 fps,
800×600 a ~3 fps, 1024×768 a ~1-2 fps. La decide la placa (`cmd` `q:0|1|2`, en
NVS) y contesta retenido en `.../quality`; si no contesta, lleva firmware
anterior y la app lo dice. Lo que limita es subir cada JPEG por WiFi al broker,
no el sensor. `tuneSensor()`: corrección de lente y de píxeles muertos, gamma,
exposición automática con modo de poca luz, ganancia limitada a 8× (más ganancia
= más grano; la masa no se mueve y es mejor exponer más), nitidez y reducción de
ruido.

**Las fotos de archivo salían ROTAS con el vivo siempre encendido**: cambiar de
480×320 a 1024×768 en caliente daba un fotograma de 480×320 lleno de bandas.
`archiveShot()` ahora reinicia la cámara a tamaño grande (~1 s cada 10 min) y
descarta cualquier foto de archivo de menos de 1000 px de ancho.

**Girar la imagen se hace en el SENSOR** (`set_vflip` / `set_hmirror`), no con
CSS: así sale derecha también en las fotos que se guardan, no solo en el vivo.
Se pide con `cmd` `flip` / `mirror` (o `flip:0|1`), se guarda en NVS —una cámara
colgada boca abajo lo sigue estando tras un corte de luz— y la placa contesta
con el estado retenido en `.../flip` como `v,h`, que es lo que enciende los
botones de la app.

**Pantalla completa** con capa propia (`.cam-full`) además del Fullscreen API:
en iPhone ese API solo existe para vídeo. Mientras está abierta, los fotogramas
del vivo se pintan también ahí — es la misma suscripción, no una segunda. Sirve
igual para una captura guardada.

Las capturas se borran desde la app, una a una o todas, con un solo `DELETE` y
la lista en `prefixes` (por tandas de 100: una lista de cientos falla). Borrar
una captura no toca la copia que se hubiera añadido a un paso.

**La IA mira el frasco** (🤖 en la tarjeta de la cámara; 👁 lo enseña u oculta
sin pararlo). Complemento del ToF, no sustituto: la cámara ve en ángulo, el ToF
mide, así que manda el ToF y se enseñan los dos porcentajes. Claude marca base y
tapa del frasco (su eje), media anchura, la cima de la cúpula y los extremos de
la línea de plumón, y dice si la masa la alcanzó. Las alturas se miden A LO LARGO
DEL EJE en fracciones del largo del frasco: no cambian al acercar, girar o mover
la cámara. Se analiza siempre la foto entera, no lo ampliado. El progreso cuenta
desde la primera medida del paso (`visionStartH`). El aviso suena una vez por paso
y con DOS lecturas seguidas (un reflejo en el cristal no basta).
Dibujo: capa SVG del tamaño de la parte que pinta la foto (`contain`) con el
MISMO transform que la imagen y el mismo centro, así acompaña giro, zoom y
arrastre. Un solo análisis para todos los aparatos: el resultado va retenido a
`.../vision` y un aparato solo analiza si el último es más viejo que el
intervalo (2/5/10/15 min). Cada análisis es una llamada a Claude: cuesta.
Solo corre con la app abierta en algún aparato.

**Las marcas del panadero no las mueve la IA.** Se pintan donde él las puso; la
IA solo sube el borde de arriba de la masa a la altura que mide. Sin marcas no se
pinta NADA ni se analiza (no se gasta una consulta). 🗑 borra todo, también el
análisis retenido en el broker.

**Seguir al frasco es cosa de píxeles, no de la IA.** Se probó con la IA: de una
lectura a otra, sin tocar nada, su frasco cambiaba de tamaño hasta un 18%, y las
marcas se desplazaban solas. Ahora, al configurar, el frasco se guarda como
plantilla en grises (160 px de ancho de trabajo) y cada 4 s se busca en la
imagen nueva por correlación normalizada, a 7 tamaños, afinando cada uno. Las
marcas se mueven solo si la plantilla aparece con parecido ≥ 0,55 en otro sitio
(>3%) o de otro tamaño (>8%) en DOS búsquedas seguidas, y se mueven enteras
(traslación y escala: proporciones intactas). A igualdad casi exacta se queda el
tamaño que tenía — si no, un tamaño vecino ganaba por medio píxel y las marcas
se movían con la cámara quieta. Medido: quieta, parecido 1,00 y nada se mueve;
movida (desplazada y al 85%), centro con <1% de error, tamaño 0,85 exacto,
~23 ms por búsqueda. Ojo: `grayFrom` usa `naturalWidth`; con `width` de una
<img> de la página la plantilla salía deformada.

**Sin textura no se sigue nada.** De noche la foto media 8 de 255 (desviación 3)
y la búsqueda encontraba el frasco cada vez un 3% más allá: arrastraba las
marcas y el × de la masa bailaba (1,86 → 1,7) con la masa y la cámara quietas.
Ahora, con plantilla o imagen de desviación < 8, las marcas no se tocan; y para
moverlas hace falta parecido ≥ 0,7, un desplazamiento > 6% (o 12% de tamaño) y
TRES búsquedas seguidas que coincidan.

**Coordenadas canónicas (4:3 del sensor).** El vivo Fluid es 3:2 y el Sharp y el
archivo 4:3; el 3:2 es la franja central (y = 1/18 + y·8/9). Marcar en uno y
pintar en otro desplazaba las marcas y les cambiaba las proporciones. Todo lo
guardado (marcas, zona de búsqueda, plantilla) va en canónico y se traduce al
formato de la imagen que se pinta o analiza (`toCanon`/`fromCanon`).
Configuraciones anteriores a esto (sin `canon`) se descartan.

**La cima de la masa se mide con píxeles; la IA es el apoyo.** La IA, con una
foto oscura, no se atrevía a mover el recuadro azul. Cada 2 s (`measureDough`)
se recorre el frasco marcado a lo largo de SU eje, 120 filas × 24 columnas del
centro, y la cima es hasta dónde sigue habiendo claro subiendo desde la mitad
del recuadro de masa (huecos de 3 filas tolerados). El umbral sale de lo marcado:
la mitad baja del recuadro de masa es masa seguro, el fondo es el percentil 15
del frasco, y el corte a medio camino. Un Otsu de toda la imagen daba 95 con la
masa a ~58 y medía 3% en vez de 30%. La fiabilidad sale de la FORMA del perfil (filas
llenas dentro de la masa, vacías encima) y del contraste en proporción, no de
niveles absolutos: de noche la masa está a 12 y el fondo a 7, y pedir 10 niveles
de diferencia dejaba el recuadro quieto con la masa a la vista. Mediana de 7 medidas; aviso con 5 seguidas
en la línea. El recuadro azul lleva dentro el × crecido (grande) y el % de la
meta (pequeño). El inicio del ×, si se acaba de marcar, es la misma medida de
píxeles (`pixStart`); para configuraciones viejas, las marcas a mano — medirlo
en la foto del archivo de entonces dio 1,39× con 1,89× real (foto negra,
cámara movida). Mediana de 30 medidas (1 min). ◐ enseña la vista de alto contraste (masa ámbar, resto azul) y se
vuelve a la normal con el mismo botón; la IA la recibe como "Image 3".
**La IA ya no mira sola: solo "✨ Calibrate with AI".** Decisión del usuario por
el gasto (a Opus cada 5 min eran ~15-25 USD al día con la app abierta). Una
consulta a Sonnet 5 por pulsación; su `dough_fraction` corrige la medida de
píxeles con un desplazamiento (`visSetup.pixCal`, fracción del frasco) que se
queda hasta la próxima calibración. Solo si está segura (≥0,45) y como mucho
±15%: más que eso es que las marcas están mal y se pide volver a marcar.
Marcar con 🎯 ya no lanza una consulta.

**Configurar lo que mira la IA: 🎯 frasco · masa · objetivo.** Tres gestos sobre
la imagen: recuadro al frasco, recuadro a la masa, línea del objetivo. De ahí
salen dos PROPORCIONES de la altura del frasco (`hDough`, `hTarget`), medidas en
la pantalla tal como se ve, que no dependen de dónde esté la cámara. Se guarda
además una foto de referencia del frasco (`ref.jpg` en Storage, 384 px) que se le
enseña a la IA como "Image 1" en cada análisis: así vuelve a encontrar ESE
recipiente aunque la cámara se mueva o haya otros al lado, y juzga la altura de
la masa comparando con cómo estaba (`dough_fraction`) — marcar la base con
precisión falla (se colaba el táper de debajo y salió un 94% falso), comparar
alturas no. La línea del objetivo se vuelve a proyectar sobre el frasco donde
esté ahora. Si no lo encuentra en la zona de búsqueda, mira la foto entera.
Por debajo de 0,45 de confianza no se da porcentaje ni suena el aviso: "not sure
this time — the ToF still leads". Cada análisis tarda ~50 s (dos imágenes y, si
se movió, una segunda consulta). La configuración viaja retenida en `.../setup`.

**Arrastrar sobre una `<img>` inicia el arrastre nativo de ficheros** y se come
el gesto: por eso "no dejaba hacer el recuadro" en el ordenador. Las imágenes
de la cámara llevan `draggable=false` y `-webkit-user-drag:none`, y en el móvil
la zona pasa a `touch-action:none` mientras se configura, o el dedo desplazaba
la página.

**Cualquier recipiente, y el panadero puede enmarcarlo.** La descripción ("What
to level", por defecto "frasco o vaso con masa") es una pista, no una regla: el
prompt acepta frasco con o sin tapa, vaso, taza, bol o tarrina, y "tapa" es la
tapa o el borde de la boca. Con ⬚ se dibuja un recuadro sobre la imagen; se
guarda en coordenadas de la FOTO (`screenToRaw` deshace el transform de la
imagen con la inversa de su matriz y el `contain`; comprobado ida y vuelta con
giro 30°, zoom 1,5 y desplazamiento) y la IA mira solo ese recorte con un 25% de
margen, ampliado si es pequeño; las coordenadas se devuelven a la foto entera
(`uncrop`). Con tu captura: sin recuadro 32-38% de confianza; con recuadro 72% y
el frasco bien orientado. Tras una detección buena el recuadro se reajusta al
recipiente, pero uno puesto a mano solo se mueve si lo encontrado cae dentro. Si
el recuadro es automático y no encuentra nada dentro, mira la foto entera. El
recuadro viaja retenido en `.../hint`.

**Actualización por el aire (OTA).** Desde el firmware 2 la placa mira
`fw/cam.json` en GitHub Pages al arrancar (pasado un minuto), cada 6 h y cuando
la app manda `update`; si la versión publicada es mayor que su `FW_VERSION`, se
descarga el `.bin` y se lo instala (`HTTPUpdate`). Solo de esa dirección: quien
puede cambiar el firmware es quien hace push al repositorio, no quien escribe en
el broker público. Una imagen rota no arranca: la placa sigue con la de antes.
Publicar: subir `FW_VERSION`, `./fw/publish-cam.sh`, commit y push de `fw/`.
La versión y el "updating" van retenidos en `.../fw` y la app los enseña. La
primera instalación con OTA tuvo que ser por USB.

**Todo es público**: el broker es público y el bucket también, y los nombres
están en el HTML. Quien los lea puede ver el vivo y el archivo.

- **Compilar con `PSRAM=opi`**: `esp32:esp32:XIAO_ESP32S3:PSRAM=opi`. Sin PSRAM
  no cabe un fotograma de 800×600.
- **Necesita la antena externa** (plana, conector U.FL). Sin ella oía una sola
  red a -91 dBm y la de casa ni aparecía; con ella, 13 redes y la de casa a
  -57. El firmware lista las redes con su señal al arrancar: es el diagnóstico.
- **WiFi con WiFiManager**: sin red guardada abre `ProofBox-Cam`
  (192.168.4.1). Muestra también las redes débiles (`setMinimumSignalQuality(0)`)
  y reintenta 3 veces. Si la señal es buena y aun así falla, es la contraseña.
- **Se calienta.** Fuera del vivo la cámara está apagada y la radio duerme.
  Trae disipadores: van sobre el chip de la XIAO.
- **macOS:** la primera vez no aparecía ningún puerto USB — ni en `ioreg`. Era un
  cable de solo carga (la luz amarilla de la placa se encendía igual). El puerto
  bueno es `/dev/cu.usbmodem101`; se graba sin pulsar BOOT.
- La foto es pública: la URL está en el HTML de la app.

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
Cualquiera de ellos abre el primer paso ("1st reading"), reclama el aparato y pone en
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

**El porcentaje de la tarjeta sale SOLO de centímetros.** La razón de distancias
(inicio÷ahora) no es el × del bote: se dispara cerca del sensor. Una hoja con el
objetivo solo en × ("LMM", 2,7×) daba 58% con la masa a 2,3 cm de 2,5 (91%) —
el panadero lo vio como "marca 30% cuando va en 80%". La causa: el diálogo Next
step mandaba los cm al aparato pero no los guardaba en la hoja; cuando el
progreso pasó a calcularse en la app, esa hoja se quedó sin cm. Ahora Next step
los guarda (hoja y paso), y `jarEquivalence()` resuelve los cm: de la hoja; si
solo tiene ×, con la equivalencia de la última hoja que tenga los dos (el mismo
bote, casi siempre), diciéndolo en la tarjeta; y si no hay ninguna, `--%` y se
piden los cm. El gráfico (`stageGoal`) usa lo mismo.

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

**El límite inferior del gráfico es el PRIMER comienzo de la hoja, no el cero
actual.** `baseAt` se mueve con cada Mark start, y usarlo como límite hizo que al
empezar el 2º refresco de "Panettone habana" desapareciera el 1º — 9 h y 597
lecturas invisibles, intactas en `proofbox_readings`. Si un paso deja de verse,
mirar primero los filtros de `rowsIn()`: la base de datos casi seguro está bien.
Cada paso guarda además su propio cero (`zeroDist`, `zeroAt`).

**Entre pasos no se dibuja nada.** El sensor suele estar en el aire o en la
mano; esas filas no son de ningún paso.

**El × del gráfico es el de la tarjeta, no el `rise` del aparato.** El firmware
guarda inicio÷ahora en distancias, y eso no es lineal: se dispara cuando la
masa se acerca al sensor. Dos pasos con el mismo objetivo salían uno gigante
(3,4 cm → 2,8×) y otro enano (1,9 cm → 1,55×), con la línea del objetivo en
otra escala. `withJarX()` recalcula cada fila como la tarjeta: cm desde el cero
de SU paso y, con equivalencia, el × del bote (1,9 cm de 2,5 = 2,29×, el 76%).
Sin equivalencia, inicio÷ahora con el cero del paso. El crudo queda en
`rise_device`. Cada paso guarda su cero y su objetivo; los antiguos sin cero
usan la mediana de sus diez primeros minutos. La columna es `dist_mm` — con
`dist` `distAt()` nunca encontró nada.

**La línea del objetivo es por paso.** Un tramo a la altura del objetivo de cada
paso, con hueco entre pasos igual que la curva; una recta de lado a lado hacía
creer que el 2º refresco perseguía la meta del 1º. El ✓ de cada tramo sale del
percentil 95 del paso, no del máximo: un pico del ToF lo marcaba antes de tiempo.

**El aviso `meta_reached` del aparato se ignora.** Cuenta desde su cero único y
con su × de distancias; decía "objetivo alcanzado 1,9×" con la hoja al 88%. La
alarma la decide la hoja.

**Pasos marcados en el gráfico, como las secciones de GarageBand** (botón
"⎸⎸ Steps" del gráfico grande). Una marca es una frontera: tocar parte el paso
ahí (`splitAt`); arrastrar de A a B crea un paso de A a B, y dentro de un paso lo
parte en tres (`sectionRange`); arrastrar una línea la mueve y el vecino que la
comparte se mueve con ella. No se permite tragarse un paso entero. Los pasos
nacen MEDIDOS —el dato ya está grabado— con su cero en la lectura de ese momento
(`distAt`) y el objetivo del paso del que salen. Recién marcado, se abre el
cuadro para ponerle nombre encima de su etiqueta.

**Renombrar no es editar.** Tocar el nombre (en la lista o en la franja del
gráfico) abre un cuadro en el sitio: Enter guarda, Esc cancela. Y el diálogo
**Edit** ya no convierte un paso medido en manual —guardaba `manual:true`
siempre— ni le mueve las horas, que redondeaba al minuto: solo cambian si se
tocaron los campos de fecha, hora o duración.

**Un hueco largo sin lecturas corta la línea** (más de 10 min, u 8 veces el
intervalo típico): la recta entre los dos lados dibujaba horas de subida que
nadie midió.

**La goma se ve como una goma.** Cursor propio mientras está activa, la línea
desaparece bajo el arrastre (una máscara SVG sobre las capas de datos, la
rejilla queda), y al confirmar la curva queda CORTADA, no unida con una recta
que dibuje una subida que nadie midió. Lo mismo entre pasos. ↶ Undo erase
recupera el último tramo; las marcas de dónde hay algo oculto solo se ven con
la goma en la mano.

**La goma no borra: oculta.** Arrastrar sobre el gráfico grande guarda el tramo
en `sess.erased`; `rowsIn()` y `sensorRows()` lo excluyen y se puede recuperar.
Nunca un DELETE en `proofbox_readings` — lo que se grabó queda grabado. El
informe recibe los tramos como `hidden_by_baker` y tiene prohibido tratarlos
como huecos.

**Una fila del histórico es de la hoja solo si es posterior a su marca de
inicio.** El registro de la nube escribe cada minuto pase lo que pase, así que
`proofbox_readings` tiene datos del aparato siempre. Sin filtrar por `baseAt`,
una hoja recién creada nacía con puntos en el gráfico y en el informe que no
eran de su masa. Los tramos de pasos anotados a mano se descuentan también, y
`sensorRows()` usa exactamente el mismo criterio que el gráfico: si dan números
distintos, es un fallo.

**Sin señal no se atenúa nada.** Media app en gris parecía rota, y la cámara,
las hojas, el gráfico y el informe no dependen del sensor. Solo se enseña el
aviso, que ahora vive arriba del tablero y no escondido en Ajustes, y que
también aparece cuando nunca llegó ninguna lectura.

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
  Abrirla con `?sandbox` (ver "Cuidado al probar").

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
local la lee y la escribe igual que el móvil. Gana el último que escribe.

**Probar SIEMPRE en modo aislado.** En `localhost` lo es por defecto (`?live` lo
desactiva a propósito); en cualquier otro sitio, con `?sandbox`. En ese modo un envoltorio de
`fetch` al principio del script corta toda escritura a Supabase (hojas, filas,
configuración, fotos) y `sendCmd` no manda nada al aparato; lecturas y
`claude-proxy` sí pasan. Las hojas van a otra clave de localStorage. Hay un
aviso fijo abajo. Anular funciones a mano (`pushState=()=>{}`) no basta: se
recarga la página y la protección desaparece.

Así se borraron hojas reales dos veces. La segunda (2026-09-17 06:41 UTC): vaciar
`pb-sessions` en la preview hizo que la migración antigua resucitara una hoja
de `pb-session` y la subiera encima de las cuatro buenas. Se recuperaron porque
el Chrome del usuario aún las tenía en local y las volvió a subir. Desde
entonces la migración no corre si el aparato ya sincronizó alguna vez
(`pb-synced-once`).

**Historial de `proofbox_state`.** Un trigger guarda cada versión anterior en
`proofbox_state_history` (las 500 últimas por aparato, sin acceso para `anon`).
Si una sobrescritura se lleva algo, está ahí. Nunca limpiar esa tabla.

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
