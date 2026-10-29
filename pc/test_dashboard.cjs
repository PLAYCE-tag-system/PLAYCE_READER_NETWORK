const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const script = fs.readFileSync(path.join(__dirname, 'dashboard.js'), 'utf8');

function runDashboard(fetch) {
  const elements = new Map();
  const document = {
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, {innerHTML: '', textContent: '', className: ''});
      return elements.get(id);
    }
  };
  vm.runInNewContext(script, {document, fetch, setInterval() {}});
  return elements;
}

test('configured readers show dwell and delay while receiver hides tag details', async () => {
  const now = 110;
  const epc = 'E28069150000401D63E439A0';
  const tag = {epc, active: true, visible: true, delayed: true, active_since: 100};
  const state = {
    now,
    devices: [
      {device: 'playce-ca9bd0', role: 'gateway', online: true, connection: 'wifi',
        packets: 5, radio_ready: true, wifi_rssi: -60,
        tags: [{...tag, delayed: false}]},
      {device: 'playce-92f6ac', role: 'reader', online: true, heartbeat_online: true,
        reader_seen: true, tags: [tag]},
      {device: 'playce-ca3490', role: 'reader', online: false, tags: []},
      {device: 'playce-cb21f8', role: 'reader', online: true, heartbeat_online: true,
        reader_seen: true, tags: []},
      {device: 'playce-fccc98', role: 'reader', online: true, tags: []}
    ],
    events: [{device: 'playce-ca9bd0', epc, event: 'seen', time: 100, relay_path: ''}]
  };
  const elements = runDashboard(async () => ({ok: true, json: async () => state}));
  await new Promise(setImmediate);
  assert.match(elements.get('summary').textContent, /리더 온라인 2\/3/);
  assert.match(elements.get('summary').textContent, /수신 지연 1개/);
  assert.match(elements.get('node1').innerHTML, /수신 지연/);
  assert.match(elements.get('node1').className, /has-tags/);
  assert.doesNotMatch(elements.get('node1').className, /detecting/);
  assert.match(elements.get('node1').innerHTML, /node-reader-label">리더1<\/strong>/);
  assert.match(elements.get('node1').innerHTML, /10<\/strong><span>초 체류/);
  assert.doesNotMatch(elements.get('node2').className, /has-tags/);
  assert.doesNotMatch(elements.get('node2').innerHTML, /초 체류/);
  assert.doesNotMatch(elements.get('gatewayNode').className, /has-tags/);
  assert.match(elements.get('gatewayNode').innerHTML, /node-index">수신기<\/strong>/);
  assert.doesNotMatch(elements.get('gatewayNode').innerHTML, /node-kind|LoRa 수신기/);
  assert.doesNotMatch(elements.get('gatewayNode').innerHTML, /초 체류/);
  assert.doesNotMatch(elements.get('gatewayNode').innerHTML, /E28069150000401D63E439A0/);
  assert.match(elements.get('history').innerHTML, /진행 중 · 10초 체류/);
  assert.match(elements.get('diagnostics').innerHTML, /리더 1 → 리더 2/);
});

test('all reader cards show dwell; receiver card never shows tag EPC or dwell', async () => {
  const tag = {epc: 'E28069150000401D63E439A0', active: true, visible: true,
    delayed: false, active_since: 100};
  const state = {now: 110, events: [], devices: [
    {device: 'playce-ca9bd0', role: 'gateway', online: true, tags: [tag]},
    {device: 'playce-92f6ac', role: 'reader', online: true, heartbeat_online: true, tags: [tag]},
    {device: 'playce-ca3490', role: 'reader', online: true, heartbeat_online: true, tags: [tag]},
    {device: 'playce-cb21f8', role: 'reader', online: true, heartbeat_online: true, tags: [tag]}
  ]};
  const elements = runDashboard(async () => ({ok: true, json: async () => state}));
  await new Promise(setImmediate);
  for (const id of ['node1', 'node2', 'node3']) {
    assert.match(elements.get(id).className, /has-tags/);
    assert.match(elements.get(id).innerHTML, /10<\/strong><span>초 체류/);
    assert.match(elements.get(id).innerHTML, /E28069150000401D63E439A0/);
  }
  assert.doesNotMatch(elements.get('gatewayNode').className, /has-tags/);
  assert.doesNotMatch(elements.get('gatewayNode').innerHTML, /초 체류|E28069150000401D63E439A0/);
  assert.doesNotMatch(elements.get('gatewayNode').className, /detecting/);
});

test('failed state request shows connection error', async () => {
  const elements = runDashboard(async () => ({ok: false, status: 503}));
  await new Promise(setImmediate);
  assert.match(elements.get('gateway').textContent, /연결할 수 없습니다/);
});
