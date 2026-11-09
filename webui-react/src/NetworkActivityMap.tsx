import { useEffect, useMemo, useRef, useState } from 'react';
import * as echarts from 'echarts/core';
import { EffectScatterChart, LinesChart, ScatterChart } from 'echarts/charts';
import { GeoComponent, TooltipComponent } from 'echarts/components';
import { CanvasRenderer } from 'echarts/renderers';

echarts.use([ScatterChart, EffectScatterChart, LinesChart, GeoComponent, TooltipComponent, CanvasRenderer]);

export type MapNode = {
  id: string;
  longitude: number;
  latitude: number;
  region: string;
  transport?: string;
  sentMessages?: number;
  receivedMessages?: number;
};

export type NetworkMapSnapshot = {
  geoAvailable: boolean;
  local?: { longitude: number; latitude: number; region: string };
  p2pLocal?: { longitude: number; latitude: number; region: string };
  dht: MapNode[];
  peers: MapNode[];
  dhtLocated: number;
  peerLocated: number;
  activityEpoch?: string;
  sampledAtMs?: number;
  broadcastSentMessages?: number;
};

type GeoGroup = {
  key: string;
  longitude: number;
  latitude: number;
  region: string;
  transport?: string;
  count: number;
  sentMessages?: number;
  receivedMessages?: number;
  alpha: number;
  stale: boolean;
};

type CacheEntry = { group: GeoGroup; lastSeen: number; present: boolean };
type DisplayState = { routes: GeoGroup[]; peers: GeoGroup[]; staleRoutes: number; stalePeers: number };
type MapFilter = 'all' | 'dht' | 'peers';
type ActivityCounters = { epoch: string; sampledAtMs: number; peers: Map<string, { sent: number; received: number }>; broadcastSent: number; dhtRequests?: number; dhtResponses?: number };
type FlowPulse = { id: string; region: string; transport: string; direction: 'send' | 'receive'; messages: number; coords: number[][]; expiresAt: number };
type OriginPulse = { id: string; source: 'dht' | 'p2p'; direction: 'send' | 'receive'; messages: number; label: string; expiresAt: number };

const demo: NetworkMapSnapshot = {
  geoAvailable: true,
  local: { longitude: 121.5, latitude: 31.2, region: '示意起点' },
  dhtLocated: 7,
  peerLocated: 3,
  dht: [
    [-122.3, 47.6], [-74, 40.7], [-0.1, 51.5], [8.7, 50.1],
    [77.2, 28.6], [139.7, 35.7], [151.2, -33.9],
  ].map(([longitude, latitude], i) => ({ id: `demo-route-${i}`, longitude, latitude, region: '演示位置' })),
  peers: [
    [-118.2, 34.1], [2.35, 48.9], [103.8, 1.3],
  ].map(([longitude, latitude], i) => ({ id: `demo-peer-${i}`, longitude, latitude, region: '演示位置', transport: i === 1 ? 'TCP' : 'UDP' })),
};

let worldLoaded: Promise<void> | undefined;
function loadWorld() {
  if (!worldLoaded) worldLoaded = fetch('/assets/ne-110m-world.geojson')
    .then(response => { if (!response.ok) throw new Error('世界地图资源不可用'); return response.json(); })
    .then(geojson => { echarts.registerMap('rats-world', geojson); })
    .catch(error => { worldLoaded = undefined; throw error; });
  return worldLoaded;
}

function normalizeTransport(value?: string) {
  const transport = value?.toUpperCase() || '';
  return transport === 'TCP' || transport === 'UDP' ? transport : 'OTHER';
}

// The API deliberately does not expose DHT IPs or stable DHT node IDs. These
// keys identify display aggregates by GeoIP region, not individual contacts.
function geoBucket(node: MapNode) {
  const region = node.region || '地区未知';
  return `${region}|${node.longitude.toFixed(1)}|${node.latitude.toFixed(1)}`;
}

