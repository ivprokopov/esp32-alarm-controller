(() => {
  'use strict';

  const ICONS = {
    shieldOff: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3 5 6v5c0 4.8 3 8.7 7 10 1.1-.4 2.2-.9 3.1-1.6M8.7 8.7A8.8 8.8 0 0 0 8 12c0 2.6 1.5 4.8 4 5.9M3 3l18 18"/></svg>',
    shield: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3 5 6v5c0 4.8 3 8.7 7 10 4-1.3 7-5.2 7-10V6l-7-3Z"/><path d="m9.2 12 1.8 1.8 3.9-4.1"/></svg>',
    home: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m3 11 9-8 9 8"/><path d="M5.5 9.5V21h13V9.5M9.5 21v-6h5v6"/></svg>',
    lock: '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="5" y="10" width="14" height="11" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/></svg>',
    unlock: '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="5" y="10" width="14" height="11" rx="2"/><path d="M8 10V7a4 4 0 0 1 7.4-2.1"/></svg>',
    bellOff: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M13.7 20H6a2 2 0 0 1-2-2v-1h2V11a6 6 0 0 1 .7-2.8M9.8 5.4A6 6 0 0 1 18 11v6h2v1c0 .5-.2 1-.5 1.4M10 21h4M3 3l18 18"/></svg>',
    alert: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M10.3 3.7 2.8 17a2 2 0 0 0 1.7 3h15a2 2 0 0 0 1.7-3L13.7 3.7a2 2 0 0 0-3.4 0Z"/><path d="M12 9v4M12 17h.01"/></svg>',
    silent: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M11 5 6 9H3v6h3l5 4V5ZM15.5 9.5l5 5M20.5 9.5l-5 5"/></svg>'
  };

  const byId = (id) => document.getElementById(id);
  const buttonHTML = (icon, label, detail = '') =>
    `<span class="ui-icon">${ICONS[icon] || ''}</span><span class="ui-btn-copy"><strong>${label}</strong>${detail ? `<small>${detail}</small>` : ''}</span>`;

  function setupModeButtons() {
    const grid = document.querySelector('#dashboard .armgrid');
    if (!grid || grid.dataset.ui11 === '1') return;
    grid.dataset.ui11 = '1';
    grid.classList.add('mode-grid');
    grid.innerHTML = `
      <button id="modeDisarm" class="btn mode-btn mode-disarm" type="button" onclick="prokopovMode('disarm','disarm')" aria-pressed="false">
        ${buttonHTML('shieldOff', 'DISARM')}
      </button>
      <button id="modeAway" class="btn mode-btn mode-away" type="button" onclick="prokopovMode('arm_away','away')" aria-pressed="false">
        ${buttonHTML('shield', 'ARM AWAY')}
      </button>
      <button id="modeHome" class="btn mode-btn mode-home" type="button" onclick="prokopovMode('arm_home','home')" aria-pressed="false">
        ${buttonHTML('home', 'ARM HOME')}
      </button>`;

    const disarmLegacy = document.querySelector('#dashboard button[onclick*="cmd(\'disarm\')"]');
    if (disarmLegacy) disarmLegacy.remove();
  }

  function decorateSystemActions() {
    const lock = document.querySelector('#dashboard button[onclick*="lock_door"]');
    const unlock = document.querySelector('#dashboard button[onclick*="unlock_door"]');
    const siren = document.querySelector('#dashboard button[onclick*="siren_off"]');
    const row = lock?.parentElement || unlock?.parentElement || siren?.parentElement;
    if (row) row.classList.add('system-actions');
    if (lock) { lock.classList.add('icon-btn'); lock.innerHTML = buttonHTML('lock', 'LOCK DOOR'); }
    if (unlock) { unlock.classList.add('icon-btn'); unlock.innerHTML = buttonHTML('unlock', 'UNLOCK DOOR'); }
    if (siren) { siren.classList.add('icon-btn'); siren.innerHTML = buttonHTML('bellOff', 'SIREN OFF'); }
  }

  function setupPanic() {
    if (byId('panicQuick')) return;
    const heroHead = document.querySelector('#dashboard .hero-head');
    if (!heroHead) return;

    const wrap = document.createElement('div');
    wrap.className = 'hero-quick-actions';
    wrap.id = 'panicQuick';

    const zoneSummary = byId('zoneSummary');
    if (zoneSummary) wrap.appendChild(zoneSummary);

    const panicGroup = document.createElement('div');
    panicGroup.className = 'panic-quick-group';
    panicGroup.innerHTML = `
      <button id="panicAudible" class="quick-panic audible" type="button" onclick="prokopovPanic(this,false)" aria-label="Audible panic, double tap to activate">
        ${buttonHTML('alert', 'PANIC', 'DOUBLE TAP')}
      </button>
      <button id="panicSilent" class="quick-panic silent" type="button" onclick="prokopovPanic(this,true)" aria-label="Silent panic, double tap to activate">
        ${buttonHTML('silent', 'SILENT', 'DOUBLE TAP')}
      </button>`;
    wrap.appendChild(panicGroup);
    heroHead.appendChild(wrap);

    const old = document.querySelector('#dashboard .panic-area');
    if (old) old.classList.add('legacy-panic-hidden');
  }

  function refreshModeState() {
    const state = String(window.STATUS?.alarm_state || '').toUpperCase();
    let active = null;
    if (state === 'DISARMED') active = 'disarm';
    else if (state.includes('AWAY')) active = 'away';
    else if (state.includes('HOME')) active = 'home';

    const map = {
      disarm: byId('modeDisarm'),
      away: byId('modeAway'),
      home: byId('modeHome')
    };

    Object.entries(map).forEach(([key, el]) => {
      if (!el) return;
      const on = key === active;
      el.classList.toggle('active', on);
      el.classList.remove('pending');
      el.setAttribute('aria-pressed', on ? 'true' : 'false');
      let badge = el.querySelector('.mode-active-label');
      if (!badge) {
        badge = document.createElement('span');
        badge.className = 'mode-active-label';
        el.appendChild(badge);
      }
      badge.textContent = on ? 'ACTIVE' : '';
    });
  }

  function normalizeZoneList() {
    const zoneList = byId('zoneList');
    if (!zoneList) return;
    zoneList.classList.add('zone-list-compact');
    zoneList.querySelectorAll('.zone').forEach((zone) => {
      const state = zone.querySelector('.zstate');
      const text = (state?.textContent || '').toUpperCase();
      zone.classList.toggle('zone-offline', text.includes('OFFLINE'));
      zone.classList.toggle('zone-active', text.includes('ACTIVE'));
      if (state) state.setAttribute('title', state.textContent.trim());
    });
  }

  function refresh() {
    refreshModeState();
    normalizeZoneList();
  }

  window.prokopovMode = (command, mode) => {
    const el = mode === 'disarm' ? byId('modeDisarm') : mode === 'away' ? byId('modeAway') : byId('modeHome');
    document.querySelectorAll('.mode-btn').forEach((b) => b.classList.remove('pending'));
    if (el) el.classList.add('pending');
    if (typeof window.cmd === 'function') window.cmd(command);
  };

  const panicState = { audible: 0, silent: 0 };
  window.prokopovPanic = (button, silent) => {
    const key = silent ? 'silent' : 'audible';
    const now = Date.now();
    const armed = now - panicState[key] < 1800;

    if (armed) {
      panicState[key] = 0;
      button.classList.remove('confirming');
      const label = button.querySelector('.ui-btn-copy strong');
      const detail = button.querySelector('.ui-btn-copy small');
      if (label) label.textContent = silent ? 'SILENT' : 'PANIC';
      if (detail) detail.textContent = 'DOUBLE TAP';
      if (navigator.vibrate) navigator.vibrate(silent ? [60, 50, 60] : [80, 60, 100]);
      if (typeof window.cmd === 'function') window.cmd(silent ? 'silent_panic' : 'panic');
      return;
    }

    panicState[key] = now;
    button.classList.add('confirming');
    const label = button.querySelector('.ui-btn-copy strong');
    const detail = button.querySelector('.ui-btn-copy small');
    if (label) label.textContent = 'TAP AGAIN';
    if (detail) detail.textContent = silent ? 'SILENT PANIC' : 'AUDIBLE PANIC';
    if (navigator.vibrate) navigator.vibrate(35);

    window.setTimeout(() => {
      if (Date.now() - panicState[key] >= 1750) {
        panicState[key] = 0;
        button.classList.remove('confirming');
        if (label) label.textContent = silent ? 'SILENT' : 'PANIC';
        if (detail) detail.textContent = 'DOUBLE TAP';
      }
    }, 1850);
  };

  function installRenderHook() {
    if (window.__prokopovUi11Hooked) return;
    window.__prokopovUi11Hooked = true;
    const original = window.renderStatus;
    if (typeof original === 'function') {
      window.renderStatus = function (...args) {
        const result = original.apply(this, args);
        window.requestAnimationFrame(refresh);
        return result;
      };
    }
  }

  function boot() {
    setupModeButtons();
    decorateSystemActions();
    setupPanic();
    installRenderHook();
    refresh();

    const zoneList = byId('zoneList');
    if (zoneList && 'MutationObserver' in window) {
      new MutationObserver(normalizeZoneList).observe(zoneList, { childList: true, subtree: true, characterData: true });
    }
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot, { once: true });
  else boot();
})();
