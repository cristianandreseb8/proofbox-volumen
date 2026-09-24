#!/bin/sh
# Publica un firmware nuevo del sensor para que se actualice solo por el aire.
# 1. Subir FW_VERSION en ProofBox_Volumen.ino.
# 2. ./fw/publish-vol.sh   (compila, copia el .bin y escribe fw/vol.json)
# 3. git add fw && git commit && git push — la placa lo recoge en ≤6 h, o al
#    momento con "Check for update" en Ajustes.
# La primera vez (firmware 5) hay que grabarlo por USB: los anteriores no
# sabían actualizarse.
set -e
cd "$(dirname "$0")/.."
V=$(grep -E '^const int +FW_VERSION' ProofBox_Volumen.ino | grep -oE '[0-9]+')
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
OUT=$(mktemp -d); SK=$(mktemp -d)/ProofBox_Volumen
mkdir -p "$SK"; cp ProofBox_Volumen.ino "$SK/"
"$CLI" --config-file ~/.arduinoIDE/arduino-cli.yaml compile -b esp32:esp32:esp32 --output-dir "$OUT" "$SK"
cp "$OUT/ProofBox_Volumen.ino.bin" "fw/vol-$V.bin"
python3 -c "import hashlib,json; b=open('fw/vol-$V.bin','rb').read(); json.dump({'version':$V,'url':'fw/vol-$V.bin','md5':hashlib.md5(b).hexdigest(),'bytes':len(b)},open('fw/vol.json','w'))"
echo "Publicado firmware $V: fw/vol-$V.bin"
echo "Para grabarlo por USB: \"$CLI\" --config-file ~/.arduinoIDE/arduino-cli.yaml upload -b esp32:esp32:esp32:UploadSpeed=115200 -p /dev/cu.usbserial-0001 --input-dir \"$OUT\" \"$SK\""
