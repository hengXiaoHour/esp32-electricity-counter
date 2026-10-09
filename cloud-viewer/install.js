// ============ PWA install prompt (HOSTED SITE ONLY) ============
//
// This file is hand-written and cloud-only, like cloud.js. It is NOT served by
// the board: src/network/web_assets.h embeds frontend/ only, and frontend/ has
// no install UI at all.
//
// Why it is not on the board page: the board serves its dashboard over plain
// http on a LAN address (http://192.168.4.1/). That is NOT a secure context, so
// browsers refuse to register the service worker and never fire
// `beforeinstallprompt` - "Install app" is never offered from the address bar
// either. The button was therefore a dead control on the one page where it
// could never work, and clicking it could only produce a "no prompt available"
// shrug. The hosted dashboard (https://esp32-electricity-counter.web.app) is a
// secure context, so the prompt genuinely fires there and the UI is worth
// shipping.
//
// scripts/build_cloud_viewer.py injects the markup (#installRow / #installBtnTop
// / #installHint) into cloud-viewer/index.html and appends the matching CSS to
// its cloud-only block. Both are asserted by scripts/check_docs.py, so the
// module and its buttons cannot drift apart silently.

let deferredInstallPrompt = null;

function isStandalone() {
  return window.matchMedia && window.matchMedia('(display-mode: standalone)').matches;
}

function showInstallRow() {
  // Two places offer install: the connect panel (pre-connect) and the top
  // bar (always visible, mobile + desktop). Both go when installed.
  const row = document.getElementById('installRow');
  if (row) row.classList.remove('hidden');
  const top = document.getElementById('installBtnTop');
  if (top) top.classList.remove('hidden');
}

function hideInstallRow() {
  for (const id of ['installRow', 'installBtnTop']) {
    const row = document.getElementById(id);
    if (row) row.classList.add('hidden');
  }
  for (const id of ['installHint']) {
    const h = document.getElementById(id);
    if (h) h.classList.add('hidden');
  }
}

window.addEventListener('beforeinstallprompt', (e) => {
  e.preventDefault();
  deferredInstallPrompt = e;
  showInstallRow();
});

window.addEventListener('appinstalled', () => {
  deferredInstallPrompt = null;
  hideInstallRow();
  showToast('App installed — launch it from your home screen');
});

function promptInstall() {
  // Android / installable: use the browser's install prompt.
  if (deferredInstallPrompt) {
    deferredInstallPrompt.prompt();
    deferredInstallPrompt.userChoice.then((choice) => {
      if (choice.outcome === 'accepted') hideInstallRow();
      deferredInstallPrompt = null;
    });
    return;
  }
  // No prompt available. On the hosted site this means the browser has not
  // offered one yet (Safari, Firefox, or an already-installed app), so the
  // manual-steps hint is the honest answer.
  const h1 = document.getElementById('installHint');
  if (h1) h1.classList.toggle('hidden');
}

(function initInstallUi() {
  if (isStandalone()) hideInstallRow();
  else showInstallRow();
})();