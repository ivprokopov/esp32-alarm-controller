(() => {
  'use strict';

  if (window.__prokopovUi132Loaded) return;
  window.__prokopovUi132Loaded = true;

  function syncReadinessVisibility() {
    const state = (document.getElementById('alarmState')?.textContent || '').trim().toUpperCase();
    const line = document.querySelector('#dashboard .readyline');
    if (!line) return;

    // READY / NOT READY is meaningful only while the system is actually DISARMED.
    // Do not show it during armed, delay, alarm or panic states.
    line.classList.toggle('ui-readiness-hidden', state !== 'DISARMED');
  }

  function boot() {
    syncReadinessVisibility();

    const alarmState = document.getElementById('alarmState');
    if (alarmState && 'MutationObserver' in window) {
      new MutationObserver(syncReadinessVisibility).observe(alarmState, {
        childList: true,
        characterData: true,
        subtree: true,
      });
    }
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot, { once: true });
  } else {
    boot();
  }
})();
