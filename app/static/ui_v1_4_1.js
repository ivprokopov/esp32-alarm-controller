(() => {
  'use strict';

  if (window.__prokopovUi141Loaded) return;
  window.__prokopovUi141Loaded = true;

  let pollBusy = false;
  let lastGoodStatusAt = 0;
  let lastWsStatusAt = 0;
  let fallbackTimer = null;
  let recoveryWs = null;
  let recoveryRetryTimer = null;

  function renderStatusPayload(payload) {
    if (!payload || typeof payload !== 'object') return;
    window.STATUS = payload.controller || null;
    if (typeof window.renderStatus === 'function') {
      window.renderStatus(payload.error || null);
    }
    if (payload.controller) lastGoodStatusAt = Date.now();
  }

  async function fetchFreshStatus() {
    if (pollBusy) return;
    pollBusy = true;
    try {
      const r = await fetch('/api/status', {
        method: 'GET',
        credentials: 'same-origin',
        cache: 'no-store',
        headers: { 'Accept': 'application/json' }
      });
      if (r.status === 401) return;
      if (!r.ok) throw new Error(`status HTTP ${r.status}`);
      const data = await r.json();
      renderStatusPayload(data);
    } catch (e) {
      // Do not force OFFLINE on a single transient browser/network error.
      // The existing UI will show controller loss if the backend itself reports it.
    } finally {
      pollBusy = false;
    }
  }

  function fallbackIntervalMs() {
    return document.visibilityState === 'visible' ? 900 : 2500;
  }

  function scheduleFallback() {
    if (fallbackTimer) clearTimeout(fallbackTimer);
    fallbackTimer = setTimeout(async () => {
      const now = Date.now();
      // Always refresh periodically, but especially when WebSocket status is stale.
      if (!lastWsStatusAt || now - lastWsStatusAt > 1400 || now - lastGoodStatusAt > 1400) {
        await fetchFreshStatus();
      } else if (now - lastGoodStatusAt > 5000) {
        await fetchFreshStatus();
      }
      scheduleFallback();
    }, fallbackIntervalMs());
  }

  function openRecoveryWebSocket() {
    if (recoveryWs && (recoveryWs.readyState === WebSocket.OPEN || recoveryWs.readyState === WebSocket.CONNECTING)) return;
    if (recoveryRetryTimer) {
      clearTimeout(recoveryRetryTimer);
      recoveryRetryTimer = null;
    }

    try {
      const proto = location.protocol === 'https:' ? 'wss' : 'ws';
      const ws = new WebSocket(`${proto}://${location.host}/ws`);
      recoveryWs = ws;

      ws.onopen = () => {
        lastWsStatusAt = Date.now();
      };

      ws.onmessage = (ev) => {
        try {
          const msg = JSON.parse(ev.data);
          if (msg.type === 'status') {
            lastWsStatusAt = Date.now();
            renderStatusPayload({ controller: msg.data, error: msg.error || null });
          }
        } catch (_) {}
      };

      ws.onclose = () => {
        recoveryWs = null;
        recoveryRetryTimer = setTimeout(openRecoveryWebSocket, 1600);
      };

      ws.onerror = () => {
        try { ws.close(); } catch (_) {}
      };
    } catch (_) {
      recoveryWs = null;
      recoveryRetryTimer = setTimeout(openRecoveryWebSocket, 1600);
    }
  }

  function hookExistingWebSocketStatus() {
    // Existing page WebSocket can keep working. This patch adds a second,
    // self-healing channel only as a resilience layer.
    const originalRender = window.renderStatus;
    if (typeof originalRender === 'function' && !window.__prokopovUi141RenderHooked) {
      window.__prokopovUi141RenderHooked = true;
      window.renderStatus = function (...args) {
        const result = originalRender.apply(this, args);
        if (window.STATUS) lastGoodStatusAt = Date.now();
        return result;
      };
    }
  }

  function boot() {
    hookExistingWebSocketStatus();
    fetchFreshStatus();
    scheduleFallback();
    openRecoveryWebSocket();

    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible') {
        fetchFreshStatus();
        openRecoveryWebSocket();
      }
      scheduleFallback();
    });

    window.addEventListener('online', () => {
      fetchFreshStatus();
      openRecoveryWebSocket();
    });

    window.addEventListener('focus', () => fetchFreshStatus());
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot, { once: true });
  } else {
    boot();
  }
})();
