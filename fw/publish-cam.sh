#!/bin/sh
# Publica un firmware nuevo de la cámara para que se actualice sola por el aire.
# 1. Subir FW_VERSION en ProofBox_Cam/ProofBox_Cam.ino.
# 2. ./fw/publish-cam.sh   (compila, copia el .bin y escribe fw/cam.json)
# 3. git add fw && git commit && git push — la placa lo recoge en ≤6 h, o al
#    momento con "update" desde la app.
set -e
cd "$(dirname "$0")/.."
V=$(grep -E '^const int +FW_VERSION' ProofBox_Cam/ProofBox_Cam.ino | grep -oE '[0-9]+')
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
OUT=$(mktemp -d)
"$CLI" --config-file ~/.arduinoIDE/arduino-cli.yaml compile -b "esp32:esp32:XIAO_ESP32S3:PSRAM=opi" --output-dir "$OUT" ProofBox_Cam
cp "$OUT/ProofBox_Cam.ino.bin" "fw/cam-$V.bin"
python3 -c "import hashlib,json,sys; b=open('fw/cam-$V.bin','rb').read(); json.dump({'version':$V,'url':'fw/cam-$V.bin','md5':hashlib.md5(b).hexdigest(),'bytes':len(b)},open('fw/cam.json','w'))"
echo "Publicado firmware $V: fw/cam-$V.bin"
