#pragma once

// Firebase config — EXAMPLE TEMPLATE (safe to commit).
// Copy this file to `src/network/firebase_config.h` and fill in real values.
// `firebase_config.h` is gitignored; never commit real service-account keys.
//
// NOTE: the web API key is NOT required — auth uses the OAuth2 service-account
// flow (private_key + client_email + project_id), and RTDB rules allow the
// resulting identity ("auth != null"). Keep FIREBASE_API_KEY empty.

#define FIREBASE_API_KEY ""        // NOT needed for service-account auth
#define FIREBASE_DB_URL ""         // Realtime database URL
#define FIREBASE_PROJECT_ID "esp32-electricity-counter"
#define FIREBASE_CLIENT_EMAIL ""   // service account email
#define FIREBASE_PRIVATE_KEY \
  "-----BEGIN PRIVATE KEY-----\n" \
  "PASTE_YOUR_PRIVATE_KEY_HERE\n" \
  "-----END PRIVATE KEY-----\n"