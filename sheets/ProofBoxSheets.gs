/**
 * ProofBox → Google Sheets
 *
 * Recibe de la app todo lo que pasó en una hoja (pasos, lecturas, notas, fotos,
 * informe) y arma con ello una Google Sheet completa en tu Drive, en la carpeta
 * "ProofBox": resumen con cifras y gráfico, pasos, un gráfico por paso, todas
 * las lecturas, notas y fotos, e informe.
 *
 * Instalación (una vez):
 *   1. Abre https://script.new con tu cuenta de Google.
 *   2. Borra lo que haya y pega este fichero entero. Guarda (⌘S).
 *   3. Implementar → Nueva implementación → tipo "Aplicación web".
 *      Ejecutar como: Yo. Quién tiene acceso: Cualquier usuario.
 *   4. Autoriza los permisos (crear hojas y carpetas en tu Drive).
 *   5. Copia la URL que termina en /exec y pégala en la app:
 *      Settings → Google Sheets.
 *
 * Corre con TU cuenta: las hojas son tuyas y solo quien tenga esa URL puede
 * pedir una. No lee nada de tu Drive; solo crea.
 */

const FOLDER = 'ProofBox';
const AMBER = '#c2700c', AMBER_SOFT = '#fbf1e2', INK = '#1c1916', MUTED = '#766e63', RULE = '#e5e0d7', BLUE = '#3f86c6', GREEN = '#2e9a5c';

function doPost(e) {
  try {
    const d = JSON.parse(e.postData.contents);
    const url = build(d);
    return out({ ok: true, url });
  } catch (err) {
    return out({ ok: false, error: String(err && err.message || err) });
  }
}
function doGet() { return out({ ok: true, hello: 'ProofBox → Google Sheets is installed' }); }
function out(o) { return ContentService.createTextOutput(JSON.stringify(o)).setMimeType(ContentService.MimeType.JSON); }

// Textos, según el idioma del informe en la app.
const TXT = {
  en: { summary: 'Summary', steps: 'Steps', charts: 'Charts', readings: 'Readings', notes: 'Notes & photos', report: 'Report',
    started: 'Started', exported: 'Exported', duration: 'Duration', target: 'Target', finalRise: 'Final growth', maxRise: 'Highest',
    grown: 'Grown', reached: 'Target reached', temp: 'Temperature', readingsN: 'Readings', yes: 'yes', no: 'not yet',
    keyFigures: 'Key figures', growthChart: 'Growth over the whole sheet', stepsTable: 'Step by step', phase: 'Phases (from the curve)',
    name: 'Step', from: 'From', to: 'To', hours: 'Hours', kind: 'Kind', measured: 'measured', byHand: 'noted by hand',
    startX: 'Start ×', endX: 'End ×', delta: 'Δ ×', rate: '× per hour', cm: 'cm grown', goal: 'Target', tAvg: 'Temp avg', tMin: 'Temp min', tMax: 'Temp max',
    note: 'Note', photos: 'Photos', time: 'Time', elapsed: 'Elapsed h', step: 'Step', growth: 'Growth ×', pct: '% of target', dist: 'Distance mm', cond: 'Conductivity %',
    text: 'Text', photo: 'Photo', hidden: 'Stretches hidden with the eraser (kept in the data, left out of every figure)',
    stepChart: 'Growth in this step', noData: 'No measured readings', latency: 'Latency ends', logPeak: 'Fastest rise at', logEnd: 'Log phase ends', still: 'Still rising',
    reportNote: 'Last report written in the app', h: 'h' },
  es: { summary: 'Resumen', steps: 'Pasos', charts: 'Gráficos', readings: 'Lecturas', notes: 'Notas y fotos', report: 'Informe',
    started: 'Inicio', exported: 'Exportado', duration: 'Duración', target: 'Objetivo', finalRise: 'Crecimiento final', maxRise: 'Máximo',
    grown: 'Crecido', reached: 'Objetivo alcanzado', temp: 'Temperatura', readingsN: 'Lecturas', yes: 'sí', no: 'aún no',
    keyFigures: 'Cifras clave', growthChart: 'Crecimiento de toda la hoja', stepsTable: 'Paso a paso', phase: 'Fases (según la curva)',
    name: 'Paso', from: 'Desde', to: 'Hasta', hours: 'Horas', kind: 'Tipo', measured: 'medido', byHand: 'anotado a mano',
    startX: '× inicio', endX: '× final', delta: 'Δ ×', rate: '× por hora', cm: 'cm crecidos', goal: 'Objetivo', tAvg: 'Temp media', tMin: 'Temp mín', tMax: 'Temp máx',
    note: 'Nota', photos: 'Fotos', time: 'Hora', elapsed: 'Horas', step: 'Paso', growth: 'Crecimiento ×', pct: '% del objetivo', dist: 'Distancia mm', cond: 'Conductividad %',
    text: 'Texto', photo: 'Foto', hidden: 'Tramos ocultados con la goma (siguen en los datos, fuera de toda cifra)',
    stepChart: 'Crecimiento en este paso', noData: 'Sin lecturas medidas', latency: 'Fin de la latencia', logPeak: 'Subida más rápida a', logEnd: 'Fin de la logarítmica', still: 'Sigue subiendo',
    reportNote: 'Último informe escrito en la app', h: 'h' }
};

