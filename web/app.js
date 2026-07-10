// Gauge SPA — talks to the JSON API (see src/web/api.cpp).
// GET = read, POST = write. Small writes use query args; themes use JSON bodies.
'use strict';
var MN = ['BOOST', 'AFR', 'WATER', 'OIL P'];
var SEC_NAMES = ['None','Intake Air Temp','Oil Temp','Fuel Temp','Fuel Press','TPS','Eng Load','Ign Timing','Baro','Speed','Gear'];
var COLOR_KEYS = ['text','low','mid','high','bg','modeLabel','linkIcon','needle','peak'];
var themesCache = null;   // /api/themes payload
var editing = -1;         // slot open in the editor, -1 = closed

function $(id){ return document.getElementById(id); }
function toast(msg, err){
  var t = $('toast'); t.textContent = msg;
  t.className = 'toast show' + (err ? ' err' : '');
  clearTimeout(t._h); t._h = setTimeout(function(){ t.className = 'toast'; }, 1800);
}
function jget(u){ return fetch(u).then(function(r){ if(!r.ok) throw new Error(u+' '+r.status); return r.json(); }); }
function post(u, body){
  var opt = { method: 'POST' };
  if (body !== undefined) { opt.headers = {'Content-Type':'application/json'}; opt.body = JSON.stringify(body); }
  return fetch(u, opt).then(function(r){ if(!r.ok) throw new Error(u+' '+r.status); return r.text(); });
}
function act(name, q){ return post('/api/action/' + name + (q || '')).then(function(v){ refreshState(); return v; }).catch(function(){ toast('Failed', 1); }); }

// ---------- state ----------
function renderState(s){
  $('devname').textContent = s.name;
  document.title = s.name;
  $('chippeers').textContent = s.peers;
  $('chipmode').textContent = MN[s.mode] || '?';
  $('chippage').textContent = s.page === 0 ? 'GAUGE' : 'GLOWCRAFT';
  $('chipslot').textContent = 'P' + s.slot + (s.tpsync ? ' ⟲' : '');
  $('chipcan').textContent = s.canOk ? 'OK' : 'DOWN';
  $('brval').textContent = s.bright; $('bright').value = s.bright;
  setTgl('test', 'Test', s.test); setTgl('stats', 'Stats', s.stats);
  setTgl('dbg', 'Debug', s.dbg); setTgl('peak', 'Peak Hold', s.peak);
  $('font').textContent = 'Font: ' + (s.font === 0 ? 'DSEG14' : 'Fira Mono');
  $('tpsync').textContent = 'Trim Sync: ' + (s.tpsync ? 'ON' : 'OFF');
  $('tpsync').classList.toggle('on', !!s.tpsync);
  for (var i = 0; i < 4; i++) $('m'+i).classList.toggle('active', s.mode === i);
  $('pg0').classList.toggle('active', s.page === 0);
  $('pg1').classList.toggle('active', s.page === 1);
  if (document.activeElement !== $('sec')) $('sec').value = s.sec;
  if (!$('newname').value) $('newname').value = s.name;
  $('footer').textContent = 'v' + s.fw + ' · built ' + s.build;
  if (themesCache && themesCache.active !== s.slot) loadThemes();  // rotary moved the slot
}
function setTgl(id, label, on){
  var b = $(id); b.textContent = label + ': ' + (on ? 'ON' : 'OFF'); b.classList.toggle('on', !!on);
}
function refreshState(){ return jget('/api/state').then(renderState).catch(function(){}); }

