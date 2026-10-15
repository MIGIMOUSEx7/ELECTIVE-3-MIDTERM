/* SafeHome — shared script, loaded by every page.
   Pages share their data through localStorage, so readings, the log, the alarm
   state and your contacts carry over when you move between pages. */
(() => {
  'use strict';

  // ==========================================================================
  // SETTINGS YOU MAY NEED TO EDIT
  // ==========================================================================

  // Where your Django backend runs. Use your PC's WiFi IP (e.g. http://192.168.0.10:8000)
  // if you open the app from a phone. 127.0.0.1 only works on the PC running Django.
  // Note: Android (Capacitor) blocks plain http:// unless cleartext is allowed in
  // capacitor.config (server.cleartext = true) or you serve the backend over https.
  const BACKEND = 'http://127.0.0.1:8000';

  // The gauge works on a "level" where 1000 = a sensor reached ITS OWN alarm point.
  const WARN = 600, DANGER = 1000, MAX = 2000, ARC_LEN = 251.3;

  // Fallback alarm points, used only if the backend does not send the device's own
  // thresholds. They match ALARM_MIN in the firmware.
  const ALARM_AT = { mq2: 100, mq7: 40, mq135: 100 };

  // Readings older than this are treated as "ESP32 offline" (needs a timestamp
  // field from the backend: created_at / timestamp / time / ts).
  const STALE_MS = 15000;

  // ==========================================================================

  const $ = id => document.getElementById(id);
  const txt = (id, t) => { const e = $(id); if (e) e.textContent = t; };
  const timeStr = () => new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });

  const store = {
    get(k, d) { try { const v = localStorage.getItem(k); return v ? JSON.parse(v) : d; } catch { return d; } },
    set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch {} }
  };

  const classify = v => v >= DANGER ? 'danger' : v >= WARN ? 'warn' : 'safe';
  const STATES = {
    safe:   { pill: '● Safe range', icon: '✅', title: 'All clear',       sub: 'No smoke or gas detected',    tag: 'safe' },
    warn:   { pill: '● Elevated',   icon: '⚠️', title: 'Elevated levels', sub: 'Ventilate and check the area', tag: 'warn' },
    danger: { pill: '● Danger',     icon: '🚨', title: 'Smoke detected!', sub: 'Inspect the area immediately', tag: 'danger' },
  };

  // ---- shared state (persisted) ------------------------------------------
  // 'readings3' so values from older scales are never mixed in
  let readings = store.get('readings3', []);
  let state    = store.get('state', 'safe');
  let muted    = store.get('muted', false);
  let forceSpike = false;

  // ---- sensor levels -----------------------------------------------------
  const SENSOR_KEYS = ['mq2', 'mq7', 'mq135'];

  // Turns each sensor's "rise above baseline" into "how close is it to its own alarm point".
  // In live mode every value is already a rise (the device subtracts its own baseline).
  function sensorLevel(k, v, live, thresh) {
    if (!live) return Math.max(0, v);                        // demo mode: numbers are already levels
    const limit = (thresh && thresh[k]) || ALARM_AT[k];
    return Math.max(0, v / limit * DANGER);
  }

  // The device decides alarms (LEDs, buzzer, Telegram). When it tells us the alarm
  // state, the app follows it so both always agree.
  function overallLevel(r, live) {
    const lvl = Math.round(Math.max(...SENSOR_KEYS.map(k => sensorLevel(k, r[k] ?? 0, live, r.thresh))));
    if (live && typeof r.alarm === 'boolean') {
      return r.alarm ? Math.max(lvl, DANGER) : Math.min(lvl, DANGER - 1);
    }
    return lvl;
  }

  // ---- activity log ------------------------------------------------------
  function logRow(e) {
    const el = document.createElement('div'); el.className = 'log-item';
    const tag = document.createElement('span'); tag.className = 'tag ' + e.tag;
    const t = document.createElement('time'); t.textContent = e.t;
    const m = document.createElement('span'); m.textContent = e.msg;
    el.append(tag, t, m);
    return el;
  }
  function addLog(msg, tag = 'info') {
    const e = { t: timeStr(), msg, tag };
    const log = store.get('log', []);
    log.unshift(e); log.length = Math.min(log.length, 60);
    store.set('log', log);
    if ($('log')) $('log').prepend(logRow(e));
  }
  if (!store.get('log', []).length) addLog('System initialized');
  if ($('log')) store.get('log', []).forEach(e => $('log').append(logRow(e)));

  // ---- toast -------------------------------------------------------------
  let toastTimer;
  function toast(msg) {
    const t = $('toast'); if (!t) return;
    t.textContent = msg; t.classList.add('show');
    clearTimeout(toastTimer); toastTimer = setTimeout(() => t.classList.remove('show'), 4500);
  }

  // ---- drawing -----------------------------------------------------------
  function drawSpark() {
    const box = $('spark'); if (!box) return;
    const w = 300, h = 70, n = 30, pts = readings.slice(-n);
    if (pts.length < 2) return;
    const step = w / (n - 1), off = n - pts.length;
    const xy = pts.map((v, i) => [(i + off) * step, h - (Math.min(v, MAX) / MAX) * (h - 6) - 3]);
    const line = xy.map(p => p.join(',')).join(' ');
    const dY = h - (DANGER / MAX) * (h - 6) - 3;
    box.innerHTML = `
      <defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0" stop-color="var(--state)" stop-opacity=".35"/><stop offset="1" stop-color="var(--state)" stop-opacity="0"/>
      </linearGradient></defs>
      <line x1="0" x2="${w}" y1="${dY}" y2="${dY}" stroke="var(--danger)" stroke-dasharray="4 4" opacity=".5"/>
      <polygon points="${xy[0][0]},${h} ${line} ${xy.at(-1)[0]},${h}" fill="url(#g)"/>
      <polyline points="${line}" fill="none" stroke="var(--state)" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>`;
  }

  function paint() {
    document.body.dataset.state = state;
    const info = STATES[state];
    txt('pill', info.pill); txt('alertIcon', info.icon); txt('alertTitle', info.title); txt('alertSub', info.sub);
    if ($('strip')) $('strip').hidden = state !== 'danger';
    if ($('muteBtn')) $('muteBtn').style.display = (state === 'danger' && !muted) ? '' : 'none';

    const v = readings.at(-1);
    if (v === undefined) return;
    txt('ppm', Math.min(v, MAX));                              // never show absurd numbers
    if ($('arc')) $('arc').style.strokeDashoffset = ARC_LEN * (1 - Math.min(v, MAX) / MAX);
    if ($('min')) {
      txt('min', Math.min(...readings));
      txt('max', Math.max(...readings));
      txt('avg', Math.round(readings.reduce((a, b) => a + b, 0) / readings.length));
    }
    drawSpark();
  }

  function render(v) {
    v = Math.min(v, MAX);
    readings.push(v);
    if (readings.length > 200) readings.shift();
    store.set('readings3', readings);

    const s = classify(v);
    if (s !== state) {                         // only react (and log) on a state change
      state = s; store.set('state', s);
      muted = false; store.set('muted', false);
      addLog(`${STATES[s].title} — level ${v}`, STATES[s].tag);
      if (s === 'danger') autoAlert();
    }
    paint();
    txt('updated', timeStr());
  }

  // Browsers block silent sending, so on danger we prompt for a one-tap alert
  function autoAlert() {
    toast('🚨 Smoke detected! Open Contacts and alert your people');
    addLog('Ready to alert contacts — open Contacts and tap send', 'danger');
  }

  // ---- sensors panel -----------------------------------------------------
  const SENSORS = [['mq2', 'MQ-2', 'Smoke · LPG'], ['mq7', 'MQ-7', 'Carbon monoxide'], ['mq135', 'MQ-135', 'Air quality']];
  if ($('sensorList')) {
    $('sensorList').innerHTML = SENSORS.map(([k, name, what]) =>
      `<div class="sensor"><div><b>${name}</b><small>${what}</small></div><div class="val" id="v-${k}">–</div><div class="bar"><i id="b-${k}"></i></div></div>`
    ).join('');
  }

  // The number shown is the sensor's rise above its baseline; the bar and its colour
  // show how close it is to its alarm point.
  function showSensors(r) {
    if (!$('sensorList')) return;
    store.set('lastSensors', r);
    const live = !!store.get('espIp', '');
    SENSORS.forEach(([k]) => {
      const rawVal = r[k] ?? 0;
      const lvl = Math.min(MAX, sensorLevel(k, rawVal, live, r.thresh));
      if ($('b-' + k)) {
        $('b-' + k).style.width = (lvl / MAX * 100) + '%';
        $('b-' + k).style.background = `var(--${classify(lvl)})`;
      }
      if ($('v-' + k)) $('v-' + k).textContent = r.warmup ? '…' : Math.round(rawVal);
    });
  }

  function setConn(m, left) {
    if ($('conn')) $('conn').classList.toggle('off', m === 'offline');
    if ($('connText')) $('connText').textContent = { demo: 'Demo mode', connecting: 'Connecting…', online: 'ESP32 online', offline: 'ESP32 offline', warmup: `Warming up ${left ?? ''}s` }[m];
    txt('mode', m === 'demo' ? 'Demo data' : 'Live');
  }

  // ---- backend helpers ---------------------------------------------------
  // Parse a timestamp from the usual Django field names; returns ms or null.
  function rowTime(row) {
    const raw = row.created_at ?? row.timestamp ?? row.time ?? row.ts ?? null;
    if (raw === null) return null;
    const ms = typeof raw === 'number' ? (raw < 1e12 ? raw * 1000 : raw) : Date.parse(raw);
    return Number.isNaN(ms) ? null : ms;
  }

  // Sends mute / test / recalibrate to the device through your Django backend,
  // which publishes them to elective3/gas_node/cmd on MQTT.
  // (Form-encoded so the browser does not need a CORS preflight.)
  async function sendCommand(cmd) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), 4000);
    try {
      const res = await fetch(`${BACKEND}/api/command/`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'cmd=' + encodeURIComponent(cmd),
        signal: ctl.signal
      });
      if (!res.ok) throw new Error(res.status);
      return true;
    } catch (e) {
      console.error('Command failed:', e);
      return false;
    } finally { clearTimeout(timer); }
  }

  // ---- data source -------------------------------------------------------
  // Live mode: fetch from the Django backend
  // Demo mode (no IP set): simulated readings
  async function readSensor() {
    const espIp = store.get('espIp', '');
    if (espIp) {
      const ctl = new AbortController();
      const timer = setTimeout(() => ctl.abort(), 2500);
      try {
        const res = await fetch(`${BACKEND}/api/telemetry/`, { signal: ctl.signal });
        if (!res.ok) throw new Error(res.status);

        const data = await res.json();
        const list = Array.isArray(data) ? data : (data.results || []);   // plain list or paginated
        if (!list.length) throw new Error('no telemetry yet');

        // Newest row: by timestamp if the backend sends one, otherwise assume the first row.
        let latest = list[0];
        if (rowTime(latest) !== null) {
          latest = list.reduce((a, b) => (rowTime(b) > rowTime(a) ? b : a), list[0]);
        }

        // If the newest reading is old, the device is not publishing any more.
        const t = rowTime(latest);
        if (t !== null && Date.now() - t > STALE_MS) throw new Error('stale telemetry');

        // MQ-2: prefer the device's own rise. If the backend only stores the absolute
        // average, fall back to "rise above the first reading after pressing Connect".
        let mq2;
        if (latest.mq2_rise !== undefined && latest.mq2_rise !== null) {
          mq2 = latest.mq2_rise;
        } else {
          const avg = latest.mq2_avg || 0;
          let idle = store.get('mq2Idle', null);
          if (idle === null) { idle = avg; store.set('mq2Idle', idle); }
          mq2 = Math.max(0, avg - idle);
        }

        // Optional extras: use them when the backend provides them.
        const thresh = {
          mq2: latest.mq2_thresh, mq7: latest.mq7_thresh, mq135: latest.mq135_thresh
        };
        const hasThresh = Object.values(thresh).every(x => typeof x === 'number' && x > 0);
        const alarm = typeof latest.alarm === 'boolean' ? latest.alarm : undefined;
        if (typeof latest.muted === 'boolean' && latest.muted !== muted && state === 'danger') {
          muted = latest.muted; store.set('muted', muted);
        }

        return {
          mq2,
          mq7: latest.mq7_rise || 0,
          mq135: latest.mq135_rise || 0,
          thresh: hasThresh ? thresh : undefined,
          alarm
        };
      } catch (e) {
        console.error('Backend fetch error:', e);
        throw e;
      } finally { clearTimeout(timer); }
    }

    // Demo data. Spikes go above DANGER so the Test button shows a real alarm.
    const jitter = () => Math.round((Math.random() - 0.5) * 30);
    let v;
    if (forceSpike) { forceSpike = false; v = 1050 + Math.floor(Math.random() * 150); }
    else {
      const last = readings.at(-1) ?? 120;
      v = last + (Math.random() - 0.5) * 60 + (120 - last) * 0.1;           // drift toward baseline
      if (Math.random() > 0.96) v = 1050 + Math.random() * 150;              // rare spike
      v = Math.round(Math.max(40, Math.min(1300, v)));
    }
    return { mq2: v, mq7: Math.max(0, Math.round(v * 0.55) + jitter()), mq135: Math.max(0, Math.round(v * 0.4) + jitter()) };
  }

  async function tick() {
    const espIp = store.get('espIp', '');
    try {
      const r = await readSensor();
      setConn(espIp ? (r.warmup ? 'warmup' : 'online') : 'demo', r.warmupLeft);
      showSensors(r);
      if (r.warmup) return;                                   // don't judge readings while sensors warm up
      render(overallLevel(r, !!espIp));
    } catch { setConn('offline'); }
  }

  // ---- actions (Home page) ----------------------------------------------
  if ($('muteBtn')) $('muteBtn').onclick = async () => {
    muted = true; store.set('muted', true); paint();
    addLog('Alarm silenced by user', 'info');
    if (store.get('espIp', '')) {
      const ok = await sendCommand('mute');
      if (!ok) addLog('Could not reach the backend to silence the buzzer', 'warn');
    }
  };
  if ($('testBtn')) $('testBtn').onclick = async () => {
    addLog('Sensor test started', 'info');
    if (store.get('espIp', '')) {
      const ok = await sendCommand('test');
      if (!ok) addLog('Could not reach the backend to run the test', 'warn');
      else toast('Test sounds are playing on the device');
    } else forceSpike = true;
    setTimeout(tick, 300);
  };

  // ---- backend connection (Sensors page) --------------------------------
  if ($('espIp')) {
    $('espIp').value = store.get('espIp', '');
    $('connectBtn').onclick = () => {
      const ip = $('espIp').value.trim().replace(/^https?:\/\//, '').replace(/\/.*$/, '');
      $('espIp').value = ip;
      store.set('espIp', ip);
      store.set('mq2Idle', null);                             // re-learn the MQ-2 fallback idle
      readings = []; store.set('readings3', readings);
      addLog(ip ? 'Connecting to backend...' : 'Switched to demo mode', 'info');
      setConn(ip ? 'connecting' : 'demo');
      tick();
    };
  }

  // ---- alert message & helpers ------------------------------------------
  const alertMsg = () =>
    `🚨 SafeHome ALERT: smoke/gas detected at home at ${new Date().toLocaleTimeString()}. Please check immediately.`;
  const cleanPhone = p => p.replace(/[^\d+]/g, '');
  const validPhone = p => p.replace(/\D/g, '').length >= 7;
  const copyMsg = msg => {
    try { return navigator.clipboard.writeText(msg).then(() => true, () => false); } catch { return Promise.resolve(false); }
  };

  // ---- contacts (Contacts page) -----------------------------------------
  if ($('contacts')) {
    let contacts = store.get('contacts', []);
    let editIndex = -1;                       // -1 = adding, otherwise index of the contact being edited

    function resetForm() {
      editIndex = -1;
      $('cName').value = $('cPhone').value = $('cMsgr').value = '';
      $('addContact').textContent = '+ Add contact';
      $('cancelEdit').style.display = 'none';
    }

    function startEdit(i) {
      const c = contacts[i];
      editIndex = i;
      $('cName').value = c.name;
      $('cPhone').value = c.phone || '';
      $('cMsgr').value = c.messenger || '';
      $('addContact').textContent = '💾 Save changes';
      $('cancelEdit').style.display = '';
      $('cName').focus();
    }

    function openSms(list) {
      const nums = list.map(c => cleanPhone(c.phone)).join(',');
      addLog(`SMS opened for ${list.map(c => c.name).join(', ')}`, 'info');
      const sep = /iPhone|iPad|iPod/i.test(navigator.userAgent) ? '&' : '?';   // iPhone "&body=", Android "?body="
      location.href = `sms:${nums}${sep}body=${encodeURIComponent(alertMsg())}`;
    }

    function openMessenger(c) {
      const msg = alertMsg();
      copyMsg(msg).then(ok => toast(ok ? 'Message copied — if the chat opens empty, just paste it' : 'If the chat opens empty, type your alert'));
      window.open(c.messenger ? `https://m.me/${encodeURIComponent(c.messenger)}?text=${encodeURIComponent(msg)}` : 'https://www.messenger.com/', '_blank');
      addLog(`Messenger opened for ${c.name}`, 'info');
    }

    function renderContacts() {
      const box = $('contacts');
      box.innerHTML = '';
      txt('contactCount', contacts.length);
      if (!contacts.length) { box.innerHTML = '<div class="empty">No contacts yet — add family or neighbours</div>'; return; }
      contacts.forEach((c, i) => {
        const row = document.createElement('div'); row.className = 'contact';
        const info = document.createElement('div'); info.className = 'cinfo';
        const n = document.createElement('b'); n.textContent = c.name;
        const p = document.createElement('span'); p.textContent = [c.phone, c.messenger && '@' + c.messenger].filter(Boolean).join(' · ');
        info.append(n, p);
        const mk = (label, cls, fn) => { const b = document.createElement('button'); b.className = 'chip ' + cls; b.textContent = label; b.onclick = fn; return b; };
        row.append(info);
        if (validPhone(c.phone)) row.append(mk('SMS', '', () => openSms([c])));
        row.append(
          mk('Messenger', '', () => openMessenger(c)),
          mk('✎', '', () => startEdit(i)),
          mk('✕', 'x', () => {
            contacts.splice(i, 1); store.set('contacts', contacts);
            if (editIndex === i) resetForm(); else if (editIndex > i) editIndex--;
            renderContacts();
          })
        );
        box.append(row);
      });
    }

    const contactInputs = document.querySelectorAll('#cName, #cPhone, #cMsgr');
    const addBtn = document.getElementById('addContact');

    if (contactInputs && addBtn) {
      contactInputs.forEach(input => {
        input.addEventListener('keypress', function (event) {
          if (event.key === 'Enter') {
            event.preventDefault();
            addBtn.click();
          }
        });
      });

      $('addContact').onclick = () => {
        const name = $('cName').value.trim();
        const phone = $('cPhone').value.trim();
        const messenger = $('cMsgr').value.trim().replace(/^.*(?:m\.me|messenger\.com\/t|facebook\.com)\//, '').replace(/[@\/]/g, '');
        if (!name || (!validPhone(phone) && !messenger)) { toast('Enter a name and a phone number or Messenger username'); return; }
        const entry = { name, phone: validPhone(phone) ? phone : '', messenger };
        if (editIndex >= 0) { contacts[editIndex] = entry; addLog(`Contact updated: ${name}`, 'info'); toast('Contact updated'); }
        else { contacts.push(entry); addLog(`Contact added: ${name}`, 'info'); }
        store.set('contacts', contacts);
        resetForm();
        renderContacts();
      };
    }

    if ($('cancelEdit')) $('cancelEdit').onclick = resetForm;

    if ($('smsAll')) {
      $('smsAll').onclick = () => {
        const list = contacts.filter(c => validPhone(c.phone));
        if (!list.length) { toast('Add a contact with a phone number first'); return; }
        openSms(list);
      };
    }

    if ($('shareBtn')) {
      $('shareBtn').onclick = async () => {
        const msg = alertMsg();
        if (navigator.share) {                  // phone share sheet: pick Messenger, SMS, etc.
          try { await navigator.share({ title: 'SafeHome Alert', text: msg }); addLog('Alert shared', 'info'); } catch {}
        } else {
          toast((await copyMsg(msg)) ? 'Alert message copied — paste it into Messenger' : 'Sharing is not supported here');
        }
      };
    }
    renderContacts();
  }

  // ---- alert strip & theme (every page) ---------------------------------
  if ($('strip')) $('strip').onclick = () => { location.href = 'contacts.html'; };

  function applyTheme(t) {
    document.documentElement.dataset.theme = t;
    if ($('themeBtn')) $('themeBtn').textContent = t === 'dark' ? '🌙' : '☀️';
  }
  applyTheme(document.documentElement.dataset.theme || 'dark');

  if ($('themeBtn')) {
    $('themeBtn').onclick = () => {
      const next = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
      applyTheme(next);
      try { localStorage.setItem('theme', next); } catch {}
    };
  }

  // ---- start -------------------------------------------------------------
  setConn(store.get('espIp', '') ? 'connecting' : 'demo');
  const last = store.get('lastSensors', null);
  if (last && store.get('espIp', '')) showSensors(last);
  paint();
  tick();
  setInterval(tick, 3000);
})();