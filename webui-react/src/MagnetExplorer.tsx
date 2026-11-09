import { ChangeEvent, FormEvent, useEffect, useRef, useState } from 'react';

type Api = <T = unknown>(method: string, params?: Record<string, unknown>, signal?: AbortSignal) => Promise<T>;
type Target = { kind: 'magnet' | 'hash' | 'torrent'; label: string; hash?: string };
type NodeRow = { id: string; state: string; lastObserved: string; handshakeConfirmed?: boolean };
type Snapshot = {
  id: string; hash: string; status: 'running' | 'complete' | 'failed' | 'cancelled'; stage: string;
  error: string; metadata: { name?: string; sizeBytes?: number; fileCount?: number };
  peers: NodeRow[]; contacts: NodeRow[]; samples: { hash: string }[];
  peerCount: number; contactCount: number; sampleCount: number; sampleRequests: number;
  registeredCount: number; reusedCount: number; reusedContacts: number; verifiedCount: number;
  registeredContacts: number; registeredSamples: number; persistenceAvailable: boolean;
  peersTruncated: boolean; contactsTruncated: boolean; samplesTruncated: boolean;
};
const FILE_LIMIT = 5 * 1024 * 1024;
const stages = [
  ['解析目标', '校验磁力链接、InfoHash 或种子文件'],
  ['发现节点', '本轮 DHT 响应及 BT Peer 线索'],
  ['获取元数据', '临时 BEP 9 任务；不下载内容文件'],
  ['登记线索', '去重保存 24 小时；可复用不代表在线'],
] as const;
function base32ToHex(value: string): string | null {
  if (!/^[A-Z2-7]{32}$/i.test(value)) return null;
  const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
  let bits = 0, count = 0, hex = '';
  for (const char of value.toUpperCase()) {
    bits = (bits << 5) | alphabet.indexOf(char); count += 5;
    if (count >= 8) { count -= 8; hex += ((bits >> count) & 255).toString(16).padStart(2, '0'); bits &= (1 << count) - 1; }
  }
  return hex.length === 40 ? hex : null;
}
function parseTarget(input: string, file: File | null): Target {
  if (file) {
    if (!file.name.toLowerCase().endsWith('.torrent')) throw new Error('请选择 .torrent 文件。');
    if (!file.size || file.size > FILE_LIMIT) throw new Error('种子文件必须介于 1 B 与 5 MiB 之间。');
    return { kind: 'torrent', label: file.name };
  }
  const value = input.trim();
  if (/^[0-9a-f]{40}$/i.test(value)) return { kind: 'hash', label: 'InfoHash', hash: value.toLowerCase() };
  if (!/^magnet:\?/i.test(value)) throw new Error('请输入 40 位 InfoHash、有效磁力链接或 .torrent 文件。');
  let url: URL; try { url = new URL(value); } catch { throw new Error('磁力链接格式无效。'); }
  for (const xt of url.searchParams.getAll('xt')) {
    const match = /^urn:btih:([0-9a-f]{40}|[a-z2-7]{32})$/i.exec(xt);
    if (!match) continue;
    const hash = match[1].length === 40 ? match[1].toLowerCase() : base32ToHex(match[1]);
    if (hash) return { kind: 'magnet', label: '磁力链接', hash };
  }
  throw new Error('磁力链接缺少有效的 BTIH InfoHash。');
}
function fileBase64(file: File): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onerror = () => reject(new Error('读取种子文件失败。'));
    reader.onload = () => resolve(String(reader.result).split(',', 2)[1] || '');
    reader.readAsDataURL(file);
  });
}
function byteSize(value?: number): string {
  if (value === undefined) return '未取得';
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB']; let size = value, i = 0;
  while (size >= 1024 && i < units.length - 1) { size /= 1024; i++; }
  return `${i ? size.toFixed(1) : size} ${units[i]}`;
}
function nodeStatus(state: string): string {
  return ({ candidate: '待验证', dialing: '正在拨号', connected: '传输已连接', handshake: 'BT 握手完成',
    closed: '连接已关闭', responded: 'DHT 已响应' } as Record<string, string>)[state] || '未取得';
}
export function MagnetExplorer({ api }: { api: Api }) {
  const [input, setInput] = useState(''); const [file, setFile] = useState<File | null>(null);
  const [target, setTarget] = useState<Target | null>(null); const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [starting, setStarting] = useState(false); const [error, setError] = useState('');
  const fileInput = useRef<HTMLInputElement>(null); const activeId = useRef(''); const statusRef = useRef('');
  const mounted = useRef(true);
  useEffect(() => { document.title = '磁力探查 - Rats Search'; mounted.current = true;
    return () => { mounted.current = false; if (activeId.current && statusRef.current === 'running')
      void api('explore.cancel', { id: activeId.current }).catch(() => {}); };
  }, [api]);
  useEffect(() => {
    const id = snapshot?.id; if (!id || snapshot.status !== 'running') return;
    const timer = window.setInterval(() => { void api<Snapshot>('explore.snapshot', { id }).then(next => {
      if (!mounted.current || activeId.current !== id) return; statusRef.current = next.status; setSnapshot(next);
    }).catch(cause => { if (mounted.current && activeId.current === id) setError(`读取探查快照失败：${String(cause)}`); }); }, 2000);
    return () => window.clearInterval(timer);
  }, [api, snapshot?.id, snapshot?.status]);
  const selectFile = (event: ChangeEvent<HTMLInputElement>) => { const chosen = event.target.files?.[0] || null;
    setFile(chosen); if (chosen) setInput(''); setError(''); };
  const start = async (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault(); if (starting || snapshot?.status === 'running') return;
    setStarting(true); setError(''); setSnapshot(null); activeId.current = '';
    try {
      const parsed = parseTarget(input, file); setTarget(parsed);
      const params: Record<string, unknown> = parsed.kind === 'torrent'
        ? { kind: 'torrent', dataBase64: await fileBase64(file!) } : { kind: parsed.kind, target: input.trim() };
      const started = await api<{ id: string; hash: string }>('explore.start', params);
      if (!mounted.current) { void api('explore.cancel', { id: started.id }).catch(() => {}); return; }
      activeId.current = started.id; statusRef.current = 'running'; setTarget({ ...parsed, hash: started.hash });
      const first = await api<Snapshot>('explore.snapshot', { id: started.id });
      if (mounted.current && activeId.current === started.id) { statusRef.current = first.status; setSnapshot(first); }
    } catch (cause) { if (mounted.current) setError(cause instanceof Error ? cause.message : String(cause)); }
    finally { if (mounted.current) setStarting(false); }
  };
  const cancel = async () => { const id = activeId.current; if (!id) return;
    try { const next = await api<Snapshot>('explore.cancel', { id }); statusRef.current = next.status; setSnapshot(next); setError(''); }
    catch (cause) { setError(`取消失败：${String(cause)}`); } };
  const retrySnapshot = async () => { if (!activeId.current) return;
    try { const next = await api<Snapshot>('explore.snapshot', { id: activeId.current }); statusRef.current = next.status; setSnapshot(next); setError(''); }
    catch (cause) { setError(`读取探查快照失败：${String(cause)}`); } };
  const stageState = (index: number): [string, string] => {
    if (!snapshot) return ['waiting', '等待'];
    if (index === 0) return ['success', '已解析'];
    if (index === 3) {
      if (!snapshot.persistenceAvailable) return ['failed', '本机存储不可用'];
      if (snapshot.status === 'running') return ['active', '持续登记中'];
      if (snapshot.status === 'cancelled') return ['skipped', '已停止登记'];
      return ['success', '登记状态已确认'];
    }
    if (index === 2 && target?.kind === 'torrent') return ['skipped', '文件已提供'];
    if (snapshot.status === 'cancelled') return ['skipped', '已取消'];
    if (snapshot.status === 'failed') {
      const failedHere = index === (snapshot.stage === 'metadata' ? 2 : 1);
      return failedHere ? ['failed', '失败'] : [index === 1 ? 'success' : 'skipped', index === 1 ? '已完成' : '未开始'];
    }
    if (snapshot.status === 'complete' && index === 1 && snapshot.error.includes('DHT network is stopped')) return ['skipped', '网络关闭'];
    if (snapshot.status === 'complete') return ['success', '已完成'];
    if (index === 1) return snapshot.stage === 'discovering' ? ['active', '查询中'] : ['success', '已发现线索'];
    return snapshot.stage === 'metadata' ? ['active', '获取中'] : ['waiting', '等待 Peer'];
  };
  return <div className="magnet-explorer">
    <section className="magnet-hero panel">
      <div className="magnet-hero-heading"><div><span className="magnet-eyebrow">MAGNET EXPLORER / 03</span><h1>探查目标</h1><p>只获取元数据与网络线索，不下载资源内容。候选地址在 24 小时内去重保存；登记不代表在线。</p></div><span className="magnet-demo-tag">真实探查 · 本机线索库</span></div>
      <form className="magnet-target-form" onSubmit={start}>
        <div className="magnet-target-input"><label htmlFor="magnet-target">磁力链接或 InfoHash</label><input id="magnet-target" value={input} disabled={starting || snapshot?.status === 'running'} onChange={event => { setInput(event.target.value); setFile(null); if (fileInput.current) fileInput.current.value = ''; setError(''); }} placeholder="magnet:?xt=urn:btih:…  或  40 位 InfoHash" autoComplete="off" spellCheck={false}/></div>
        <div className="magnet-file-input"><span>或者选择种子文件</span><input ref={fileInput} id="magnet-file" type="file" accept=".torrent,application/x-bittorrent" disabled={starting || snapshot?.status === 'running'} onChange={selectFile}/><label htmlFor="magnet-file">选择 .torrent</label><small title={file?.name}>{file?.name || '尚未选择文件'}</small></div>
        <div className="magnet-target-actions"><button className="primary" type="submit" disabled={starting || snapshot?.status === 'running'}>{starting ? '正在解析…' : snapshot?.status === 'running' ? '探查中…' : '开始探查'}</button>{snapshot?.status === 'running' && <button type="button" onClick={() => void cancel()}>取消</button>}</div>
      </form>
      {error && <p className="magnet-error" role="alert">{error} {activeId.current && <button type="button" onClick={() => void retrySnapshot()}>重试快照</button>}</p>}
      {target && <div className="magnet-current-target"><span>本次目标</span><strong>{target.label}</strong>{target.hash && <code>{target.hash}</code>}</div>}
    </section>
    <section className="magnet-progress panel" aria-label="探查进度"><div className="panel-title"><h2>探查进度</h2><span>{snapshot?.status === 'running' ? '真实查询中' : snapshot?.status === 'complete' ? snapshot.error ? '种子已解析；DHT 网络关闭' : '本轮完成' : snapshot?.status === 'failed' ? `探查失败：${snapshot.error}` : snapshot?.status === 'cancelled' ? '已取消' : '等待输入'}</span></div><ol>{stages.map(([name, description], index) => { const [state, label] = stageState(index); return <li className={`magnet-stage is-${state}`} key={name}><span className="magnet-stage-index">0{index + 1}</span><div><strong>{name}</strong><small>{description}</small></div><span className="magnet-stage-state">{label}</span></li>; })}</ol></section>
    <section className="magnet-resource panel"><div className="panel-title"><h2>资源信息与统计</h2><span>{snapshot?.metadata?.name ? '已取得可信元数据' : '尚未取得元数据'}</span></div><div className="magnet-resource-body"><div className="magnet-resource-name"><span>资源名称</span><strong>{snapshot?.metadata?.name || '未取得'}</strong><small>InfoHash：{snapshot?.hash || '未取得'} · 状态：{target?.kind === 'torrent' ? '种子文件已解析' : snapshot?.metadata?.name ? 'BEP 9 已校验' : '未取得完整元数据'}</small></div><div className="magnet-stat-grid">{[['文件数量', snapshot?.metadata?.fileCount ?? '未取得'], ['资源标称容量', byteSize(snapshot?.metadata?.sizeBytes)], ['BT Peer 线索', snapshot ? `观察 ${snapshot.peerCount} · 登记 ${snapshot.registeredCount < 0 ? '不可用' : snapshot.registeredCount} · 复用 ${snapshot.reusedCount} · 已验证 ${snapshot.verifiedCount < 0 ? '不可用' : snapshot.verifiedCount}` : '未取得'], ['DHT 响应联系人', snapshot ? `本轮 ${snapshot.contactCount} · 登记 ${snapshot.registeredContacts < 0 ? '不可用' : snapshot.registeredContacts} · 复用 ${snapshot.reusedContacts}` : '未取得'], ['同轮 DHT 样本', snapshot ? `本轮 ${snapshot.sampleCount} · 登记 ${snapshot.registeredSamples < 0 ? '不可用' : snapshot.registeredSamples}` : '未取得']].map(([label, value]) => <div key={label}><span>{label}</span><b>{value}</b><small>{String(label).includes('Peer') || String(label).includes('联系人') ? '已登记不表示当前在线' : String(label).includes('样本') ? '不保证与目标资源相关' : '资源标称信息'}</small></div>)}</div></div>{snapshot && !snapshot.persistenceAvailable && <p className="magnet-error" role="status">线索库暂不可用或本轮有写入失败；登记数量不能确认，探查仍可继续。</p>}</section>
    <div className="magnet-node-grid">
      <section className="panel"><div className="panel-title"><h2>BT Peer 名单</h2><span>本轮观察 {snapshot?.peerCount ?? 0} · 最多显示 50 条{snapshot?.peersTruncated ? ' · 名单已截断' : ''}</span></div><p className="magnet-list-note">候选在握手前保持未验证。匿名标识仅用于本轮显示；已登记地址会在 24 小时内供元数据任务复用。</p><div className="magnet-node-list">{snapshot?.peers.length ? snapshot.peers.map(peer => <div className="magnet-node-row" key={peer.id}><div><strong>Peer #{peer.id}</strong><small>最近观察：{new Date(peer.lastObserved).toLocaleString()} · 已登记线索</small></div><span className={peer.handshakeConfirmed ? 'is-good' : ''}>{nodeStatus(peer.state)}{peer.state === 'closed' && peer.handshakeConfirmed ? ' · 曾完成握手' : ''}</span></div>) : <p className="magnet-empty">尚未从本轮 DHT 查询取得 BT Peer 线索。</p>}</div></section>
      <section className="panel"><div className="panel-title"><h2>DHT 联系人</h2><span>本轮响应 {snapshot?.contactCount ?? 0} · 最多显示 50 条{snapshot?.contactsTruncated ? ' · 名单已截断' : ''}</span></div><p className="magnet-list-note">仅列出实际响应 KRPC 的联系人；重用时仅向其发起受控采样，不写入路由表。</p><div className="magnet-node-list">{snapshot?.contacts.length ? snapshot.contacts.map(contact => <div className="magnet-node-row" key={contact.id}><div><strong>联系人 #{contact.id}</strong><small>最近观察：{new Date(contact.lastObserved).toLocaleString()} · 已验证响应</small></div><span className="is-good">{nodeStatus(contact.state)}</span></div>) : <p className="magnet-empty">尚未收到本次查询的 DHT 响应。</p>}</div></section>
    </div>
    <section className="magnet-clues panel"><div className="panel-title"><h2>延伸线索</h2><span>同轮 DHT 样本 · 与目标资源无关联保证</span></div><div className="magnet-clue-summary"><span>本轮去重样本 <b>{snapshot?.sampleCount ?? '未取得'}</b></span><span>采样请求 <b>{snapshot?.sampleRequests ?? 0} / 2</b></span><span>24 小时内已登记 <b>{snapshot && snapshot.registeredSamples >= 0 ? snapshot.registeredSamples : '不可用'}</b></span><span>BT Peer 线索复用 <b>{snapshot?.reusedCount ?? '未取得'}</b></span>{snapshot?.samplesTruncated && <span>名单已截断</span>}</div>{snapshot?.samples.length ? <div className="magnet-clue-list">{snapshot.samples.map(clue => <div key={clue.hash}><code>{clue.hash}</code><span>本轮观察 · 未核对本地索引</span></div>)}</div> : <p className="magnet-empty">尚无同轮 DHT 样本；这不表示目标资源没有更多 Peer。</p>}</section>
  </div>;
}
