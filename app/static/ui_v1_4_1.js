(() => {
  'use strict';

  if (window.__prokopovUi141Loaded) return;
  window.__prokopovUi141Loaded = true;

  let busy = false;
  let timer = null;
  let recoveryWs = null;
  let retryTimer = null;

  async function refreshStatus() {
    if (busy) return;
    busy = true;

    try {
      if (typeof window.loadStatus === 'function') {
        await window.loadStatus();
      }
    } catch (_) {
      // Запазваме последното валидно състояние при временна мрежова грешка.
    } finally {
      busy = false;
    }
  }

  function schedule() {
    if (timer) clearTimeout(timer);

    timer = setTimeout(async () => {
      await refreshStatus();
      schedule();
    }, document.visibilityState === 'visible' ? 850 : 2500);
  }

  function connectRecoveryWs() {
    if (
      recoveryWs &&
      (
        recoveryWs.readyState === WebSocket.OPEN ||
        recoveryWs.readyState === WebSocket.CONNECTING
      )
    ) return;

    try {
      const proto = location.protocol === 'https:' ? 'wss' : 'ws';
      recoveryWs = new WebSocket(`${proto}://${location.host}/ws`);

      recoveryWs.onmessage = (event) => {
        try {
          const msg = JSON.parse(event.data);
          if (msg.type === 'status') {
            refreshStatus();
          }
        } catch (_) {}
      };

      recoveryWs.onclose = () => {
        recoveryWs = null;
        clearTimeout(retryTimer);
        retryTimer = setTimeout(connectRecoveryWs, 1500);
      };

      recoveryWs.onerror = () => {
        try {
          recoveryWs.close();
        } catch (_) {}
      };
    } catch (_) {
      recoveryWs = null;
      clearTimeout(retryTimer);
      retryTimer = setTimeout(connectRecoveryWs, 1500);
    }
  }

  function boot() {
    refreshStatus();
    schedule();
    connectRecoveryWs();

    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible') {
        refreshStatus();
        connectRecoveryWs();
      }
      schedule();
    });

    window.addEventListener('focus', refreshStatus);
    window.addEventListener('online', () => {
      refreshStatus();
      connectRecoveryWs();
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot, { once: true });
  } else {
    boot();
  }
})();
