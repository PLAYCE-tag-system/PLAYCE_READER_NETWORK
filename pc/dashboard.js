const $ = id => document.getElementById(id);
const esc = v => String(v ?? "").replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const when = t => t ? new Date(t * 1000).toLocaleString('ko-KR') : '-';
const ago = (t, now) => t ? `${Math.max(0, Math.floor(now - t))}초 전` : '-';
const metric = (value, unit = '') => value == null ? '-' : `${Number(value)}${unit}`;
const elapsed = seconds => {
  const value = Math.max(0, Math.floor(seconds || 0));
  const minutes = Math.floor(value / 60);
  return minutes ? `${minutes}분 ${value % 60}초 체류` : `${value}초 체류`;
};
const eventDuration = (event, now, activeTags) => {
  if (event.dwell_seconds != null) return elapsed(event.dwell_seconds);
  const tag = activeTags.get(`${event.device}|${event.epc}`);
  return event.event === 'seen' && tag && event.time >= tag.active_since
    ? `${tag.delayed ? '수신 지연 · ' : '진행 중 · '}${elapsed(now - tag.active_since)}`
    : '-';
};
const readerConfig = [
  {id: 'playce-92f6ac', index: 1},
  {id: 'playce-ca3490', index: 2},
  {id: 'playce-cb21f8', index: 3}
];
const hops = [
  {reader: readerConfig[0], title: '리더 1 → 리더 2'},
  {reader: readerConfig[1], title: '리더 2 → 리더 3'},
  {reader: readerConfig[2], title: '리더 3 → LoRa 수신기'}
];

function renderNode(element, title, device, tags, now, gateway = false) {
  const online = Boolean(device && device.online);
  const node = $(element);
  node.className = `node ${gateway ? 'receiver' : ''} ${tags.length ? 'has-tags' : ''} ${tags.length > 1 ? 'multi-tags' : ''} ${online ? 'online' : 'offline'}`;
  const status = online ? '온라인' : '오프라인';
  const readerStatus = !device?.heartbeat_online ? 'RFID 상태 지연'
    : device.reader_seen ? 'RFID 응답 확인' : 'RFID 응답 미확인';
  const items = tags.map(tag => {
    const seconds = Math.max(0, Math.floor(now - (tag.active_since ?? now)));
    return `<div class="tag-item digits-${String(seconds).length} ${tag.delayed ? 'delayed' : ''}" aria-label="${tag.delayed ? '수신 지연 · ' : ''}태그 ${esc(tag.epc)} · ${seconds}초 체류">
      <div class="tag-dwell"><strong>${seconds}</strong><span>초 체류</span></div>
      <div class="tag-identity"><strong class="tag-epc" title="${esc(tag.epc)}">${esc(tag.epc)}</strong>${tag.delayed ? '<span class="tag-delay">수신 지연</span>' : ''}</div>
    </div>`;
  }).join('');
  const nodeTitle = gateway
    ? '<strong class="node-index">수신기</strong>'
    : `<strong class="node-reader-label">리더${esc(title.slice(-1))}</strong>`;
  node.innerHTML = `<div class="node-stage"><div class="node-title" aria-label="${esc(title)}">${nodeTitle}</div>
    <div class="tag-list">${items || `<div class="no-tags">${gateway ? device?.packets ? 'PC로 수신 중' : '수신 대기 중' : '현재 태그 없음'}</div>`}</div></div>
    <div class="node-footer"><div class="node-details"><div class="node-name">${esc(device?.device || (gateway ? '수신기 미연결' : '보드 미확인'))}</div>
    <div class="node-meta">${gateway ? `${device?.connection === 'usb+wifi' ? 'USB + Wi-Fi' : device?.connection === 'wifi' ? 'Wi-Fi' : device?.connection === 'usb' ? 'USB' : '연결 대기'} · LoRa ${device?.radio_ready ? '정상' : '확인 중'} · 패킷 ${Number(device?.packets || 0)}` : `${readerStatus} · 활성 ${tags.length}개`}</div></div><span class="state">${status}</span></div>`;
}

function renderGateway(gateway) {
  const connection = {
    'usb+wifi': 'USB + Wi-Fi 연결됨',
    wifi: 'Wi-Fi 연결됨',
    usb: 'USB 연결됨'
  }[gateway?.connection] || '연결 끊김';
  $('gateway').innerHTML = gateway
    ? `<strong>수신기 ${esc(gateway.device)}</strong> · ${gateway.online ? connection : '연결 끊김'} · 받은 패킷 ${Number(gateway.packets || 0)}${gateway.connection?.includes('wifi') ? ` · <span class="wifi-signal">Wi-Fi ${metric(gateway.wifi_rssi, ' dBm')}</span>` : ''}`
    : '수신기 연결을 기다리는 중…';
}

