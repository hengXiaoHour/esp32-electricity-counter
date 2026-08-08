#include "device_id.h"

String deviceId() {
  static String id;
  if (id.length() == 0) {
    // Use the low 24 bits of the efuse MAC — unique per chip, stable forever.
    uint32_t chip = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF);
    id = "esp-";
    id += String(chip, HEX);
  }
  return id;
}