function groupLocations(nodes: MapNode[], withTransport: boolean): GeoGroup[] {
  const groups = new Map<string, GeoGroup>();
  for (const node of nodes) {
    const region = node.region || '地区未知';
    const transport = withTransport ? normalizeTransport(node.transport) : undefined;
    const key = `${geoBucket(node)}${withTransport ? `|${transport}` : ''}`;
    const previous = groups.get(key);
    if (previous) {
      previous.count++;
      if (withTransport) {
        previous.sentMessages = (previous.sentMessages || 0) + Number(node.sentMessages || 0);
        previous.receivedMessages = (previous.receivedMessages || 0) + Number(node.receivedMessages || 0);
      }
    }
    else groups.set(key, {
      key,
      longitude: node.longitude,
      latitude: node.latitude,
      region,
      transport,
      count: 1,
      sentMessages: withTransport ? Number(node.sentMessages || 0) : undefined,
      receivedMessages: withTransport ? Number(node.receivedMessages || 0) : undefined,
      alpha: 1,
      stale: false,
    });
  }
  return [...groups.values()].sort((a, b) => a.key.localeCompare(b.key));
}

function hashUnit(value: string) {
  let hash = 2166136261;
  for (let i = 0; i < value.length; i++) hash = Math.imul(hash ^ value.charCodeAt(i), 16777619);
  return (hash >>> 0) / 0xffffffff;
}

function connectionCurve(key: string) {
  return 0.19 + hashUnit(key) * 0.13;
}

function originPulse(id: string, source: OriginPulse['source'], direction: OriginPulse['direction'], messages: number,
  label: string, expiresAt: number): OriginPulse {
  return { id, source, direction, messages, label, expiresAt };
}

