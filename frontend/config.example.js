// ESP32 Counter — Firebase web config template.
//
// Copy this file to `config.js` (gitignored) and fill in your values:
//   cp config.example.js config.js
//
// apiKey      : Firebase Console → Project Settings → Your apps → Web app →
//               "API key". NOT secret (required for Google sign-in).
// authDomain  : "<project-id>.firebaseapp.com" (Firebase Auth → Settings).
// databaseURL : RTDB instance URL (Realtime Database →
//               "https://<project>-default-rtdb.firebaseio.com").
// deviceId    : the board's chip-unique id (see src/utils/device_id.cpp, lower
//               24 bits of the efuse MAC). With multiple boards each node lives
//               under /devices/<deviceId>/ — set this to the board you target.
// adminEmails : Google accounts granted full control. MUST match the
//               isAdmin() list in database.rules.json (that file is the real
//               enforcement; this list only drives the UI state).
window.FB_CONFIG = {
  apiKey: "YOUR_WEB_API_KEY",
  authDomain: "esp32-electricity-counter.firebaseapp.com",
  databaseURL: "https://esp32-electricity-counter-default-rtdb.firebaseio.com",
  deviceId: "esp-000000",
  adminEmails: ["heng.xiao.hour@gmail.com"]
};