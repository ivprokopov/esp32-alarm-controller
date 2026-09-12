(() => {
  'use strict';

  const ICONS = {
    shieldOff:'<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3 5 6v5c0 4.8 3 8.7 7 10 1.1-.4 2.2-.9 3.1-1.6M8.7 8.7A8.8 8.8 0 0 0 8 12c0 2.6 1.5 4.8 4 5.9M3 3l18 18"/></svg>',
    shield:'<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3 5 6v5c0 4.8 3 8.7 7 10 4-1.3 7-5.2 7-10V6l-7-3Z"/><path d="m9.2 12 1.8 1.8 3.9-4.1"/></svg>',
    home:'<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m3 11 9-8 9 8"/><path d="M5.5 9.5V21h13V9.5M9.5 21v-6h5v6"/></svg>',
    lock:'<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="5" y="10" width="14" height="11" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/></svg>',
    unlock:'<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="5" y="10" width="14" height="11" rx="2"/><path d="M8 10V7a4 4 0 0 1 7.4-2.1"/></svg>',
    bellOff:'<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M13.7 20H6a2 2 0 0 1-2-2v-1h2V11a6 6 0 0 1 .7-2.8M9.8 5.4A6 6 0 0 1 18 11v6h2v1c0 .5-.2 1-.5 1.4M10 21h4M3 3l18 18"/></svg>',
    alert:'<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M10.3 3.7 2.8 17a2 2 0 0 0 1.7 3h15a2 2 0 0 0 1.7-3L13.7 3.7a2 2 0 0 0-3.4 0Z"/><path d="M12 9v4M12 17h.01"/></svg>',
    silent:'<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M11 5 6 9H3v6h3l5 4V5ZM15.5 9.5l5 5M20.5 9.5l-5 5"/></svg>',
    settings:'<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.9l.1.1-2.8 2.8-.1-.1a1.7 1.7 0 0 0-1.9-.3 1.7 1.7 0 0 0-1 1.6V21h-4v-.1a1.7 1.7 0 0 0-1-1.6 1.7 1.7 0 0 0-1.9.3l-.1.1L4.2 17l.1-.1A1.7 1.7 0 0 0 4.6 15 1.7 1.7 0 0 0 3 14H3v-4h.1a1.7 1.7 0 0 0 1.6-1 1.7 1.7 0 0 0-.3-1.9L4.2 7 7 4.2l.1.1A1.7 1.7 0 0 0 9 4.6 1.7 1.7 0 0 0 10 3V3h4v.1a1.7 1.7 0 0 0 1 1.6 1.7 1.7 0 0 0 1.9-.3l.1-.1L19.8 7l-.1.1a1.7 1.7 0 0 0-.3 1.9 1.7 1.7 0 0 0 1.6 1h.1v4H21a1.7 1.7 0 0 0-1.6 1Z"/></svg>',
    logout:'<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M10 5H5a2 2 0 0 0-2 2v10a2 2 0 0 0 2 2h5M14 8l4 4-4 4M18 12H8"/></svg>',
    chevron:'<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m7 9 5 5 5-5"/></svg>'
  };

  const byId = id => document.getElementById(id);
  const iconButton = (icon,label,detail='') => `<span class="ui-icon">${ICONS[icon]||''}</span><span class="ui-btn-copy"><strong>${label}</strong>${detail?`<small>${detail}</small>`:''}</span>`;

  function closePopovers(except=null){
    document.querySelectorAll('.ui-popover.show').forEach(p=>{if(p!==except)p.classList.remove('show')});
    document.querySelectorAll('.userbox.open').forEach(x=>x.classList.remove('open'));
  }

  function setupTopbar(){
    const actions=document.querySelector('.top-actions');
    const user=document.querySelector('.userbox');
    if(!actions||!user||actions.dataset.ui12==='1')return;
    actions.dataset.ui12='1';

    const chevron=document.createElement('span');
    chevron.className='ui-chevron'; chevron.innerHTML=ICONS.chevron;
    user.appendChild(chevron); user.setAttribute('role','button'); user.setAttribute('tabindex','0');

    const alert=document.createElement('button');
    alert.type='button'; alert.id='uiAlertBtn'; alert.className='ui-alert-btn ui-hidden';
    alert.innerHTML=`${ICONS.alert}<span id="uiAlertCount" class="ui-alert-count">0</span>`;
    actions.insertBefore(alert,user);

    const issuePop=document.createElement('div'); issuePop.id='uiIssuePopover'; issuePop.className='ui-popover';
    issuePop.innerHTML='<div class="ui-popover-title">SYSTEM ATTENTION</div><div id="uiIssueList"></div>';
    actions.appendChild(issuePop);

    const userPop=document.createElement('div'); userPop.id='uiUserPopover'; userPop.className='ui-popover';
    userPop.innerHTML=`<div class="ui-popover-title">ACCOUNT</div><button id="uiMenuSettings" class="ui-menu-btn" type="button">${ICONS.settings}<span>Settings</span></button><button id="uiMenuLogout" class="ui-menu-btn" type="button">${ICONS.logout}<span>Logout</span></button>`;
    actions.appendChild(userPop);

    const toggleUser=()=>{const show=!userPop.classList.contains('show');closePopovers();userPop.classList.toggle('show',show);user.classList.toggle('open',show)};
    user.addEventListener('click',e=>{e.stopPropagation();toggleUser()});
    user.addEventListener('keydown',e=>{if(e.key==='Enter'||e.key===' '){e.preventDefault();toggleUser()}});
    alert.addEventListener('click',e=>{e.stopPropagation();const show=!issuePop.classList.contains('show');closePopovers();issuePop.classList.toggle('show',show)});
    byId('uiMenuSettings').onclick=()=>{closePopovers();if(typeof window.toggleSettings==='function')window.toggleSettings()};
    byId('uiMenuLogout').onclick=()=>{closePopovers();if(typeof window.logout==='function')window.logout()};
    document.addEventListener('click',()=>closePopovers());
  }

  function collectIssues(){
    const out=[];
    document.querySelectorAll('.top-actions .chip').forEach(ch=>{
      const t=(ch.textContent||'').trim(); const u=t.toUpperCase();
      if(u.includes('OFFLINE')||u.includes('ERROR')) out.push({title:t,detail:u.includes('I4')?'Remote input module is not reachable.':'System component requires attention.'});
    });
    const ready=byId('readyPill'); const readyText=byId('readyText');
    if(ready&&(ready.textContent||'').toUpperCase().includes('NOT READY')){
      const reason=(readyText?.textContent||'').trim();
      if(reason) out.push({title:'NOT READY',detail:reason});
    }
    const uniq=[]; const seen=new Set();
    out.forEach(x=>{const k=x.title+'|'+x.detail;if(!seen.has(k)){seen.add(k);uniq.push(x)}});
    return uniq;
  }

  function refreshTopbar(){
    const alert=byId('uiAlertBtn'), count=byId('uiAlertCount'), list=byId('uiIssueList');
    if(!alert||!count||!list)return;
    const issues=collectIssues();
    alert.classList.toggle('ui-hidden',issues.length===0); count.textContent=String(issues.length);
    list.innerHTML=issues.length?issues.map(x=>`<div class="ui-issue"><span class="ui-issue-dot"></span><div><strong>${escapeHtml(x.title)}</strong><small>${escapeHtml(x.detail)}</small></div></div>`).join(''):'<div class="ui-issue"><span class="ui-issue-dot" style="background:#22c55e"></span><div><strong>No active problems</strong></div></div>';
    const settings=byId('settingsBtn'), menuSettings=byId('uiMenuSettings');
    if(menuSettings) menuSettings.classList.toggle('ui-hidden',!!settings?.classList.contains('hidden'));
  }

  function escapeHtml(s){return String(s??'').replace(/[&<>"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]))}

  function setupModeButtons(){
    const grid=document.querySelector('#dashboard .armgrid'); if(!grid||grid.dataset.ui12==='1')return;
    grid.dataset.ui12='1'; grid.classList.add('mode-grid');
    grid.innerHTML=`
      <button id="modeDisarm" class="btn mode-btn mode-disarm" type="button" onclick="prokopovMode('disarm','disarm')">${iconButton('shieldOff','DISARM')}</button>
      <button id="modeAway" class="btn mode-btn mode-away" type="button" onclick="prokopovMode('arm_away','away')">${iconButton('shield','ARM AWAY')}</button>
      <button id="modeHome" class="btn mode-btn mode-home" type="button" onclick="prokopovMode('arm_home','home')">${iconButton('home','ARM HOME')}</button>`;
    const legacy=document.querySelector('#dashboard button[onclick*="cmd(\'disarm\')"]'); if(legacy)legacy.remove();
  }

  function decorateSystemActions(){
    const lock=document.querySelector('#dashboard button[onclick*="lock_door"]');
    const unlock=document.querySelector('#dashboard button[onclick*="unlock_door"]');
    const siren=document.querySelector('#dashboard button[onclick*="siren_off"]');
    const row=lock?.parentElement||unlock?.parentElement||siren?.parentElement;
    if(row)row.classList.add('system-actions');
    if(lock&&!lock.dataset.ui12){lock.dataset.ui12='1';lock.classList.add('icon-btn');lock.innerHTML=iconButton('lock','LOCK DOOR')}
    if(unlock&&!unlock.dataset.ui12){unlock.dataset.ui12='1';unlock.classList.add('icon-btn');unlock.innerHTML=iconButton('unlock','UNLOCK DOOR')}
    if(siren&&!siren.dataset.ui12){siren.dataset.ui12='1';siren.classList.add('icon-btn');siren.innerHTML=iconButton('bellOff','SIREN OFF')}
  }

  function setupPanic(){
    if(byId('panicQuick'))return; const head=document.querySelector('#dashboard .hero-head'); if(!head)return;
    const wrap=document.createElement('div'); wrap.id='panicQuick'; wrap.className='hero-quick-actions';
    const summary=byId('zoneSummary'); if(summary)wrap.appendChild(summary);
    const group=document.createElement('div'); group.className='panic-quick-group';
    group.innerHTML=`<button id="panicAudible" class="quick-panic" type="button" onclick="prokopovPanic(this,false)">${iconButton('alert','PANIC','DOUBLE TAP')}</button><button id="panicSilent" class="quick-panic silent" type="button" onclick="prokopovPanic(this,true)">${iconButton('silent','SILENT','DOUBLE TAP')}</button>`;
    wrap.appendChild(group); head.appendChild(wrap);
    const old=document.querySelector('#dashboard .panic-area'); if(old)old.classList.add('legacy-panic-hidden');
  }

  function refreshModeState(){
    const state=(byId('alarmState')?.textContent||'').trim().toUpperCase();
    let active=null; if(state==='DISARMED')active='disarm'; else if(state.includes('AWAY'))active='away'; else if(state.includes('HOME'))active='home';
    [['disarm','modeDisarm'],['away','modeAway'],['home','modeHome']].forEach(([k,id])=>{
      const el=byId(id); if(!el)return; const on=k===active; el.classList.toggle('active',on); el.classList.remove('pending');
      let badge=el.querySelector('.mode-active-label'); if(!badge){badge=document.createElement('span');badge.className='mode-active-label';el.appendChild(badge)} badge.textContent=on?'ACTIVE':'';
    });
  }

  function normalizeZoneList(){
    const list=byId('zoneList'); if(!list)return; list.classList.add('zone-list-compact');
    list.querySelectorAll('.zone').forEach(z=>{const s=z.querySelector('.zstate');const t=(s?.textContent||'').toUpperCase();z.classList.toggle('zone-offline',t.includes('OFFLINE'));z.classList.toggle('zone-active',t.includes('ACTIVE'))});
  }

  function refresh(){refreshModeState();normalizeZoneList();refreshTopbar()}

  window.prokopovMode=(command,mode)=>{document.querySelectorAll('.mode-btn').forEach(b=>b.classList.remove('pending'));const el=byId(mode==='disarm'?'modeDisarm':mode==='away'?'modeAway':'modeHome');if(el)el.classList.add('pending');if(typeof window.cmd==='function')window.cmd(command)};

  const panicState={audible:0,silent:0};
  window.prokopovPanic=(button,silent)=>{
    const key=silent?'silent':'audible',now=Date.now();
    if(now-panicState[key]<1800){panicState[key]=0;button.classList.remove('confirming');restorePanicLabel(button,silent);if(navigator.vibrate)navigator.vibrate([70,50,80]);if(typeof window.cmd==='function')window.cmd(silent?'silent_panic':'panic');return}
    panicState[key]=now;button.classList.add('confirming');const strong=button.querySelector('strong'),small=button.querySelector('small');if(strong)strong.textContent='TAP AGAIN';if(small)small.textContent=silent?'SILENT PANIC':'AUDIBLE PANIC';if(navigator.vibrate)navigator.vibrate(35);
    setTimeout(()=>{if(Date.now()-panicState[key]>=1750){panicState[key]=0;button.classList.remove('confirming');restorePanicLabel(button,silent)}},1850);
  };
  function restorePanicLabel(button,silent){const strong=button.querySelector('strong'),small=button.querySelector('small');if(strong)strong.textContent=silent?'SILENT':'PANIC';if(small)small.textContent='DOUBLE TAP'}

  function installHook(){
    if(window.__prokopovUi12Hooked)return; window.__prokopovUi12Hooked=true;
    const original=window.renderStatus;
    if(typeof original==='function') window.renderStatus=function(...args){const r=original.apply(this,args);requestAnimationFrame(refresh);return r};
  }

  function boot(){setupTopbar();setupModeButtons();decorateSystemActions();setupPanic();installHook();refresh();
    const zl=byId('zoneList');if(zl&&window.MutationObserver)new MutationObserver(()=>requestAnimationFrame(refresh)).observe(zl,{childList:true,subtree:true,characterData:true});
    setInterval(refreshTopbar,1500);
  }

  if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',boot,{once:true});else boot();
})();
