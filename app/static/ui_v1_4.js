(() => {
  'use strict';

  const byId = (id) => document.getElementById(id);
  let doorOptimistic = null;

  function doorButtons() {
    const buttons = Array.from(document.querySelectorAll('#dashboard button'));
    return {
      lock: buttons.find((b) => (b.getAttribute('onclick') || '').includes("cmd('lock_door')")),
      unlock: buttons.find((b) => (b.getAttribute('onclick') || '').includes("cmd('unlock_door')")),
    };
  }

  function paintDoor(target, pending = false) {
    const state = byId('doorState');
    const { lock, unlock } = doorButtons();
    const locked = target === 'LOCKED';
    const unlocked = target === 'UNLOCKED';

    if (state) {
      if ((state.textContent || '').trim().toUpperCase() !== target) {
        state.textContent = target;
      }
      state.classList.toggle('door-locked-state', locked);
      state.classList.toggle('door-unlocked-state', unlocked);
    }

    if (lock) {
      lock.classList.add('door-control');
      lock.classList.toggle('door-active-lock', locked);
      lock.classList.remove('door-active-unlock');
      lock.classList.toggle('door-command-pending', pending && locked);
      lock.setAttribute('aria-pressed', locked ? 'true' : 'false');
    }
    if (unlock) {
      unlock.classList.add('door-control');
      unlock.classList.toggle('door-active-unlock', unlocked);
      unlock.classList.remove('door-active-lock');
      unlock.classList.toggle('door-command-pending', pending && unlocked);
      unlock.setAttribute('aria-pressed', unlocked ? 'true' : 'false');
    }
  }

  function makePanicClear() {
    const panic = byId('panicUnified');
    if (!panic) return;
    panic.setAttribute('title', 'PANIC — 1 tap: silent alarm · 2 taps: siren alarm');
    panic.setAttribute('aria-label', 'PANIC. Single tap activates silent alarm. Double tap activates siren alarm.');
    const strong = panic.querySelector('.ui-btn-copy strong');
    const small = panic.querySelector('.ui-btn-copy small');
    const idle = !panic.classList.contains('pending-single') && !panic.classList.contains('firing-audible');
    if (idle && strong && strong.textContent !== 'PANIC') strong.textContent = 'PANIC';
    if (idle && small && small.textContent !== '1× SILENT · 2× SIREN') small.textContent = '1× SILENT · 2× SIREN';
  }

  function installFastDoorCommand() {
    if (window.__prokopovUi14CmdHooked || typeof window.cmd !== 'function') return;
    window.__prokopovUi14CmdHooked = true;
    const originalCmd = window.cmd;

    window.cmd = async function(command) {
      let target = null;
      if (command === 'lock_door' || command === 'lock') target = 'LOCKED';
      if (command === 'unlock_door' || command === 'unlock') target = 'UNLOCKED';

      if (target) {
        doorOptimistic = { target, until: Date.now() + 1800 };
        paintDoor(target, true);
      }

      const result = await originalCmd.apply(this, arguments);

      if (target && typeof window.loadStatus === 'function') {
        setTimeout(() => window.loadStatus(), 40);
        setTimeout(() => window.loadStatus(), 180);
        setTimeout(() => window.loadStatus(), 500);
      }
      return result;
    };
  }

  function installRenderGuard() {
    if (window.__prokopovUi14RenderHooked || typeof window.renderStatus !== 'function') return;
    window.__prokopovUi14RenderHooked = true;
    const originalRender = window.renderStatus;

    window.renderStatus = function(...args) {
      const result = originalRender.apply(this, args);
      requestAnimationFrame(() => {
        makePanicClear();
        if (!doorOptimistic) return;
        const actual = (byId('doorState')?.textContent || '').trim().toUpperCase();
        if (actual === doorOptimistic.target) {
          doorOptimistic = null;
          const { lock, unlock } = doorButtons();
          lock?.classList.remove('door-command-pending');
          unlock?.classList.remove('door-command-pending');
          return;
        }
        if (Date.now() < doorOptimistic.until) {
          paintDoor(doorOptimistic.target, true);
        } else {
          doorOptimistic = null;
          const { lock, unlock } = doorButtons();
          lock?.classList.remove('door-command-pending');
          unlock?.classList.remove('door-command-pending');
        }
      });
      return result;
    };
  }

  function boot() {
    makePanicClear();
    installFastDoorCommand();
    installRenderGuard();

    const current = (byId('doorState')?.textContent || '').trim().toUpperCase();
    if (current === 'LOCKED' || current === 'UNLOCKED') paintDoor(current, false);

    // IMPORTANT: no MutationObserver here. The previous observer watched the
    // dashboard while makePanicClear() rewrote text nodes inside the dashboard,
    // creating a self-triggering mutation loop that could freeze Chromium and
    // leave the page stuck on OFFLINE. renderStatus already calls this function.
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot, { once: true });
  else boot();
})();
