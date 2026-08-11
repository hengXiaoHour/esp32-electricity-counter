# Plan: Admin (Google Auth) + Read-Only Guests

## Goal
- **Admin** (`heng.xiao.hour@gmail.com`, signed in with Google) → full control in both Cloud and Local modes.
- **Guests** (not signed-in / non-admin Google) → read-only dashboard in Cloud and Local.
- **Demo** → unaffected (no gating).

## Identities
| Party | Firebase token email | Role |
|---|---|---|
| Admin (browser) | `heng.xiao.hour@gmail.com` | isAdmin() — push commands, read/clear console |
| Device (ESP32, service-account JWT) | `firebase-adminsdk-fbsvc@esp32-electricity-counter.iam.gserviceaccount.com` | isDevice() — write /latest, read+delete /commands, write /console responses |
| Guest (anonymous browser) | auth == null | IsAdmin()=false, isDevice()=false → read-only |

## Files to Modify
- `database.rules.json` — server-enforced isAdmin/isDevice gates (the real security boundary)
- `frontend/config.example.js` + `frontend/config.js` — add `apiKey`, `authDomain`, `adminEmails`
- `frontend/index.html` — add `firebase-auth-compat.js`; auth button + account row in sidebar & Settings
- `frontend/script.js` — Google sign-in/out, onAuthStateChanged, `isAdmin()` helper, gate `sendCommand()` per mode, hide admin controls for guests, guest badge
- `frontend/style.css` — admin role badge + `body.role-guest` rules (hide Edit/Reset/Calibration/Console/ntfy)
- `README.md` — auth setup + rules deploy steps

## database.rules.json — new rules
```js
function isAdmin()  { return auth != null && auth.token.email == 'heng.xiao.hour@gmail.com'; }
function isDevice() { return auth != null && auth.token.email == 'firebase-adminsdk-fbsvc@esp32-electricity-counter.iam.gserviceaccount.com'; }
```
- `/devices/.read: true` (board picker lists boards; guest data in `latest` stays public, sensitive children pruned by child rules)
- `$device/latest`  → read: true, write: isDevice() || isAdmin()
- `$device/commands` → read: isAdmin() || isDevice(); `$cmd` write: isAdmin() || isDevice()
- `$device/console` → read: isAdmin(); `$out` write: isAdmin() || isDevice()
- `$device/ota`, `$device/firmware` → read: true, write: isDevice() || isAdmin()

## Frontend gating
- `isAdmin()` on client = signed-in user email ∈ `FB_CONFIG.adminEmails` (mirrors rules; server still enforces).
- `sendCommand()`: rejects with toast if `!isAdmin` and mode is `cloud` or `local`. Demo always allowed.
- `connectLocal` / `connectCloud`: guests may still connect to VIEW; only command send is blocked.
- UI: `body` gets `role-admin` / `role-guest`. Guest hides `.admin-only` (Edit buttons, Reset Counter, Calibration panel, Reset NVS, ntfy panel, Console panel+input).
- Sidebar + Settings: "Sign in with Google" (admin → shows email + Sign out of Google). Existing "Sign Out" (connection) stays as-is.

## External prerequisites (blocking — needs user)
1. Firebase Console → Authentication → Sign-in method → **Google**: enable. Authorized domains: `esp32-electricity-counter.web.app` (auto for Firebase Hosting) + `localhost` for testing.
2. Firebase Console → Project Settings → Your apps → Web app: **apiKey** value → goes into `config.js`. `authDomain` = `esp32-electricity-counter.firebaseapp.com` (derivable, no input needed).
3. Deploy: `firebase deploy --only database:rules,hosting`

## Verification
- `node --check frontend/script.js`
- Anonymous (Playwright, no auth) → guest view: open dashboard in cloud, channels render, no Edit/Reset/Cal buttons, sendCommand blocked with toast on WS + console shows rule denial on /commands push.
- Sign-in via Google popup in test browser → admin view appears, commands flow.
- REST: anonymous POST `/devices/<id>/commands` → permission_denied; `/latest` GET → public.
- Local (WS) guest: UI hides admin controls; `sendCommand` blocked client-side before wire.
- Demo mode: all controls enabled regardless of auth.