function renderReaders(devices, now) {
  const configured = readerConfig.map(reader => ({
    ...reader,
    device: devices.get(reader.id)
  }));
  for (const reader of configured) {
    const visibleTags = reader.device?.tags.filter(tag => tag.visible) || [];
    renderNode(`node${reader.index}`, `리더 ${reader.index}`, reader.device, visibleTags, now);
  }
  const online = configured.filter(reader => reader.device?.online).length;
  const visibleTags = configured.flatMap(reader => reader.device?.tags.filter(tag => tag.visible) || []);
  const delayed = visibleTags.filter(tag => tag.delayed).length;
  $('summary').textContent = `리더 온라인 ${online}/${readerConfig.length} · 감지 중인 태그 ${visibleTags.length}개${delayed ? ` · 수신 지연 ${delayed}개` : ''}`;
}

function renderDiagnostics(devices, now) {
  $('diagnostics').innerHTML = hops.map(hop => {
    const device = devices.get(hop.reader.id);
    const measured = device?.hop_age_ms != null && device?.hop_rssi != null && device.hop_rssi > -200;
    const heartbeat = device?.heartbeat_seen ? ago(device.heartbeat_seen, now) : '-';
    const status = device?.hop_age_ms == null ? '새 리더 펌웨어 대기 중' : device.heartbeat_online ? '하트비트 수신 중' : '하트비트 지연';
    return `<div class="hop"><strong>${esc(hop.title)} <span class="${device?.heartbeat_online ? '' : 'warning'}">· ${status}</span></strong>
      <div class="hop-data"><span>RSSI <b>${measured ? metric(device.hop_rssi, ' dBm') : '-'}</b></span>
      <span>SNR <b>${measured ? metric(device.hop_snr10 / 10, ' dB') : '-'}</b></span>
      <span>누락 추정 <b>${device?.hop_age_ms == null ? '-' : metric(device?.missed, '개')}</b></span>
      <span>마지막 HB <b>${heartbeat}</b></span>
      <span>구간 수신 지연 <b>${metric(device?.hop_age_ms, ' ms')}</b></span>
      <span>RFID 응답 <b>${!device?.heartbeat_online ? '상태 지연' : device.reader_seen ? '있음' : '없음'}</b></span></div></div>`;
  }).join('');
}

function renderHistory(state) {
  const activeTags = new Map();
  for (const device of state.devices) {
    for (const tag of device.tags.filter(tag => tag.active)) {
      activeTags.set(`${device.device}|${tag.epc}`, tag);
    }
  }
  $('history').innerHTML = state.events.length
    ? `<table><thead><tr><th>시각</th><th>읽은 리더</th><th>상태</th><th>태그 EPC</th><th>경로</th><th>체류 시간</th></tr></thead><tbody>${state.events.map(event => `<tr>
        <td>${when(event.time)}</td><td class="code">${esc(event.device)}</td><td>${event.event === 'seen' ? '감지' : '이탈'}</td>
        <td class="code">${esc(event.epc)}</td><td>${esc(event.relay_path || '-')}</td><td>${eventDuration(event, state.now, activeTags)}</td>
      </tr>`).join('')}</tbody></table>`
    : '<div class="empty">태그 수신 이력이 없습니다.</div>';
}

function renderState(state) {
  const devices = new Map(state.devices.map(device => [device.device, device]));
  const gateway = state.devices.find(device => device.role === 'gateway');
  renderGateway(gateway);
  renderReaders(devices, state.now);
  renderNode('gatewayNode', '수신기', gateway, [], state.now, true);
  renderDiagnostics(devices, state.now);
  const readers = state.devices.filter(device => device.role === 'reader');
  $('devices').innerHTML = readers.map(device => `<div class="device-mini"><strong>${esc(device.device)}</strong><span>${device.online ? '온라인' : '오프라인'} · ${ago(device.last_seen, state.now)}</span></div>`).join('')
    || '<div class="empty">리더 데이터 대기 중</div>';
  renderHistory(state);
}

let refreshing = false;
async function refresh() {
  if (refreshing) return;
  refreshing = true;
  try {
    const response = await fetch('/api/state', {cache: 'no-store'});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    renderState(await response.json());
  } catch (error) {
    $('gateway').textContent = 'PC 수신 프로그램에 연결할 수 없습니다.';
  } finally {
    refreshing = false;
  }
}

refresh();
setInterval(refresh, 1000);