function build(d) {
  const T = TXT[d.lang] || TXT.en;
  const title = `${d.sheet.name} · ProofBox`;
  const ss = SpreadsheetApp.create(title);
  if (d.tz) ss.setSpreadsheetTimeZone(d.tz);
  moveToFolder(ss);
  const date = v => (v ? new Date(v) : '');

  // ── Lecturas ─────────────────────────────────────────────────────────────
  const rd = ss.getSheets()[0];
  rd.setName(T.readings);
  const rh = [T.time, T.elapsed, T.step, T.growth, T.cm, T.pct, T.dist, T.temp + ' °C', T.cond];
  const rows = (d.readings || []).map(r => [date(r.t), r.h, r.step || '', num(r.x), num(r.cm), num(r.pct), num(r.dist), num(r.temp), num(r.cond)]);
  header(rd, 1, rh);
  if (rows.length) {
    rd.getRange(2, 1, rows.length, rh.length).setValues(rows);
    rd.getRange(2, 1, rows.length, 1).setNumberFormat('dd/MM/yyyy HH:mm');
    rd.getRange(2, 2, rows.length, 1).setNumberFormat('0.00');
    rd.getRange(2, 4, rows.length, 1).setNumberFormat('0.00"×"');
    rd.getRange(2, 5, rows.length, 1).setNumberFormat('0.0" cm"');
    rd.getRange(2, 6, rows.length, 1).setNumberFormat('0"%"');
    rd.getRange(2, 8, rows.length, 1).setNumberFormat('0.0"°"');
    rd.getRange(2, 9, rows.length, 1).setNumberFormat('0"%"');
    rd.getRange(1, 1, rows.length + 1, rh.length).createFilter();
    // El % del objetivo en color: de claro a ámbar, verde al llegar.
    const pr = rd.getRange(2, 6, rows.length, 1);
    rd.setConditionalFormatRules([
      SpreadsheetApp.newConditionalFormatRule().whenNumberGreaterThanOrEqualTo(100).setBackground('#d8f0e1').setFontColor(GREEN).setRanges([pr]).build(),
      SpreadsheetApp.newConditionalFormatRule().setGradientMinpointWithValue('#ffffff', SpreadsheetApp.InterpolationType.NUMBER, '0')
        .setGradientMaxpointWithValue('#f3cf97', SpreadsheetApp.InterpolationType.NUMBER, '100').setRanges([pr]).build()
    ]);
  }
  rd.setFrozenRows(1);
  widths(rd, [140, 70, 170, 90, 80, 90, 95, 105, 115]);

  // ── Resumen ──────────────────────────────────────────────────────────────
  const sm = ss.insertSheet(T.summary, 0);
  sm.setHiddenGridlines(true);
  widths(sm, [24, 190, 170, 190, 170, 40, 40, 40]);
  sm.getRange('B2:E2').merge().setValue(d.sheet.name).setFontSize(22).setFontWeight('bold').setFontColor(INK);
  const sub = [`${T.started} ${fmtDate(d.sheet.started, d.tz)}`, `${T.duration} ${fmtH(d.summary.durationH, T)}`, `${T.exported} ${fmtDate(d.exported, d.tz)}`].join('   ·   ');
  sm.getRange('B3:E3').merge().setValue(sub).setFontColor(MUTED).setFontSize(10);
  sm.getRange('B4:E4').merge().setBorder(null, null, true, null, null, null, AMBER, SpreadsheetApp.BorderStyle.SOLID_MEDIUM);

  section(sm, 6, T.keyFigures);
  const s = d.summary || {};
  const kv = [
    [T.target, s.target || '—', T.readingsN, s.readings != null ? s.readings : '—'],
    [T.finalRise, s.finalX != null ? s.finalX : '—', T.maxRise, s.maxX != null ? s.maxX : '—'],
    [T.grown, s.grownCm != null ? s.grownCm : '—', T.reached, s.reached == null ? '—' : (s.reached ? T.yes : T.no)],
    [T.temp, s.temp || '—', T.duration, fmtH(s.durationH, T)]
  ];
  sm.getRange(7, 2, kv.length, 4).setValues(kv);
  for (let i = 0; i < kv.length; i++) {
    sm.getRange(7 + i, 2).setFontColor(MUTED); sm.getRange(7 + i, 4).setFontColor(MUTED);
    sm.getRange(7 + i, 3).setFontWeight('bold').setFontSize(13).setHorizontalAlignment('left');
    sm.getRange(7 + i, 5).setFontWeight('bold').setFontSize(13).setHorizontalAlignment('left');
  }
  sm.getRange(8, 3).setNumberFormat('0.00"×"'); sm.getRange(8, 5).setNumberFormat('0.00"×"'); sm.getRange(9, 3).setNumberFormat('0.0" cm"');
  sm.getRange(7, 2, kv.length, 4).setBorder(null, null, null, null, null, true, RULE, SpreadsheetApp.BorderStyle.SOLID);

  let row = 12;
  if (d.phases && d.phases.length) {
    section(sm, row, T.phase); row++;
    sm.getRange(row, 2, d.phases.length, 2).setValues(d.phases.map(p => [p[0], p[1]]));
    sm.getRange(row, 2, d.phases.length, 1).setFontColor(MUTED);
    row += d.phases.length + 1;
  }

  // Gráfico principal: × y temperatura a lo largo de toda la hoja.
  section(sm, row, T.growthChart); row++;
  if (rows.length > 1) {
    const cd = ss.insertSheet('_chart');
    const cdRows = (d.readings || []).map(r => [r.h, num(r.x), num(r.temp)]);
    cd.getRange(1, 1, 1, 3).setValues([[T.elapsed, T.growth, T.temp + ' °C']]);
    cd.getRange(2, 1, cdRows.length, 3).setValues(cdRows);
    const ch = sm.newChart().setChartType(Charts.ChartType.LINE)
      .addRange(cd.getRange(1, 1, cdRows.length + 1, 3))
      .setNumHeaders(1)
      .setOption('title', '')
      .setOption('legend', { position: 'top' })
      .setOption('interpolateNulls', false)
      .setOption('hAxis', { title: T.elapsed, format: '0' })
      .setOption('series', { 0: { color: AMBER, lineWidth: 2, targetAxisIndex: 0 }, 1: { color: BLUE, lineWidth: 1, targetAxisIndex: 1 } })
      .setOption('vAxes', { 0: { title: '×', format: '0.00' }, 1: { title: '°C', format: '0.0' } })
      .setOption('width', 780).setOption('height', 360)
      .setPosition(row, 2, 0, 0).build();
    sm.insertChart(ch);
    cd.hideSheet();
    row += 19;
  } else { sm.getRange(row, 2).setValue(T.noData).setFontColor(MUTED); row += 2; }

  // Tabla de pasos en el resumen.
  section(sm, row, T.stepsTable); row++;
  const st = d.steps || [];
  if (st.length) {
    const hdr = [T.name, T.from, T.hours, T.endX];
    header(sm, row, hdr, 2);
    sm.getRange(row + 1, 2, st.length, 4).setValues(st.map(x => [x.name, date(x.start), x.hours != null ? x.hours : '', x.endX != null ? x.endX : '']));
    sm.getRange(row + 1, 3, st.length, 1).setNumberFormat('dd/MM HH:mm');
    sm.getRange(row + 1, 4, st.length, 1).setNumberFormat('0.0');
    sm.getRange(row + 1, 5, st.length, 1).setNumberFormat('0.00"×"');
    row += st.length + 2;
  }

  // ── Pasos ────────────────────────────────────────────────────────────────
  const sp = ss.insertSheet(T.steps, 1);
  const sh = [T.name, T.kind, T.from, T.to, T.hours, T.startX, T.endX, T.delta, T.rate, T.cm, T.goal, T.reached, T.tAvg, T.tMin, T.tMax, T.readingsN, T.photos, T.note];
  header(sp, 1, sh);
  if (st.length) {
    sp.getRange(2, 1, st.length, sh.length).setValues(st.map(x => [x.name, x.manual ? T.byHand : T.measured, date(x.start), date(x.end), num(x.hours),
      num(x.startX), num(x.endX), num(x.deltaX), num(x.rate), num(x.cm), x.goal || '', x.reached == null ? '' : (x.reached ? T.yes : T.no),
      num(x.tAvg), num(x.tMin), num(x.tMax), num(x.readings), num(x.photos), x.note || '']));
    sp.getRange(2, 3, st.length, 2).setNumberFormat('dd/MM/yyyy HH:mm');
    sp.getRange(2, 5, st.length, 1).setNumberFormat('0.0');
    sp.getRange(2, 6, st.length, 3).setNumberFormat('0.00"×"');
    sp.getRange(2, 9, st.length, 1).setNumberFormat('0.000');
    sp.getRange(2, 10, st.length, 1).setNumberFormat('0.0" cm"');
    sp.getRange(2, 13, st.length, 3).setNumberFormat('0.0"°"');
    sp.getRange(1, 1, st.length + 1, sh.length).applyRowBanding(SpreadsheetApp.BandingTheme.LIGHT_GREY, true, false);
    sp.getRange(2, 18, st.length, 1).setWrap(true);
  }
  sp.setFrozenRows(1); sp.setFrozenColumns(1);
  widths(sp, [180, 110, 125, 125, 60, 70, 70, 60, 75, 80, 110, 80, 75, 70, 70, 70, 60, 320]);

  // ── Gráficos: uno por paso medido ────────────────────────────────────────
  const gc = ss.insertSheet(T.charts, 2);
  gc.setHiddenGridlines(true);
  const gd = ss.insertSheet('_steps');
  let gcRow = 2, gdCol = 1;
  gc.getRange(1, 2).setValue(T.charts).setFontSize(18).setFontWeight('bold');
  for (const x of st) {
    const pts = (d.readings || []).filter(r => r.stepId === x.id);
    if (x.manual || pts.length < 3) continue;
    const t0 = new Date(x.start).getTime();
    const block = pts.map(r => [+((new Date(r.t).getTime() - t0) / 3600000).toFixed(3), num(r.x), num(r.temp)]);
    gd.getRange(1, gdCol, 1, 3).setValues([[T.hours, x.name, T.temp]]);
    gd.getRange(2, gdCol, block.length, 3).setValues(block);
    gc.getRange(gcRow + 1, 2).setValue(x.name).setFontSize(13).setFontWeight('bold').setFontColor(AMBER);
    gc.getRange(gcRow + 2, 2).setValue(`${fmtH(x.hours, T)} · ${x.startX != null ? x.startX.toFixed(2) : '—'}× → ${x.endX != null ? x.endX.toFixed(2) : '—'}×${x.goal ? ' · ' + T.goal + ' ' + x.goal : ''}`).setFontColor(MUTED);
    const ch = gc.newChart().setChartType(Charts.ChartType.LINE)
      .addRange(gd.getRange(1, gdCol, block.length + 1, 3)).setNumHeaders(1)
      .setOption('legend', { position: 'none' })
      .setOption('hAxis', { title: T.hours, format: '0.0' })
      .setOption('series', { 0: { color: AMBER, lineWidth: 2, targetAxisIndex: 0 }, 1: { color: BLUE, lineWidth: 1, targetAxisIndex: 1 } })
      .setOption('vAxes', { 0: { title: '×', format: '0.00' }, 1: { title: '°C', format: '0.0' } })
      .setOption('width', 720).setOption('height', 300)
      .setPosition(gcRow + 3, 2, 0, 0).build();
    gc.insertChart(ch);
    gcRow += 20; gdCol += 4;
  }
  gd.hideSheet();
  gc.setColumnWidth(1, 24);

  // ── Notas y fotos ────────────────────────────────────────────────────────
  const np = ss.insertSheet(T.notes, 3);
  let nr = 1;
  header(np, nr, [T.time, T.step, T.text]); nr++;
  const notes = d.notes || [];
  if (notes.length) { np.getRange(nr, 1, notes.length, 3).setValues(notes.map(n => [date(n.t), n.step || '', n.text])); np.getRange(nr, 1, notes.length, 1).setNumberFormat('dd/MM HH:mm'); nr += notes.length; }
  for (const x of st) if (x.note) { np.getRange(nr, 1, 1, 3).setValues([[date(x.start), x.name, x.note]]); np.getRange(nr, 1).setNumberFormat('dd/MM HH:mm'); nr++; }
  if (d.erased && d.erased.length) {
    nr++; np.getRange(nr, 1).setValue(T.hidden).setFontColor(MUTED).setFontStyle('italic'); nr++;
    np.getRange(nr, 1, d.erased.length, 2).setValues(d.erased.map(g => [date(g.from), date(g.to)]));
    np.getRange(nr, 1, d.erased.length, 2).setNumberFormat('dd/MM HH:mm'); nr += d.erased.length;
  }
  nr += 2;
  const ph = d.photos || [];
  if (ph.length) {
    header(np, nr, [T.time, T.step, T.photo]); nr++;
    for (const p of ph) {
      np.getRange(nr, 1, 1, 2).setValues([[date(p.t), p.step]]);
      np.getRange(nr, 1).setNumberFormat('dd/MM HH:mm');
      np.getRange(nr, 3).setFormula(`=IMAGE("${p.url}")`);
      np.setRowHeight(nr, 180); nr++;
    }
  }
  widths(np, [130, 190, 320]);
  np.setFrozenRows(1);

  // ── Informe ──────────────────────────────────────────────────────────────
  if (d.report && d.report.length) {
    const rp = ss.insertSheet(T.report, 4);
    rp.setHiddenGridlines(true);
    rp.setColumnWidth(1, 24); rp.setColumnWidth(2, 760);
    rp.getRange(1, 2).setValue(T.reportNote).setFontColor(MUTED).setFontStyle('italic');
    let r = 3;
    for (const b of d.report) {
      const c = rp.getRange(r, 2).setValue(b.text).setWrap(true).setVerticalAlignment('top');
      if (b.kind === 'h1') c.setFontSize(18).setFontWeight('bold').setFontColor(AMBER);
      else if (b.kind === 'h2') c.setFontSize(13).setFontWeight('bold').setFontColor(INK);
      else if (b.kind === 'li') c.setValue('•  ' + b.text);
      r++;
    }
  }

  sm.activate();
  ss.setActiveSheet(sm);
  return ss.getUrl();
}