export function NetworkActivityMap({ snapshot, requestError, dhtTotal, peerTotal, running, crawlerActivity }:
  { snapshot?: NetworkMapSnapshot; requestError?: string; dhtTotal?: number; peerTotal?: number; running?: boolean;
    crawlerActivity?: { sampleRequests?: number; sampleResponses?: number } }) {
  const host = useRef<HTMLDivElement>(null);
  const chart = useRef<ReturnType<typeof echarts.init> | null>(null);
  const cleanup = useRef<(() => void) | undefined>(undefined);
  const routeCache = useRef(new Map<string, CacheEntry>());
  const peerCache = useRef(new Map<string, CacheEntry>());
  const previousPeerIds = useRef<Set<string> | null>(null);
  const previousDataset = useRef<'demo' | 'live' | null>(null);
  const displayTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const pulseTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const previousOriginKey = useRef<string | null>(null);
  const previousActivity = useRef<ActivityCounters | null>(null);
  const activitySequence = useRef(0);
  const [filter, setFilter] = useState<MapFilter>('all');
  const [showDemo, setShowDemo] = useState(false);
  const [mapError, setMapError] = useState('');
  const [ready, setReady] = useState(false);
  const [display, setDisplay] = useState<DisplayState>({ routes: [], peers: [], staleRoutes: 0, stalePeers: 0 });
  const [pulseKeys, setPulseKeys] = useState<string[]>([]);
  const [flowPulses, setFlowPulses] = useState<FlowPulse[]>([]);
  const [originPulses, setOriginPulses] = useState<OriginPulse[]>([]);
  const [reducedMotion, setReducedMotion] = useState(() => matchMedia('(prefers-reduced-motion: reduce)').matches);
  const [themeRevision, setThemeRevision] = useState(0);

  const data = showDemo ? demo : snapshot;
  const hasCurrentRoutes = !!data?.dht?.length;
  const hasCurrentPeers = !!data?.peers?.length;
  const hasVisibleNodes = filter === 'dht' ? hasCurrentRoutes : filter === 'peers' ? hasCurrentPeers : hasCurrentRoutes || hasCurrentPeers;
  const visibleRoutes = useMemo(() => filter === 'peers' ? [] : display.routes, [display.routes, filter]);
  const visiblePeers = useMemo(() => filter === 'dht' ? [] : display.peers, [display.peers, filter]);

  useEffect(() => {
    const media = matchMedia('(prefers-reduced-motion: reduce)');
    const onChange = () => setReducedMotion(media.matches);
    media.addEventListener?.('change', onChange);
    return () => media.removeEventListener?.('change', onChange);
  }, []);

  useEffect(() => {
    const observer = new MutationObserver(() => setThemeRevision(value => value + 1));
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme', 'class'] });
    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    let disposed = false;
    loadWorld().then(() => {
      if (disposed || !host.current) return;
      const instance = echarts.init(host.current, undefined, { renderer: 'canvas' });
      chart.current = instance;
      instance.setOption({
        animation: !reducedMotion,
        animationDurationUpdate: 650,
        animationEasingUpdate: 'cubicOut',
        backgroundColor: 'transparent',
        geo: {
          map: 'rats-world', roam: false, silent: true,
          left: 7, right: 7, top: 16, bottom: 15,
          itemStyle: { areaColor: '#142131', borderColor: '#253b51', borderWidth: 0.55 },
          emphasis: { disabled: true },
        },
        tooltip: { trigger: 'item', renderMode: 'richText' },
        series: [
          { id: 'route-regions', name: 'DHT 路由地区', type: 'scatter', coordinateSystem: 'geo', data: [] },
          { id: 'peer-lines-tcp', name: 'TCP 已建立连接', type: 'lines', coordinateSystem: 'geo', data: [] },
          { id: 'peer-lines-udp', name: 'UDP 已建立连接', type: 'lines', coordinateSystem: 'geo', data: [] },
          { id: 'activity-flows', name: '采样窗口应用消息活动', type: 'lines', coordinateSystem: 'geo', data: [] },
          { id: 'peer-regions', name: 'Rats P2P 连接地区', type: 'scatter', coordinateSystem: 'geo', data: [] },
          { id: 'peer-pulses', name: '新连接提示', type: 'effectScatter', coordinateSystem: 'geo', data: [] },
          { id: 'origin-ring', name: '本机观察位置柔光', type: 'scatter', coordinateSystem: 'geo', data: [] },
          { id: 'origin-outline', name: '本机观察位置外环', type: 'scatter', coordinateSystem: 'geo', data: [] },
          { id: 'origin-core', name: '本机观察位置', type: 'scatter', coordinateSystem: 'geo', data: [] },
          { id: 'origin-pulses', name: '本机消息活动', type: 'effectScatter', coordinateSystem: 'geo', data: [] },
        ],
      });
      setReady(true);
      const observer = new ResizeObserver(() => instance.resize());
      observer.observe(host.current);
      cleanup.current = () => { observer.disconnect(); instance.dispose(); chart.current = null; };
    }).catch(error => { if (!disposed) setMapError(String(error)); });
    return () => { disposed = true; cleanup.current?.(); cleanup.current = undefined; };
  }, []);

  useEffect(() => {
    if (!data) return;
    const now = Date.now();
    const dataset = showDemo ? 'demo' : 'live';
    const routeGroups = groupLocations(data.dht || [], false);
    const peerGroups = groupLocations(data.peers || [], true);
    const currentPeerIds = new Set((data.peers || []).map(peer => peer.id).filter(Boolean));
    const datasetChanged = previousDataset.current !== dataset;
    if (datasetChanged) {
      previousDataset.current = dataset;
      previousPeerIds.current = currentPeerIds;
      previousActivity.current = null;
      setPulseKeys([]);
      setFlowPulses([]);
      setOriginPulses([]);
    } else {
      const oldPeerIds = previousPeerIds.current;
      const addedIds = oldPeerIds ? [...currentPeerIds].filter(id => !oldPeerIds.has(id)) : [];
      if (addedIds.length) {
        const added = new Set(addedIds);
        const nextPulseKeys = peerGroups.filter(group => data.peers.some(peer => added.has(peer.id)
          && `${geoBucket(peer)}|${normalizeTransport(peer.transport)}` === group.key)).map(group => group.key);
        setPulseKeys(nextPulseKeys);
        if (pulseTimer.current) clearTimeout(pulseTimer.current);
        pulseTimer.current = setTimeout(() => setPulseKeys([]), 2200);
      } else {
        setPulseKeys([]);
      }
      previousPeerIds.current = currentPeerIds;
    }

    const sampledAtMs = Number(data.sampledAtMs) || now;
    const activityEpoch = data.activityEpoch || '';
    const currentActivity: ActivityCounters = {
      epoch: activityEpoch,
      sampledAtMs,
      peers: new Map((data.peers || []).filter(peer => Number.isFinite(peer.sentMessages)
        && Number.isFinite(peer.receivedMessages)).map(peer => [peer.id, {
        sent: Number(peer.sentMessages), received: Number(peer.receivedMessages),
      }])),
      broadcastSent: Number(data.broadcastSentMessages || 0),
      dhtRequests: Number.isFinite(crawlerActivity?.sampleRequests) ? Number(crawlerActivity?.sampleRequests) : undefined,
      dhtResponses: Number.isFinite(crawlerActivity?.sampleResponses) ? Number(crawlerActivity?.sampleResponses) : undefined,
    };
    const previous = previousActivity.current;
    const intervalValid = !showDemo && !datasetChanged && !!activityEpoch && !!previous?.epoch
      && previous.epoch === activityEpoch && sampledAtMs >= previous.sampledAtMs
      // The dashboard polls every 15 seconds; reject missed-snapshot gaps so
      // a reconnect or stale interval cannot look like a fresh traffic burst.
      && sampledAtMs - previous.sampledAtMs <= 22_000;
    const nextFlows: FlowPulse[] = [];
    const local = data.p2pLocal || data.local;
    let sentActivity = 0;
    let receivedActivity = 0;
    if (intervalValid && previous) {
      if (local) {
        for (const peer of data.peers || []) {
          const before = previous.peers.get(peer.id);
          if (!before || !Number.isFinite(peer.sentMessages) || !Number.isFinite(peer.receivedMessages)) continue;
          const sentNow = Number(peer.sentMessages);
          const receivedNow = Number(peer.receivedMessages);
          const sentDelta = sentNow >= before.sent ? sentNow - before.sent : 0;
          const receivedDelta = receivedNow >= before.received ? receivedNow - before.received : 0;
          const transport = normalizeTransport(peer.transport);
          if (sentDelta > 0) {
            sentActivity += sentDelta;
            nextFlows.push({ id: `flow-${++activitySequence.current}`, region: peer.region, transport, direction: 'send', messages: sentDelta,
              coords: [[local.longitude, local.latitude], [peer.longitude, peer.latitude]], expiresAt: now + (transport === 'UDP' ? 1_250 : 2_000) });
          }
          if (receivedDelta > 0) {
            receivedActivity += receivedDelta;
            nextFlows.push({ id: `flow-${++activitySequence.current}`, region: peer.region, transport, direction: 'receive', messages: receivedDelta,
              coords: [[peer.longitude, peer.latitude], [local.longitude, local.latitude]], expiresAt: now + (transport === 'UDP' ? 1_250 : 2_000) });
          }
        }
      }

      const broadcastDelta = currentActivity.broadcastSent >= previous.broadcastSent
        ? currentActivity.broadcastSent - previous.broadcastSent : 0;
      const dhtSent = currentActivity.dhtRequests !== undefined && previous.dhtRequests !== undefined
        && currentActivity.dhtRequests >= previous.dhtRequests ? currentActivity.dhtRequests - previous.dhtRequests : 0;
      const dhtReceived = currentActivity.dhtResponses !== undefined && previous.dhtResponses !== undefined
        && currentActivity.dhtResponses >= previous.dhtResponses ? currentActivity.dhtResponses - previous.dhtResponses : 0;
      const localPulses: OriginPulse[] = [];
      if (dhtSent) localPulses.push(originPulse(`origin-${++activitySequence.current}`, 'dht', 'send', dhtSent, 'BEP 51 请求', now + 2_000));
      if (dhtReceived) localPulses.push(originPulse(`origin-${++activitySequence.current}`, 'dht', 'receive', dhtReceived, 'BEP 51 有效响应', now + 2_000));
      if (broadcastDelta) localPulses.push(originPulse(`origin-${++activitySequence.current}`, 'p2p', 'send', broadcastDelta, 'P2P 广播消息', now + 2_000));
      if (sentActivity) localPulses.push(originPulse(`origin-${++activitySequence.current}`, 'p2p', 'send', sentActivity, 'P2P 单播发送', now + 2_000));
      if (receivedActivity) localPulses.push(originPulse(`origin-${++activitySequence.current}`, 'p2p', 'receive', receivedActivity, 'P2P 单播接收', now + 2_000));
      if (localPulses.length) setOriginPulses(items => [...items.filter(item => item.expiresAt > now), ...localPulses].slice(-4));
      if (nextFlows.length) setFlowPulses(items => [...items.filter(item => item.expiresAt > now), ...nextFlows].slice(-12));
    }
    previousActivity.current = currentActivity;

    const routes = routeCache.current;
    const peers = peerCache.current;
    for (const entry of routes.values()) entry.present = false;
    for (const entry of peers.values()) entry.present = false;
    for (const group of routeGroups) routes.set(group.key, { group, lastSeen: now, present: true });
    for (const group of peerGroups) peers.set(group.key, { group, lastSeen: now, present: true });

    const makeDisplay = () => {
      const time = Date.now();
      const routeItems: GeoGroup[] = [];
      const peerItems: GeoGroup[] = [];
      for (const [key, entry] of routes) {
        const age = time - entry.lastSeen;
        if (age > 36_000) { routes.delete(key); continue; }
        const stale = !entry.present;
        const alpha = stale ? Math.max(0, 0.58 * (1 - Math.max(0, age - 24_000) / 12_000)) : 0.62;
        routeItems.push({ ...entry.group, alpha, stale });
      }
      for (const [key, entry] of peers) {
        const age = time - entry.lastSeen;
        if (!entry.present && age > 2_600) { peers.delete(key); continue; }
        const stale = !entry.present;
        const alpha = stale ? Math.max(0, 1 - age / 2_600) : 1;
        peerItems.push({ ...entry.group, alpha, stale });
      }
      const staleRoutes = routeItems.filter(item => item.stale).length;
      const stalePeers = peerItems.filter(item => item.stale).length;
      setDisplay({ routes: routeItems, peers: peerItems, staleRoutes, stalePeers });
      if (displayTimer.current) clearTimeout(displayTimer.current);
      if (staleRoutes || stalePeers) displayTimer.current = setTimeout(makeDisplay, 900);
    };
    makeDisplay();
    return () => { if (displayTimer.current) clearTimeout(displayTimer.current); };
  }, [data, showDemo, crawlerActivity]);

  useEffect(() => {
    const expiries = [...flowPulses, ...originPulses].map(item => item.expiresAt);
    if (!expiries.length) return;
    const delay = Math.max(16, Math.min(...expiries) - Date.now());
    const timer = setTimeout(() => {
      const now = Date.now();
      setFlowPulses(items => items.filter(item => item.expiresAt > now));
      setOriginPulses(items => items.filter(item => item.expiresAt > now));
    }, delay);
    return () => clearTimeout(timer);
  }, [flowPulses, originPulses]);

  useEffect(() => () => {
    if (displayTimer.current) clearTimeout(displayTimer.current);
    if (pulseTimer.current) clearTimeout(pulseTimer.current);
    cleanup.current?.();
  }, []);

  useEffect(() => {
    const instance = chart.current;
    if (!ready || !instance || !host.current) return;

    const css = getComputedStyle(host.current);
    const color = (name: string, fallback: string) => css.getPropertyValue(name).trim() || fallback;
    const origin = data?.p2pLocal || data?.local;
    const hasOrigin = !!origin;
    const displayedRoutes = visibleRoutes.slice(0, 200);
    const displayedPeers = visiblePeers.slice(0, 12);
    const displayedFlows = !reducedMotion && filter !== 'dht' ? flowPulses.filter(pulse => pulse.expiresAt > Date.now()).slice(-12) : [];
    const displayedOriginPulses = !reducedMotion ? originPulses.filter(pulse => pulse.expiresAt > Date.now()
      && (pulse.source === 'dht' ? filter !== 'peers' : filter !== 'dht')).slice(-4) : [];
    const routeColor = color('--rs-map-route', '#7890A4');
    const landColor = color('--rs-map-land', '#142131');
    const borderColor = color('--rs-map-border', '#253B51');
    const tcpColor = color('--rs-map-tcp', '#9E8BFA');
    const udpColor = color('--rs-map-udp', '#43D5D0');
    const peerColor = color('--rs-map-peer', '#63CFE2');
    const originColor = color('--rs-map-origin', '#4D9EFF');
    const packetColor = color('--rs-map-packet', '#4D9EFF');
    const tooltipBg = color('--rs-map-tooltip', '#0B1420');
    const tooltipText = color('--rs-map-text', '#D7E6F4');

    const peerSymbolData = displayedPeers.map(group => {
      return {
        id: group.key,
        name: group.region,
        value: [group.longitude, group.latitude, group.count],
        group,
        itemStyle: { color: peerColor, opacity: group.alpha, shadowBlur: group.stale ? 2 : 10, shadowColor: peerColor },
      };
    });
    const makeLines = (transport: string) => displayedPeers.filter(group => group.transport === transport && hasOrigin)
      .map(group => ({
        id: group.key,
        name: `${group.transport} · ${group.region}`,
        coords: [[origin!.longitude, origin!.latitude], [group.longitude, group.latitude]],
        group,
        lineStyle: {
          color: peerColor,
          width: 0.85,
          opacity: (transport === 'TCP' ? 0.56 : 0.62) * group.alpha,
          type: transport === 'UDP' ? 'dashed' : 'solid',
          curveness: connectionCurve(group.key),
        },
      }));

    const activityFlowData = displayedFlows.map(pulse => ({
      id: pulse.id,
      name: `${pulse.transport} ${pulse.direction === 'send' ? '发送' : '接收'}`,
      coords: pulse.coords,
      activity: pulse,
      lineStyle: {
        color: packetColor,
        width: (pulse.transport === 'TCP' ? 1.05 : 0.9) + Math.min(0.45, Math.log2(Math.max(1, pulse.messages)) * 0.08),
        opacity: 0.62 + Math.min(0.3, Math.log2(Math.max(1, pulse.messages)) * 0.06),
        type: pulse.transport === 'UDP' ? 'dashed' : 'solid',
        curveness: connectionCurve(`${pulse.region}|${pulse.transport}|${pulse.direction}`),
      },
    }));

    const tooltipFormatter = (params: any) => {
      const flow = params.data?.activity as FlowPulse | undefined;
      const localPulse = params.data?.activity as OriginPulse | undefined;
      if (flow) return `${flow.transport} 应用消息活动\n${flow.direction === 'send' ? '本机 → 远端' : '远端 → 本机'}\n${flow.region}\n采样窗口新增 ${flow.messages} 条消息（非逐包记录）`;
      if (localPulse) return `${localPulse.label}\n${localPulse.direction === 'send' ? '本机发送' : '本机收到有效响应'}\n采样窗口计数增加 ${localPulse.messages}（非逐包记录）`;
      const group = params.data?.group as GeoGroup | undefined;
      if (group) {
        const title = params.seriesId === 'route-regions' ? 'DHT 路由地区' : 'Rats P2P 连接地区';
        const detail = params.seriesId === 'route-regions'
          ? `${group.count} 个采样联系人`
          : `${group.count} 个已建立的 ${group.transport === 'OTHER' ? '其他' : group.transport} 连接`;
        return `${title}\n${group.region}\n${detail}${group.stale ? '\n最近快照 · 渐隐中' : ''}\nGeoIP 为地区近似位置`;
      }
      if (params.seriesId?.startsWith('origin-')) return `本机观察位置\n${origin?.region || '地区未知'}\nGeoIP 为近似地区`;
      return params.name || '';
    };

    const routeData = displayedRoutes.map(group => ({
      id: group.key,
      name: group.region,
      value: [group.longitude, group.latitude, group.count],
      group,
      itemStyle: { color: routeColor, opacity: group.alpha, borderColor: '#B8CDD8', borderWidth: 0.25 },
    }));
    const pulseData = !reducedMotion ? displayedPeers.filter(group => pulseKeys.includes(group.key)).map(group => ({
      id: group.key,
      name: group.region,
      value: [group.longitude, group.latitude],
      group,
      itemStyle: { color: group.transport === 'TCP' ? tcpColor : udpColor },
    })) : [];
    const originPulseData = displayedOriginPulses.filter(() => hasOrigin).map(pulse => ({
      id: pulse.id,
      name: pulse.label,
      value: [origin!.longitude, origin!.latitude],
      activity: pulse,
      itemStyle: { color: packetColor },
    }));

    const originKey = hasOrigin ? `${origin!.longitude.toFixed(3)}|${origin!.latitude.toFixed(3)}|${origin!.region}` : '';
    const updateOrigin = originKey !== previousOriginKey.current;
    if (updateOrigin) previousOriginKey.current = originKey;
    const series: any[] = [
        {
          id: 'route-regions', name: 'DHT 路由地区', type: 'scatter', coordinateSystem: 'geo',
          symbol: 'circle', symbolSize: (value: number[]) => 3.4 + Math.min(5, Math.sqrt(value[2] || 1) * 1.25),
          itemStyle: { color: routeColor }, data: filter === 'peers' ? [] : routeData,
          z: 2,
        },
        {
          id: 'peer-lines-tcp', name: 'TCP 已建立连接', type: 'lines', coordinateSystem: 'geo', z: 3,
          effect: { show: false }, lineStyle: { color: tcpColor, width: 0.85, opacity: 0.56, curveness: 0.24 },
          data: makeLines('TCP'),
        },
        {
          id: 'peer-lines-udp', name: 'UDP 已建立连接', type: 'lines', coordinateSystem: 'geo', z: 3,
          effect: { show: false }, lineStyle: { color: udpColor, width: 0.85, opacity: 0.62, type: 'dashed', curveness: 0.24 },
          data: makeLines('UDP'),
        },
        {
          id: 'activity-flows', name: '采样窗口应用消息活动', type: 'lines', coordinateSystem: 'geo', z: 4,
          effect: { show: !reducedMotion, period: Math.max(1.05, 1.8 - Math.min(0.75,
            Math.log2(Math.max(1, ...displayedFlows.map(pulse => pulse.messages))) * 0.12)), trailLength: 0.24, symbol: 'circle', symbolSize: 4 },
          lineStyle: { width: 1.1, opacity: 0.9 }, data: filter === 'dht' ? [] : activityFlowData,
        },
        {
          id: 'peer-regions', name: 'Rats P2P 连接地区', type: 'scatter', coordinateSystem: 'geo', z: 5,
          symbol: 'circle', symbolSize: (value: number[]) => 6 + Math.min(4, Math.sqrt(value[2] || 1)),
          itemStyle: { color: peerColor }, data: filter === 'dht' ? [] : peerSymbolData,
        },
        {
          id: 'peer-pulses', name: '新连接提示', type: 'effectScatter', coordinateSystem: 'geo', z: 6,
          showEffectOn: 'render', rippleEffect: { period: 1.9, scale: 2.6, brushType: 'stroke' },
          symbolSize: 7, itemStyle: { color: peerColor }, data: filter === 'dht' ? [] : pulseData,
        },
      ];
    series.push({
      id: 'origin-pulses', name: '本机消息活动', type: 'effectScatter', coordinateSystem: 'geo', z: 8,
      showEffectOn: 'render', rippleEffect: { period: 1.3, scale: 3, brushType: 'stroke' },
      symbolSize: 7, data: originPulseData,
    });
    if (updateOrigin) {
      series.push(
        {
          id: 'origin-ring', name: '本机观察位置柔光', type: 'scatter', coordinateSystem: 'geo', z: 7,
          symbolSize: 21, itemStyle: { color: 'rgba(77,158,255,0.11)', shadowBlur: 18, shadowColor: originColor },
          data: hasOrigin ? [{ id: 'local-observer', name: '本机观察位置', value: [origin!.longitude, origin!.latitude] }] : [],
        },
        {
          id: 'origin-outline', name: '本机观察位置外环', type: 'scatter', coordinateSystem: 'geo', z: 8,
          symbolSize: 16, itemStyle: { color: 'rgba(0,0,0,0)', borderColor: originColor, borderWidth: 1.1, opacity: 0.85 },
          data: hasOrigin ? [{ id: 'local-outline', name: '本机观察位置', value: [origin!.longitude, origin!.latitude] }] : [],
        },
        {
          id: 'origin-core', name: '本机观察位置', type: 'scatter', coordinateSystem: 'geo', z: 9,
          symbolSize: 8, itemStyle: { color: originColor, shadowBlur: 14, shadowColor: originColor },
          data: hasOrigin ? [{ id: 'local-core', name: '本机观察位置', value: [origin!.longitude, origin!.latitude] }] : [],
        },
      );
    }

    instance.setOption({
      animation: !reducedMotion,
      animationDurationUpdate: reducedMotion ? 0 : 650,
      animationEasingUpdate: 'cubicOut',
      geo: { itemStyle: { areaColor: landColor, borderColor, borderWidth: 0.55 } },
      tooltip: {
        trigger: 'item', renderMode: 'richText', backgroundColor: tooltipBg, borderColor: color('--rs-map-border', '#29445B'),
        textStyle: { color: tooltipText, fontSize: 11 }, formatter: tooltipFormatter,
      },
      series,
    }, { notMerge: false, lazyUpdate: true });
  }, [ready, visibleRoutes, visiblePeers, filter, reducedMotion, data, hasCurrentRoutes, hasCurrentPeers,
    pulseKeys, flowPulses, originPulses, themeRevision]);

  const currentRoutePoints = filter === 'peers' ? 0 : groupLocations(data?.dht || [], false).length;
  const currentPeerPoints = filter === 'dht' ? 0 : groupLocations(data?.peers || [], true).length;
  const unavailable = !showDemo && (!data?.geoAvailable || !hasVisibleNodes);
  return <div className="network-map">
    <div className="network-map-toolbar" aria-label="地图节点筛选">
      {([['all', '全部'], ['dht', '路由节点'], ['peers', '活跃连接']] as const).map(([value, label]) =>
        <button key={value} type="button" className={filter === value ? 'active' : ''}
          aria-pressed={filter === value} onClick={() => setFilter(value)}>{label}</button>)}
      {showDemo && <strong className="network-map-demo">DEMO · 示意数据</strong>}
    </div>
    <div className="network-map-canvas" ref={host} role="img" aria-label="世界网络活动地图" />
    {(mapError || (!showDemo && !running && !hasVisibleNodes) || unavailable) && <div className="network-map-empty">
      {mapError || requestError || (!running && !hasVisibleNodes ? 'DHT 网络未运行' : !data?.geoAvailable ? '缺少本地 GeoIP 数据，暂无可定位节点'
        : filter === 'peers' && !data?.peers?.length ? '暂无已定位的活跃连接' : '暂无可定位节点')}
      {!mapError && !showDemo && <button type="button" onClick={() => setShowDemo(true)}>查看示意</button>}
    </div>}
    <div className="network-map-footer">
      <span>路由地区点 {filter === 'peers' ? 0 : display.routes.length}（当前 {currentRoutePoints} · 渐隐 {filter === 'peers' ? 0 : display.staleRoutes}）</span>
      <span>已定位采样联系人 {showDemo ? demo.dhtLocated : data?.dhtLocated ?? '—'} / 路由表 {showDemo ? demo.dht.length : dhtTotal ?? '—'}</span>
      <span>P2P 地区点 {filter === 'dht' ? 0 : display.peers.length}（当前 {currentPeerPoints} · 渐隐 {filter === 'dht' ? 0 : display.stalePeers}）</span>
      <span>已定位连接 {showDemo ? demo.peerLocated : data?.peerLocated ?? '—'} / 已连接 {showDemo ? demo.peers.length : peerTotal ?? '—'}</span>
      {showDemo ? <button type="button" onClick={() => setShowDemo(false)}>返回真实数据</button> : <small>光点为采样窗口消息增量（非逐包） · 弧线为地区关联 · <a href="https://db-ip.com/db/download/ip-to-city-lite" target="_blank" rel="noreferrer">DB-IP City Lite</a></small>}
    </div>
  </div>;
}
