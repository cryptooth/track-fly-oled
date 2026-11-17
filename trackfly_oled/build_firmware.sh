#!/bin/zsh
# Builds the firmware.
#   firmware/trackfly_oled-<ver>.bin           -> web update (device page > Firmware)
#   firmware/trackfly_oled-<ver>-full-usb.bin  -> first flash over USB at 0x0
#
#   ./build_firmware.sh            build only
#   ./build_firmware.sh upload     build and flash over USB (auto-detects the port)
#   PORT=/dev/cu.usbmodemXXXX ./build_firmware.sh upload
set -e
cd "${0:A:h}"
# min_spiffs: two 1.9 MB app slots, so web/self updates work.
FQBN="esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,FlashMode=qio,FlashSize=4M,PartitionScheme=min_spiffs"
VER=$(grep -m1 '#define FW_VERSION' trackfly_oled.ino | sed -E 's/.*"(.*)".*/\1/')
OUT=build
arduino-cli compile --fqbn "$FQBN" --output-dir "$OUT" --warnings default .
mkdir -p firmware
cp "$OUT/trackfly_oled.ino.bin" "firmware/trackfly_oled-$VER.bin"
# Full image for a first install over USB (bootloader + partitions + app), without the
# 4 MB padding of Arduino's merged.bin.
ESPTOOL=$(ls -d ~/Library/Arduino15/packages/esp32/tools/esptool_py/*/ | tail -1)esptool
BOOTAPP=$(ls ~/Library/Arduino15/packages/esp32/hardware/esp32/*/tools/partitions/boot_app0.bin | tail -1)
"$ESPTOOL" --chip esp32c3 merge_bin -o "firmware/trackfly_oled-$VER-full-usb.bin" --flash_mode keep --flash_size keep \
  0x0 "$OUT/trackfly_oled.ino.bootloader.bin" 0x8000 "$OUT/trackfly_oled.ino.partitions.bin" \
  0xe000 "$BOOTAPP" 0x10000 "$OUT/trackfly_oled.ino.bin" > /dev/null
echo "v$VER"
echo "web update : firmware/trackfly_oled-$VER.bin"
echo "USB (0x0)  : firmware/trackfly_oled-$VER-full-usb.bin"
if [[ "$1" == "upload" ]]; then
  PORT=${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}
  [[ -z "$PORT" ]] && { echo "No USB device found (/dev/cu.usbmodem*)"; exit 1; }
  echo "Flashing $PORT"
  arduino-cli upload --fqbn "$FQBN" --input-dir "$OUT" -p "$PORT" .
fi