// ---------- theme slots ----------
function renderSlots(t){
  themesCache = t;
  var html = '';
  t.slots.forEach(function(s, i){
    var sw = COLOR_KEYS.slice(0, 5).map(function(k){ return '<i style="background:' + s.colors[k] + '"></i>'; }).join('');
    html += '<div class="slot' + (i === t.active ? ' activeslot' : '') + '">'
      + '<div class="swatches">' + sw + '</div>'
      + '<div class="name">' + esc(s.name) + '<small>P' + i + (i === t.active ? ' · ACTIVE' : '') + '</small></div>'
      + '<button class="sm" onclick="openEditor(' + i + ')">Edit</button>'
      + '<button class="sm" onclick="copySlot(' + i + ')">Copy</button>'
      + (i === t.active ? '' : '<button class="sm" onclick="activate(' + i + ')">Activate</button>')
      + '</div>';
  });
  $('slots').innerHTML = html;
}
function esc(s){ return String(s).replace(/[&<>"]/g, function(c){ return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]; }); }
function loadThemes(){ return jget('/api/themes').then(renderSlots).catch(function(){}); }
function activate(i){
  post('/api/themes/' + i + '/activate').then(function(){ toast('Slot P' + i + ' live'); loadThemes(); refreshState(); })
    .catch(function(){ toast('Failed', 1); });
}
function tglSync(){ act('tpsync').then(loadThemes); }
function copySlot(src){
  var dst = prompt('Copy P' + src + ' into which slot? (0-3)');
  if (dst === null) return;
  dst = parseInt(dst, 10);
  if (isNaN(dst) || dst < 0 || dst > 3 || dst === src) { toast('Bad slot', 1); return; }
  post('/api/themes/' + dst + '/copy?from=' + src)
    .then(function(){ toast('P' + src + ' → P' + dst); loadThemes(); refreshState(); })
    .catch(function(){ toast('Copy failed', 1); });
}

// ---------- editor ----------
function openEditor(i){
  var s = themesCache.slots[i];
  editing = i;
  $('edslot').textContent = i;
  $('edname').value = s.name;
  COLOR_KEYS.forEach(function(k){ $('c_' + k).value = s.colors[k]; });
  $('c_g2').value = s.gradient.c2; $('c_g3').value = s.gradient.c3;
  $('g_type').value = s.gradient.type; $('g_stops').value = s.gradient.stops;
  $('g_angle').value = s.gradient.angle; $('gaval').textContent = s.gradient.angle + '°';
  $('edhint').textContent = (i === themesCache.active)
    ? 'This is the ACTIVE slot — Save applies live and pushes to all gauges.'
    : 'Inactive slot — Save stores it; the display only changes when you Activate.';
  gradUI();
  $('editor').classList.remove('hidden');
  $('editor').scrollIntoView({ behavior: 'smooth' });
}
function closeEditor(){ editing = -1; $('editor').classList.add('hidden'); }
function gradUI(){
  var t = +$('g_type').value, s = +$('g_stops').value;
  $('gsrow').style.display = t == 0 ? 'none' : '';
  $('g2row').style.display = t == 0 ? 'none' : '';
  $('g3row').style.display = (t != 0 && s == 3) ? '' : 'none';
  var ang = (t == 3 || t == 5);
  $('garow').style.display = ang ? '' : 'none';
  $('g_angle').style.display = ang ? '' : 'none';
}
function collectTheme(){
  var colors = {};
  COLOR_KEYS.forEach(function(k){ colors[k] = $('c_' + k).value; });
  return {
    fmt: 'ugb-theme', v: 1,
    name: $('edname').value.trim() || ('P' + editing),
    colors: colors,
    gradient: { type: +$('g_type').value, stops: +$('g_stops').value,
                angle: +$('g_angle').value, c2: $('c_g2').value, c3: $('c_g3').value }
  };
}
function saveTheme(){
  if (editing < 0) return;
  post('/api/themes/' + editing, collectTheme())
    .then(function(){ toast('Slot P' + editing + ' saved'); loadThemes(); refreshState(); })
    .catch(function(){ toast('Save failed', 1); });
}
function exportTheme(){
  if (editing < 0) return;
  var data = JSON.stringify(collectTheme(), null, 2);
  var a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([data], { type: 'application/json' }));
  a.download = 'theme-p' + editing + '.json';
  a.click();
  URL.revokeObjectURL(a.href);
}
function toggleImport(){ $('importbox').classList.toggle('hidden'); }
function importTheme(){
  var slot = +$('importslot').value, obj;
  try { obj = JSON.parse($('importjson').value); } catch (e) { toast('Bad JSON', 1); return; }
  post('/api/themes/' + slot, obj)
    .then(function(){ toast('Imported into P' + slot); $('importjson').value = ''; toggleImport(); loadThemes(); })
    .catch(function(){ toast('Import rejected', 1); });
}

// ---------- controls ----------
function tglBtn(name){ act(name); }
function tglFont(){ act('font'); }
function setMode(m){ act('mode', '?v=' + m); }
function setPage(p){ act('page', '?v=' + p); }
function rename(){
  var n = $('newname').value.trim();
  if (!n) { toast('Name required', 1); return; }
  post('/api/action/name?v=' + encodeURIComponent(n))
    .then(function(){ toast('Renamed — device restarting'); })
    .catch(function(){ toast('Rename failed', 1); });
}

// ---------- fleet ----------
function renderFleet(f){
  if (!f.peers.length) { $('fleet').innerHTML = '<small>No peers seen yet.</small>'; return; }
  var html = '';
  f.peers.forEach(function(p){
    var btns = MN.map(function(n, m){
      return '<button class="sm" onclick="peerMode(\'' + p.mac + '\',' + m + ')">' + n + '</button>';
    }).join('');
    html += '<div class="slot"><div class="name">Gauge ' + p.mac.slice(-6) + '<small>Mode ' + (MN[p.mode] || '?') + '</small></div>' + btns + '</div>';
  });
  $('fleet').innerHTML = html;
}
function peerMode(mac, m){
  post('/api/fleet/' + mac + '/mode?v=' + m).then(function(){ toast('Sent'); setTimeout(loadFleet, 600); })
    .catch(function(){ toast('Failed', 1); });
}
function loadFleet(){ return jget('/api/fleet').then(renderFleet).catch(function(){}); }

// ---------- boot ----------
(function(){
  var sel = $('sec');
  SEC_NAMES.forEach(function(n, i){
    var o = document.createElement('option'); o.value = i; o.textContent = n; sel.appendChild(o);
  });
  refreshState(); loadThemes(); loadFleet();
  setInterval(refreshState, 3000);
  setInterval(loadFleet, 5000);
})();
