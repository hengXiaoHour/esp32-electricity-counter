// ESP32 Counter — Firebase web config template.
//
// Copy this file to `config.js` (gitignored) and fill in your values:
//   cp config.example.js config.js
//
// The databaseURL is the RTDB instance URL (Firebase Console → Realtime
// Database → "https://<project>-default-rtdb.firebaseio.com").
// deviceId is the board's chip-unique id (see src/utils/device_id.cpp, lower
// 24 bits of the efuse MAC). With multiple boards sharing one database each
// node lives under /devices/<deviceId>/ — set this to the board you target.
window.FB_CONFIG = {
  databaseURL: "https://esp32-electricity-counter-default-rtdb.firebaseio.com",
  deviceId: "esp-000000"
};