// ── Ayudas ───────────────────────────────────────────────────────────────
function num(v) { return v == null || v === '' || !isFinite(v) ? '' : Number(v); }
function header(sh, row, labels, col) {
  const r = sh.getRange(row, col || 1, 1, labels.length).setValues([labels]);
  r.setFontWeight('bold').setBackground(AMBER_SOFT).setFontColor(INK).setBorder(null, null, true, null, null, null, AMBER, SpreadsheetApp.BorderStyle.SOLID);
  return r;
}
function section(sh, row, text) {
  sh.getRange(row, 2).setValue(text.toUpperCase()).setFontSize(10).setFontWeight('bold').setFontColor(AMBER);
}
function widths(sh, arr) { arr.forEach((w, i) => sh.setColumnWidth(i + 1, w)); }
function fmtDate(v, tz) { return v ? Utilities.formatDate(new Date(v), tz || Session.getScriptTimeZone(), 'dd/MM/yyyy HH:mm') : '—'; }
function fmtH(h, T) {
  if (h == null || !isFinite(h)) return '—';
  const m = Math.round(h * 60);
  return m < 60 ? m + ' min' : Math.floor(m / 60) + T.h + String(m % 60).padStart(2, '0');
}
function moveToFolder(ss) {
  const it = DriveApp.getFoldersByName(FOLDER);
  const folder = it.hasNext() ? it.next() : DriveApp.createFolder(FOLDER);
  DriveApp.getFileById(ss.getId()).moveTo(folder);
}
