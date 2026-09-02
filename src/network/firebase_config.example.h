#pragma once

// Firebase config — EXAMPLE TEMPLATE (safe to commit).
// Copy this file to `src/network/firebase_config.h` and fill in real values.
// `firebase_config.h` is gitignored; never commit real credentials.
//
// Auth uses API key + email/password. Create a user in Firebase Console →
// Authentication (enable Email/Password sign-in), then set FIREBASE_AUTH_EMAIL
// and FIREBASE_AUTH_PASSWORD here. Keep the private key OUT (no service account).

#define FIREBASE_API_KEY ""         // Web API key (Firebase Console → Project settings)
#define FIREBASE_DB_URL ""          // Realtime database URL
#define FIREBASE_PROJECT_ID "esp32-electricity-counter"
#define FIREBASE_AUTH_EMAIL ""
#define FIREBASE_AUTH_PASSWORD ""
