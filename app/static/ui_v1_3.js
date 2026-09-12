(() => {
  'use strict';

  const byId = (id) => document.getElementById(id);
  const ICON = {
    logo: '<svg viewBox="0 0 32 32" aria-hidden="true"><path d="M16 3 6.5 7v7c0 6.5 4 11.5 9.5 14 5.5-2.5 9.5-7.5 9.5-14V7L16 3Z"/><path d="m10.5 15 5.5-4.8 5.5 4.8"/><path d="M12.5 13.4V21h7v-7.6"/><path d="M15 21v-4h2v4"/></svg>',
    panic: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M10.3 3.7 2.8 17a2 2 0 0 0 1.7 3h15a2 2 0 0 0 1.7-3L13.7 3.7a2 2 0 0 0-3.4 0Z"/><path d="M12 9v4M12 17h.01"/></svg>'
  };

  function installBrand() {
    const brand = document.querySelector('.brand');
    if (!brand || brand.dataset.ui13 === '1') return;
    brand.dataset.ui13 = '1';
    brand.innerHTML = `
      <div class="brand-v13-logo" aria-hidden="true">${ICON.logo}</div>
      <div class="brand-v13-copy" aria-label="Prokopov Home Alarm System">
        <span class="brand-prokopov">PROKOPOV</span>
        <span class="brand-home">HOME ALARM</span>
        <span class="brand-system">SYSTEM</span>
      </div>`;
  }

  function ensureUnifiedPanic() {
    const quick = byId('panicQuick');
    if (!quick) return;

    let group = quick.querySelector('.panic-quick-group');
    if (!group) {
      group = document.createElement('div');
      group.className = 'panic-quick-group';
      quick.appendChild(group);
    }

    const oldAudible = byId('panicAudible');
    const oldSilent = byId('panicSilent');
    if (oldAudible) oldAudible.remove();
    if (oldSilent) oldSilent.remove();

    if (byId('panicUnified')) return;

    const button = document.createElement('button');
    button.id = 'panicUnified';
    button.type = 'button';
    button.setAttribute('aria-label', 'Panic: single tap silent alarm, double tap siren alarm');
    button.setAttribute('title', '1 tap = silent alarm · 2 taps = siren alarm');
    button.innerHTML = `
      <span class="ui-icon">${ICON.panic}</span>
      <span class="ui-btn-copy">
        <strong>PANIC</strong>
        <small>1× SILENT · 2× SIREN</small>
      </span>`;
    group.appendChild(button);

    let clickTimer = null;
    let lastClick = 0;
    const resetVisual = () => {
      button.classList.remove('pending-single', 'firing-audible');
      const strong = button.querySelector('strong');
      const small = button.querySelector('small');
      if (strong) strong.textContent = 'PANIC';
      if (small) small.textContent = '1× SILENT · 2× SIREN';
    };

    button.addEventListener('click', () => {
      const now = Date.now();
      const isDouble = lastClick && (now - lastClick <= 360);

      if (isDouble) {
        lastClick = 0;
        if (clickTimer) {
          clearTimeout(clickTimer);
          clickTimer = null;
        }
        button.classList.remove('pending-single');
        button.classList.add('firing-audible');
        const strong = button.querySelector('strong');
        const small = button.querySelector('small');
        if (strong) strong.textContent = 'SIREN PANIC';
        if (small) small.textContent = 'ACTIVATING';
        if (navigator.vibrate) navigator.vibrate([90, 50, 120]);
        if (typeof window.cmd === 'function') window.cmd('panic');
        setTimeout(resetVisual, 1100);
        return;
      }

      lastClick = now;
      button.classList.add('pending-single');
      const strong = button.querySelector('strong');
      const small = button.querySelector('small');
      if (strong) strong.textContent = 'SILENT?';
      if (small) small.textContent = 'TAP AGAIN FOR SIREN';
      if (navigator.vibrate) navigator.vibrate(30);

      clickTimer = setTimeout(() => {
        clickTimer = null;
        lastClick = 0;
        if (typeof window.cmd === 'function') window.cmd('silent_panic');
        resetVisual();
      }, 380);
    });
  }

  function getDoorButtons() {
    const buttons = Array.from(document.querySelectorAll('#dashboard button'));
    const lock = buttons.find((b) => (b.getAttribute('onclick') || '').includes("cmd('lock_door')"));
    const unlock = buttons.find((b) => (b.getAttribute('onclick') || '').includes("cmd('unlock_door')"));
    return { lock, unlock };
  }

  function refreshDoorControls() {
    const state = (byId('doorState')?.textContent || '').trim().toUpperCase();
    const { lock, unlock } = getDoorButtons();
    const locked = state === 'LOCKED';
    const unlocked = state === 'UNLOCKED';

    if (lock) {
      lock.classList.add('door-control');
      lock.classList.toggle('door-active-lock', locked);
      lock.classList.remove('door-active-unlock');
      lock.setAttribute('aria-pressed', locked ? 'true' : 'false');
    }
    if (unlock) {
      unlock.classList.add('door-control');
      unlock.classList.toggle('door-active-unlock', unlocked);
      unlock.classList.remove('door-active-lock');
      unlock.setAttribute('aria-pressed', unlocked ? 'true' : 'false');
    }

    const label = byId('doorState');
    if (label) {
      label.classList.toggle('door-locked-state', locked);
      label.classList.toggle('door-unlocked-state', unlocked);
    }
  }

  function tidyTopbar() {
    const alertBtn = byId('uiAlertBtn');
    const userbox = document.querySelector('.userbox');
    if (alertBtn) alertBtn.setAttribute('title', 'System problems');
    if (userbox) userbox.setAttribute('title', 'Account menu');
  }

  function installRenderHook() {
    if (window.__prokopovUi13Hooked) return;
    window.__prokopovUi13Hooked = true;
    const original = window.renderStatus;
    if (typeof original !== 'function') return;
    window.renderStatus = function (...args) {
      const result = original.apply(this, args);
      window.requestAnimationFrame(() => {
        refreshDoorControls();
        ensureUnifiedPanic();
      });
      return result;
    };
  }

  function boot() {
    installBrand();
    tidyTopbar();
    ensureUnifiedPanic();
    refreshDoorControls();
    installRenderHook();

    const dashboard = byId('dashboard');
    if (dashboard && 'MutationObserver' in window) {
      new MutationObserver(() => {
        ensureUnifiedPanic();
        refreshDoorControls();
      }).observe(dashboard, { childList: true, subtree: true, characterData: true });
    }
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot, { once: true });
  } else {
    boot();
  }
})();
