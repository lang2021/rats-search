import { FormEvent, KeyboardEvent, ReactNode, Suspense, lazy, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Link, Navigate, Route, Routes, useLocation, useNavigate, useParams, useSearchParams } from 'react-router-dom';
import { CompactNumber } from './CompactNumber';
import { MagnetExplorer } from './MagnetExplorer';

const NetworkActivityMap = lazy(() => import('./NetworkActivityMap').then(module => ({ default: module.NetworkActivityMap })));

type Torrent = { hash: string; name: string; size: number; files: number; files_list?: {path:string;size:number}[]; added?: number; seeders?:number; leechers?:number; peer?:string; remote?:boolean; contentType?:string; contentCategory?:string; matchingPaths?:string[]; fileMatch?:boolean; isFileMatch?:boolean };
type Env = {success:boolean;data?:any;error?:string};
const tokenKey='rats-api-token';
function token(){const saved=localStorage.getItem(tokenKey);if(saved){sessionStorage.removeItem(tokenKey);return saved}const legacy=sessionStorage.getItem(tokenKey)||'';if(legacy){localStorage.setItem(tokenKey,legacy);sessionStorage.removeItem(tokenKey)}return legacy}
function storeToken(value:string){localStorage.setItem(tokenKey,value);sessionStorage.removeItem(tokenKey)}
function clearToken(){localStorage.removeItem(tokenKey);sessionStorage.removeItem(tokenKey)}
let tokenRequest:Promise<string>|null=null;
function requestToken():Promise<string>{const saved=token();if(saved)return Promise.resolve(saved);if(tokenRequest)return tokenRequest;tokenRequest=new Promise(resolve=>{const dialog=document.createElement('dialog');dialog.setAttribute('aria-label','API 令牌');dialog.style.cssText='background:#10120d;color:#e6e9df;border:1px solid #b5ff00;padding:24px;max-width:min(420px,90vw)';const form=document.createElement('form');form.method='dialog';const label=document.createElement('label');label.textContent='输入本机数据目录 api-token 文件中的 API 令牌';const input=document.createElement('input');input.type='password';input.autocomplete='off';input.required=true;input.style.cssText='display:block;box-sizing:border-box;width:100%;margin:16px 0;padding:10px;background:#080908;color:#fff;border:1px solid #7b8c45';const button=document.createElement('button');button.type='submit';button.textContent='连接 API';button.style.cssText='padding:9px 16px;background:#b5ff00;color:#080908;border:0;cursor:pointer';label.appendChild(input);form.append(label,button);dialog.appendChild(form);document.body.appendChild(dialog);let submitted=false;form.addEventListener('submit',()=>{submitted=true});dialog.addEventListener('close',()=>{const value=submitted?input.value.trim():'';if(value){storeToken(value);window.dispatchEvent(new Event('rats-authenticated'))}dialog.remove();tokenRequest=null;resolve(value)});dialog.showModal();input.focus()});return tokenRequest}
async function api<T=any>(method:string,params:Record<string,unknown>={},signal?:AbortSignal){const t=await requestToken();if(!t)throw new Error('未提供 API 令牌。');const r=await fetch(`/api/${method}`,{method:'POST',headers:{'Content-Type':'application/json',Authorization:`Bearer ${t}`},body:JSON.stringify(params),signal});if(r.status===401){clearToken();window.dispatchEvent(new Event('rats-auth-failed'));throw new Error('API 令牌无效；令牌已清除，请重新连接。')}const e=await r.json() as Env;if(!r.ok||!e.success)throw new Error(e.error||`API ${r.status}`);return e.data as T}
async function ready(){const t=await requestToken();const r=await fetch('/readyz',{headers:t?{Authorization:`Bearer ${t}`}:{}});if(r.status===401){clearToken();window.dispatchEvent(new Event('rats-auth-failed'));throw new Error('API 令牌无效；令牌已清除，请重新连接。')}return r.ok}
function wsUrl(){if(import.meta.env.DEV)return `ws://${location.host}/ws`;return `${location.protocol==='https:'?'wss':'ws'}://${location.hostname}:${Number(location.port|| (location.protocol==='https:'?443:80))+1}`}
function useTitle(title:string){useEffect(()=>{document.title=`${title} - Rats Search`},[title])}
const nav=[['/console','01','总览'],['/search','02','资源搜索'],['/discover','03','发现资源'],['/history','04','搜索历史'],['/watch','•','资源关注'],['/magnet-explorer','◆','磁力探查'],['/console/network','05','DHT / P2P'],['/console/index','06','索引与同步'],['/console/downloads','07','下载任务'],['/console/events','08','实时事件'],['/console/settings','09','基础设置']];
function App(){const loc=useLocation();const [menu,setMenu]=useState(false);const [unread,setUnread]=useState<number|null>(null);const title=useMemo(()=>loc.pathname==='/discover'?'':loc.pathname==='/console/network'?'网络状态':nav.find(x=>x[0]===loc.pathname)?.[2]||'资源详情',[loc.pathname]);const [theme,setTheme]=useState(localStorage.getItem('rats-search-theme')||'tundra');const [language,setLanguage]=useState(localStorage.getItem('bitmagnet-language')||'zh');useEffect(()=>{document.documentElement.dataset.theme=theme==='tundra'?'dark-industrial':'light-industrial';localStorage.setItem('rats-search-theme',theme)},[theme]);useEffect(()=>{localStorage.setItem('bitmagnet-language',language)},[language]);useEffect(()=>{let socket:WebSocket|undefined;let timer:ReturnType<typeof setTimeout>;let dead=false;const connect=()=>{if(dead)return;const t=token();if(!t){timer=setTimeout(connect,1000);return}socket=new WebSocket(wsUrl());socket.onopen=()=>socket?.send(JSON.stringify({method:'auth',params:{token:t}}));socket.onmessage=e=>{try{const f=JSON.parse(e.data);if(f.data?.authenticated)void api<number>('watch.unread').then(setUnread).catch(()=>setUnread(null));if(f.data?.authenticated===false||f.success===false&&f.error?.toLowerCase?.().includes('auth')){clearToken();setUnread(null);window.dispatchEvent(new Event('rats-auth-failed'))}if(f.event==='watchChanged'){window.dispatchEvent(new Event('watchChanged'));void api<number>('watch.unread').then(setUnread).catch(()=>setUnread(null))}}catch{}};socket.onclose=e=>{if(e.code===1008){clearToken();window.dispatchEvent(new Event('rats-auth-failed'))}setUnread(null);timer=setTimeout(connect,10000)}};connect();return()=>{dead=true;clearTimeout(timer);socket?.close()}},[]);const[authFailed,setAuthFailed]=useState(false);useEffect(()=>{const onAuthFailed=()=>setAuthFailed(true);const onAuthenticated=()=>setAuthFailed(false);window.addEventListener('rats-auth-failed',onAuthFailed);window.addEventListener('rats-authenticated',onAuthenticated);return()=>{window.removeEventListener('rats-auth-failed',onAuthFailed);window.removeEventListener('rats-authenticated',onAuthenticated)}},[]);return <div className="shell"><aside className={`sidebar ${menu?'open':''}`}><Link className="brand" to="/console"><img src="/assets/rat-avatar.png" alt="Rats Search"/><span><b>RATS SEARCH</b><small>DHT · P2P CONTROL</small></span></Link><label>WORKSPACE</label><nav>{nav.slice(0,6).map(n=><Link onClick={()=>setMenu(false)} className={loc.pathname===n[0]?'active':''} to={n[0]} key={n[0]}><i>{n[1]}</i>{n[2]}{n[0]==='/magnet-explorer'&&<span className="nav-incomplete">未完成</span>}{n[0]==='/watch'&&unread? <b className="unread">{unread} 未读</b>:null}</Link>)}</nav><label>SYSTEM</label><nav>{nav.slice(6).map(n=><Link onClick={()=>setMenu(false)} className={loc.pathname===n[0]?'active':''} to={n[0]} key={n[0]}><i>{n[1]}</i>{n[2]}</Link>)}</nav><footer><span className="dot"/> RATS SEARCH<small>LOCAL CONSOLE</small></footer></aside>{menu&&<div className="scrim" onClick={()=>setMenu(false)}/>}<div className="main"><header><button className="mobile menu" onClick={()=>setMenu(!menu)}>☰</button><div className="heading">{title&&<strong>{title}</strong>}<small>RATS SEARCH · LIVE SYSTEM</small></div><span className="spacer"/><span className="health">●</span><select aria-label="主题" value={theme} onChange={e=>setTheme(e.target.value)}><option value="tundra">◐ 深色</option><option value="classic">☼ 浅色</option></select><select aria-label="语言" value={language} onChange={e=>setLanguage(e.target.value)}><option value="zh">中文</option><option value="en">English</option><option value="ja">日本語</option><option value="de">Deutsch</option><option value="fr">Français</option></select><a href="https://github.com/lang2021/rats-search" target="_blank" rel="noreferrer" className="iconbtn" aria-label="项目链接">↗</a></header><main className="content">{authFailed&&<p className="error" role="alert">API 令牌无效，已清除；下次访问 API 时请重新连接。</p>}<Routes><Route path="/" element={<Navigate to="/search" replace/>}/><Route path="/search" element={<Search/>}/><Route path="/search/permalink/:hash" element={<LegacyDetailRedirect/>}/><Route path="/search/:hash" element={<LegacyDetailRedirect/>}/><Route path="/torrent/:hash" element={<Detail/>}/><Route path="/discover" element={<Browse mode="discover"/>}/><Route path="/history" element={<SearchHistoryPage/>}/><Route path="/watch" element={<Watch/>}/><Route path="/magnet-explorer" element={<MagnetExplorer api={api}/>}/><Route path="/console" element={<Dashboard/>}/><Route path="/console/network" element={<Network/>}/><Route path="/console/settings" element={<Settings/>}/><Route path="/console/index" element={<DataPage mode="index"/>}/><Route path="/console/downloads" element={<DataPage mode="downloads"/>}/><Route path="/console/events" element={<Trawl/>}/><Route path="/dashboard/*" element={<Navigate to="/console" replace/>}/><Route path="/torrents/*" element={<Navigate to="/search" replace/>}/><Route path="*" element={<section className="panel pad">页面不存在</section>}/></Routes></main></div></div>}
function LegacyDetailRedirect(){const {hash=''}=useParams();return <Navigate to={`/torrent/${hash}`} replace/>}

function PageTitle({title,sub,action}:{title:string;sub?:string;action?:ReactNode}){return <div className="page-title"><div><h1>{title}</h1>{sub&&<p>{sub}</p>}</div>{action}</div>}
type SearchMode = 'local' | 'peers';
type MatchSource = 'all' | 'content' | 'filename';
type SearchOptions = { query: string; mode: SearchMode; type: string; source: MatchSource; sort: string; descending: boolean; page: number; recordHistory?: boolean };
const searchTypes = [['', '全部'], ['video', '视频'], ['audio', '音频'], ['books', '书籍'], ['pictures', '图片'], ['software', '软件'], ['games', '游戏'], ['archive', '压缩包'], ['other', '其他']] as const;
const searchTypeIcons:Record<string,string> = {
  '': 'M4 5h16v3H4zm0 6h16v3H4zm0 6h16v3H4z',
  video: 'M4 6h11v12H4zM15 10l5-3v10l-5-3z',
  audio: 'M9 18V5l10-2v13M9 18a3 2 0 1 1-3-2 3 2 0 0 1 3 2zm10-2a3 2 0 1 1-3-2 3 2 0 0 1 3 2z',
  books: 'M4 4h5v16H4zm6 0h5v16h-5zm6 2h4v14h-4z',
  pictures: 'M4 5h16v14H4zm1 12 4-4 3 3 3-5 4 6',
  software: 'M5 4h14v16H5zM9 8h6M9 12h6M9 16h3',
  games: 'M7 9h10a4 4 0 0 1 3.8 5.2l-1 3a2 2 0 0 1-3.2 1l-2.1-1.7h-5L7.4 18a2 2 0 0 1-3.2-1l-1-3A4 4 0 0 1 7 9zm1 2v4m-2-2h4m5-1h.01M17 14h.01',
  archive: 'M4 5h16v4H4zm2 4v10h12V9m-7 3h2m-2 3h2',
  other: 'M4 5h16v14H4zm4 4h8m-8 3h8m-8 3h5'
};
const matchSources: [MatchSource, string, string][] = [
  ['all', '全部结果', 'M3 12h18M3 6h18M3 18h18'],
  ['content', '内容匹配', 'M4 4h6l2 2h8v14H4zM8 11h8M8 14h6'],
  ['filename', '文件名匹配', 'M4 5h16v14H4zM8 9h8M8 12h6M8 15h4'],
];
const searchSortOptions = [['relevance', '相关度'], ['added', '最近加入'], ['size', '大小'], ['seeders', '做种数'], ['name', '名称']] as const;
function SortControls({sort,descending,onSortChange,onDirectionChange,label='排序方式'}:{sort:string;descending:boolean;onSortChange:(value:string)=>void;onDirectionChange:()=>void;label?:string}){
  const sortLabel=searchSortOptions.find(([value])=>value===sort)?.[1]||'相关度';
  return <div className="search-sort">
    <details className="search-sort-picker">
      <summary aria-label={`${label}：${sortLabel}`} data-tooltip={`排序：${sortLabel}`}>
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 6h10M4 12h7M4 18h4M18 5v14m0 0-3-3m3 3 3-3"/></svg>
      </summary>
      <div className="search-sort-options" role="group" aria-label={label}>
        {searchSortOptions.map(([value,optionLabel])=><button key={value} type="button" className={sort===value?'selected':''}
          aria-pressed={sort===value} onClick={event=>{onSortChange(value);event.currentTarget.closest('details')?.removeAttribute('open')}}>
          <span>{optionLabel}</span>{sort===value&&<svg viewBox="0 0 16 16" aria-hidden="true"><path d="m3 8 3 3 7-7"/></svg>}
        </button>)}
      </div>
    </details>
    <button type="button" className="sort-direction-button" aria-label={descending?'当前降序，切换为升序':'当前升序，切换为降序'}
      data-tooltip={descending?'降序':'升序'} onClick={onDirectionChange}>
      <svg viewBox="0 0 24 24" aria-hidden="true"><path d={descending?'M12 5v14m0 0-5-5m5 5 5-5':'M12 19V5m0 0-5 5m5-5 5 5'}/></svg>
    </button>
  </div>;
}
const hashKey = (item: Torrent) => (item.hash || '').toLowerCase();
function sortSearchResults(items:Torrent[], sort:string, descending:boolean){
  if(sort==='relevance')return items;
  const timestamp=(value:unknown)=>{const number=Number(value||0);return number&&number<1e12?number*1000:number||Date.parse(String(value||''))||0};
  const compare=(a:Torrent,b:Torrent)=>{
    if(sort==='name')return String(a.name||'').localeCompare(String(b.name||''),'zh-CN',{numeric:true,sensitivity:'base'});
    if(sort==='size')return Number(a.size||0)-Number(b.size||0);
    if(sort==='seeders')return Number(a.seeders||0)-Number(b.seeders||0);
    if(sort==='added')return timestamp(a.added)-timestamp(b.added);
    return 0;
  };
  return [...items].sort((a,b)=>{const order=compare(a,b);return descending?-order:order});
}

function Search(){
  useTitle('资源搜索');
  const [params, setParams] = useSearchParams();
  const initialQuery = params.get('query') || '';
  const initialPage = Math.max(1, Number(params.get('page')) || 1);
  const [draft, setDraft] = useState(initialQuery);
  const [searchHistory, setSearchHistory] = useState<{query:string; count:number; lastSearchedAt:number}[]>([]);
  const [showSuggestions, setShowSuggestions] = useState(false);
  const [suggestionIndex, setSuggestionIndex] = useState(-1);
  const historyLoaded = useRef(false);
  const suggestionBlur = useRef<number|undefined>(undefined);
  const [committed, setCommitted] = useState(initialQuery);
  const [mode, setMode] = useState<SearchMode>('local');
  const [type, setType] = useState('');
  const [matchSource, setMatchSource] = useState<MatchSource>('all');
  const [sort, setSort] = useState('relevance');
  const [descending, setDescending] = useState(true);
  const [availableTypes, setAvailableTypes] = useState<string[]>([]);
  const [typeCounts, setTypeCounts] = useState<Record<string, number>|null>(null);
  const [typeCountsError, setTypeCountsError] = useState('');
  const [page, setPage] = useState(initialPage);
  const [totalResults, setTotalResults] = useState<number|null>(null);
  const [items, setItems] = useState<Torrent[]>([]);
  const [remote, setRemote] = useState<Torrent[]>([]);
  const [candidates, setCandidates] = useState<Torrent[]>([]);
  const [hasMore, setHasMore] = useState(false);
  const [loading, setLoading] = useState(false);
  const [remoteLoading, setRemoteLoading] = useState(false);
  const [error, setError] = useState('');
  const [remoteStatus, setRemoteStatus] = useState('');
  const [filtersPending, setFiltersPending] = useState(false);
  const version = useRef(0);
  const initialRun = useRef(false);
  const localAbort = useRef<AbortController|null>(null);
  const remoteSession = useRef<{id:string; controller:AbortController; socket:WebSocket}|null>(null);
  const nav = useNavigate();

  useEffect(() => {
    let active = true;
    void api<Record<string, number>>('stats.contentTypes').then(counts => {
      if (!active) return;
      setAvailableTypes(searchTypes.slice(1).map(([value]) => value).filter(value => (counts[value] || 0) > 0));
    }).catch(async () => {
      if (!active) return;
      const results = await Promise.allSettled(searchTypes.slice(1).map(async ([value]) => {
        const rows = await api<Torrent[]>('search.browse', {type:value, limit:1, safeSearch:false});
        return rows.length > 0 ? value : '';
      }));
      if (!active) return;
      setAvailableTypes(results.flatMap(result => result.status === 'fulfilled' && result.value ? [result.value] : []));
    });
    return () => { active = false; };
  }, []);

  useEffect(() => {
    if (type && availableTypes.length && !availableTypes.includes(type)) setType('');
  }, [availableTypes, type]);

  const stopRemote = useCallback((showCancelled = false) => {
    const old = remoteSession.current;
    remoteSession.current = null;
    old?.controller.abort();
    old?.socket.close();
    if (old) void api('search.cancel', {searchId:old.id}).catch(()=>{});
    setRemoteLoading(false);
    if (showCancelled) setRemoteStatus('已取消 Rats 节点查询；已有结果仍可查看。');
  }, []);

  const mergeRemote = useCallback((incoming: Torrent[], candidate: boolean, query: string) => {
    if (!Array.isArray(incoming)) return;
    const update = candidate ? setCandidates : setRemote;
    update(previous => {
      const byHash = new Map(previous.map(item => [hashKey(item), item]));
      for (const item of incoming) {
        if (!/^[0-9a-f]{40}$/i.test(item.hash || '') || !item.name || !item.peer) continue;
        if (candidate) {
          const text = [item.name, ...(item.matchingPaths || [])].join(' ').toLowerCase();
          if (!query.toLowerCase().split(/\s+/).filter(Boolean).every(word => text.includes(word))) continue;
        }
        const key = hashKey(item);
        byHash.set(key, {...byHash.get(key), ...item, remote:true});
      }
      return [...byHash.values()].slice(0, 100);
    });
  }, []);

  const startRemote = async (options: SearchOptions, currentVersion: number, safeSearch: boolean) => {
    if (options.query.length < 3) { setRemoteStatus('Rats 节点查询至少需要 3 个字符。'); return; }
    const id = crypto.randomUUID();
    const controller = new AbortController();
    const socket = new WebSocket(wsUrl());
    remoteSession.current = {id, controller, socket};
    setRemoteLoading(true);
    setRemoteStatus('正在连接 Rats 节点搜索…');
    const active = () => version.current === currentVersion && remoteSession.current?.id === id;
    try {
      await new Promise<void>((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('WebSocket 鉴权等待超时')), 4000);
        const finish = (error?:Error) => { clearTimeout(timer); error ? reject(error) : resolve(); };
        socket.onopen = () => socket.send(JSON.stringify({method:'auth',params:{token:token()}}));
        socket.onmessage = event => {
          try {
            const frame = JSON.parse(event.data);
            if (frame.data?.authenticated) { finish(); return; }
            if (frame.data?.authenticated === false || frame.success === false && String(frame.error).toLowerCase().includes('auth')) {
              clearToken(); window.dispatchEvent(new Event('rats-auth-failed'));
              finish(new Error('WebSocket 鉴权失败，请重新连接')); return;
            }
            if (!active()) return;
            if (frame.event === 'remoteSearchResults' && frame.data?.searchId === id)
              mergeRemote(frame.data.torrents || [], false, options.query);
            if (frame.event === 'remoteSearchCandidates' && frame.data?.candidateSessionId === id)
              mergeRemote(frame.data.torrents || [], true, options.query);
          } catch { /* Ignore malformed event frames. */ }
        };
        socket.onerror = () => finish(new Error('WebSocket 连接失败'));
        socket.onclose = event => {
          if (event.code === 1008) { clearToken(); window.dispatchEvent(new Event('rats-auth-failed')); }
          finish(new Error('WebSocket 已断开'));
        };
        controller.signal.addEventListener('abort', () => finish(new Error('已取消')), {once:true});
      });
      if (!active()) return;
      setRemoteStatus('正在查询已连接的 Rats 节点…');
      const result = await api<{items:Torrent[]; candidates:Torrent[]; peers:number; windowExpired:boolean}>(
        'search.remote', {searchId:id, query:options.query, limit:50, sort:options.sort,
          orderDesc:options.descending, safeSearch, contentType:options.type || undefined}, controller.signal);
      if (!active()) return;
      mergeRemote(result.items || [], false, options.query);
      mergeRemote(result.candidates || [], true, options.query);
      setRemoteStatus(`查询窗口已结束 · 已查询 ${result.peers} 个已连接节点。尚未收到结果也不代表网络中没有该资源。`);
    } catch (cause) {
      if (active()) setRemoteStatus(`Rats 节点查询失败：${String(cause)}`);
    } finally {
      if (active()) { remoteSession.current = null; setRemoteLoading(false); }
      socket.close();
    }
  };

  const run = async (next: SearchOptions) => {
    const options = {...next, query:next.query.trim(), page:Math.max(1, next.page)};
    const currentVersion = ++version.current;
    localAbort.current?.abort();
    stopRemote();
    setDraft(options.query); setCommitted(options.query); setMode(options.mode); setType(options.type);
    setMatchSource(options.source); setSort(options.sort); setDescending(options.descending); setPage(options.page);
    setItems([]); setRemote([]); setCandidates([]); setHasMore(false); setTotalResults(null);
    setTypeCounts(null); setTypeCountsError(''); setError(''); setRemoteStatus(''); setFiltersPending(false);
    setParams(options.query ? {query:options.query, page:String(options.page)} : options.page > 1 ? {page:String(options.page)} : {});
    const controller = new AbortController();
    localAbort.current = controller;
    setLoading(true);
    try {
      const config = await api<{safeSearch:boolean}>('config.get', {}, controller.signal);
      if (version.current !== currentVersion) return;
      const common = {query:options.query, sort:options.sort, orderDesc:options.descending,
        type:options.type || undefined, safeSearch:config.safeSearch, limit:20, offset:(options.page-1)*20,
        recordHistory:options.recordHistory === true};
      let result:{items:Torrent[]; counts:Record<string,number>|null; total:number|null; hasMore?:boolean};
      let legacyBackend = false;
      let legacyPathError = '';
      try {
        result = await api<{items:Torrent[]; counts:Record<string,number>; total:number}>('search.local',
          {...common, source:options.source}, controller.signal);
      } catch (cause) {
        if (!String(cause).includes('Unknown method: search.local')) throw cause;
        // The live preview may still run an older backend. Keep its search
        // usable, but never present all-index counts as query-specific counts.
        const limit = 21;
        const names = options.query && options.source !== 'content'
          ? await api<Torrent[]>('search.torrents', {...common, localOnly:true, limit}, controller.signal) : [];
        let paths:Torrent[] = [];
        if (options.query.length >= 2 && options.source !== 'filename') {
          try { paths = await api<Torrent[]>('search.files', {...common, limit}, controller.signal); }
          catch (fileCause) { legacyPathError = `；旧后端文件路径查询失败：${String(fileCause)}`; }
        }
        const browsed = !options.query
          ? await api<Torrent[]>('search.browse', {...common, limit}, controller.signal) : [];
        const words = options.query.toLowerCase().split(/\s+/).filter(Boolean);
        const titles = names.filter(item => /^[0-9a-f]{40}$/i.test(options.query)
          ? options.source === 'all' && hashKey(item) === options.query.toLowerCase()
          : words.every(word => item.name.toLowerCase().includes(word)));
        const byHash = new Map([...titles, ...browsed].map(item => [hashKey(item), item]));
        for (const item of paths) if (!byHash.has(hashKey(item))) byHash.set(hashKey(item), item);
        const legacy = sortSearchResults([...byHash.values()], options.sort, options.descending);
        result = {items:legacy.slice(0,20), counts:null, total:null,
          hasMore:names.length > 20 || paths.length > 20 || browsed.length > 20};
        legacyBackend = true;
      }
      if (version.current !== currentVersion) return;
      setItems(result.items || []);
      setTypeCounts(result.counts);
      setTotalResults(result.total);
      if (options.query) historyLoaded.current = false;
      if (legacyBackend) setTypeCountsError(`当前后端尚未提供搜索分类数量${legacyPathError}；请更新后端。`);
      setHasMore(result.total!==null ? options.page*20<result.total : !!result.hasMore);
      if (options.query && options.mode === 'peers')
        void startRemote(options, currentVersion, config.safeSearch);
    } catch (cause) {
      if (version.current === currentVersion) {
        setError(`本地搜索失败：${String(cause)}`);
        setTotalResults(null);
        setTypeCounts(null);
        setTypeCountsError('分类数量不可用');
      }
    } finally {
      if (version.current === currentVersion) { setLoading(false); localAbort.current = null; }
    }
  };

  useEffect(() => {
    if (initialRun.current) return;
    initialRun.current = true;
    void run({query:initialQuery, mode:'local', type:'', source:'all', sort:'relevance', descending:true, page:initialPage,
      recordHistory:params.get('recordHistory') === '1'});
    return () => { initialRun.current = false; };
  }, []);
  useEffect(() => () => {
    version.current++;
    localAbort.current?.abort();
    const old = remoteSession.current;
    old?.controller.abort(); old?.socket.close();
    if (old) void api('search.cancel', {searchId:old.id}).catch(()=>{});
  }, []);

  const current = {query:committed, mode, type, source:matchSource, sort, descending, page};
  const localKeys = new Set(items.map(hashKey));
  const visibleLocal = items;
  const matchesSelectedSource = (item:Torrent) => {
    if (matchSource === 'all') return true;
    const words = committed.toLowerCase().split(/\s+/).filter(Boolean);
    const containsWords = (value:string) => words.length > 0 && words.every(word => value.toLowerCase().includes(word));
    if (matchSource === 'filename') return containsWords(item.name || '');
    return !!(item.fileMatch || item.isFileMatch || item.matchingPaths?.some(containsWords));
  };
  const visibleRemote = sortSearchResults(remote.filter(item => !localKeys.has(hashKey(item)) && matchesSelectedSource(item)),sort,descending);
  const candidateKeys = new Set([...localKeys, ...remote.map(hashKey)]);
  const visibleCandidates = matchSource === 'all' ? sortSearchResults(candidates.filter(item => !candidateKeys.has(hashKey(item))),sort,descending) : [];
  const showPagination = !committed || items.length > 0 || hasMore || page > 1;
  const totalPages = totalResults===null ? null : Math.max(1,Math.ceil(totalResults/20));
  const clear = () => { setDraft(''); void run({...current, query:'', source:'all', page:1}); };
  const changeFilter = (changes:Partial<SearchOptions>) => {
    const next = {...current, ...changes, page:1};
    if (draft.trim() === committed) void run(next);
    else { if (changes.type !== undefined) setType(changes.type); if (changes.sort !== undefined) setSort(changes.sort);
      if (changes.descending !== undefined) setDescending(changes.descending);
      if (changes.source !== undefined) setMatchSource(changes.source);
      if (changes.mode !== undefined) { setMode(changes.mode); stopRemote(); setRemote([]); setCandidates([]); setRemoteStatus(''); }
      setFiltersPending(true); }
  };
  const openRemote = async (item:Torrent) => {
    try { await api('torrent.get', {hash:item.hash, peer:item.peer, files:true}); nav(`/torrent/${item.hash}`); }
    catch (cause) { setRemoteStatus(`无法读取节点资源详情：${String(cause)}`); }
  };
  const chooseMode = (next:SearchMode) => {
    changeFilter({mode:next});
    const picker = document.querySelector<HTMLDetailsElement>('.search-mode-picker');
    if (picker) picker.open = false;
  };
  const loadSearchHistory = async () => {
    if (historyLoaded.current) return;
    try {
      const rows = await api<{query:string; count:number; lastSearchedAt:number}[]>('search.history', {limit:30});
      historyLoaded.current = true;
      setSearchHistory(Array.isArray(rows) ? rows : []);
    } catch { /* Suggestions stay empty when history is disabled or unavailable. */ }
  };
  const suggestions = searchHistory.filter(entry => !draft.trim()
    || entry.query.toLocaleLowerCase().includes(draft.trim().toLocaleLowerCase())).slice(0,8);
  const submitSuggestion = (query:string) => {
    setShowSuggestions(false);
    setSuggestionIndex(-1);
    void run({...current, query, page:1, recordHistory:true});
  };
  const handleSearchKeyDown = (event:KeyboardEvent<HTMLInputElement>) => {
    if (event.key === 'Escape') { setShowSuggestions(false); setSuggestionIndex(-1); return; }
    if (!showSuggestions || suggestions.length === 0) return;
    if (event.key === 'ArrowDown') {
      event.preventDefault(); setSuggestionIndex(index => (index + 1) % suggestions.length);
    } else if (event.key === 'ArrowUp') {
      event.preventDefault(); setSuggestionIndex(index => index <= 0 ? suggestions.length - 1 : index - 1);
    } else if (event.key === 'Enter' && suggestionIndex >= 0) {
      event.preventDefault(); submitSuggestion(suggestions[suggestionIndex].query);
    }
  };
  const modeLabel = mode === 'local' ? '本地索引' : 'Rats 网络';
  const matchSourceLabel = matchSources.find(([value]) => value === matchSource)?.[1] || '全部结果';
  const selectedTypeLabel = searchTypes.find(([value]) => value === type)?.[1] || '全部';

  return <div className="search-page-layout"><section className="search-workspace">
      <form className="search-main" onSubmit={(event:FormEvent) => {event.preventDefault(); setShowSuggestions(false); void run({...current, query:draft, page:1, recordHistory:true});}}>
        <details className="search-mode-picker">
          <summary aria-label={`搜索模式：${modeLabel}，点击切换`}>
            <svg viewBox="0 0 24 24" aria-hidden="true"><path d={mode === 'local' ? 'M4 5.5 12 2l8 3.5-8 3.5-8-3.5Zm0 5L12 14l8-3.5M4 15.5 12 19l8-3.5' : 'M12 3v5m0 8v5M4.2 7.5l4.4 2.6m6.8 3.8 4.4 2.6M4.2 16.5l4.4-2.6m6.8-3.8 4.4-2.6'} stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" fill="none"/><circle cx="12" cy="12" r="2.3" fill="currentColor"/></svg>
            <span>{modeLabel}</span><svg className="chevron" viewBox="0 0 16 16" aria-hidden="true"><path d="m4 6 4 4 4-4"/></svg>
          </summary>
          <div className="search-mode-options" role="group" aria-label="选择搜索模式">
            <button type="button" className={mode === 'local' ? 'selected' : ''} onClick={() => chooseMode('local')}><b>本地索引</b></button>
            <button type="button" className={mode === 'peers' ? 'selected' : ''} onClick={() => chooseMode('peers')}><b>扩展至 Rats 网络</b><small>搜索已连接的 Rats 节点</small></button>
          </div>
        </details>
        <div className="search-input-wrap">
          <input aria-label="搜索关键词" role="combobox" aria-autocomplete="list" aria-expanded={showSuggestions && suggestions.length > 0}
            aria-controls="search-history-suggestions" aria-activedescendant={suggestionIndex >= 0 ? `search-history-${suggestionIndex}` : undefined}
            value={draft} onFocus={() => {if (suggestionBlur.current) window.clearTimeout(suggestionBlur.current); setShowSuggestions(true); void loadSearchHistory();}}
            onBlur={() => {suggestionBlur.current = window.setTimeout(() => {setShowSuggestions(false); setSuggestionIndex(-1);}, 120);}}
            onKeyDown={handleSearchKeyDown}
            onChange={event => {setDraft(event.target.value); setSuggestionIndex(-1); setShowSuggestions(true); if (remoteSession.current) stopRemote(true);}}
            placeholder="输入资源名称、文件名或 InfoHash"/>
          {showSuggestions && suggestions.length > 0 && <div id="search-history-suggestions" className="search-history-suggestions" role="listbox" aria-label="最近搜索">
            <div className="search-history-suggestions-heading">最近搜索</div>
            {suggestions.map((entry,index) => <button id={`search-history-${index}`} key={entry.query} type="button" role="option"
              aria-selected={suggestionIndex === index} className={suggestionIndex === index ? 'active' : ''}
              onMouseDown={event => event.preventDefault()} onClick={() => submitSuggestion(entry.query)}>
              <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 7v5l3 2M20 12a8 8 0 1 1-2.35-5.65M20 4v5h-5"/></svg>
              <span>{entry.query}</span><small>{entry.count} 次</small>
            </button>)}
          </div>}
        </div>
        {draft && <button type="button" onClick={clear}>清空</button>}
        <button className="primary" type="submit" disabled={!draft.trim()}>搜索</button>
      </form>
      <div className="search-controls">
        <div className="search-category-picker" role="group" aria-label="资源类型筛选">
          {searchTypes.filter(([value]) => !value || availableTypes.includes(value) || type === value).map(([value,label]) => <button key={value} type="button"
            className={`search-category-button ${type === value ? 'selected' : ''}`} aria-label={`${label}${typeof (value ? typeCounts?.[value] : typeCounts?.total) === 'number' ? `，${value ? typeCounts?.[value] : typeCounts?.total} 个` : ''}`}
            aria-pressed={type === value} data-tooltip={label}
            onClick={() => changeFilter({type:value})}>
            <svg viewBox="0 0 24 24" aria-hidden="true"><path d={searchTypeIcons[value]} /></svg>
            {typeof (value ? typeCounts?.[value] : typeCounts?.total) === 'number' && <span><CompactNumber value={value ? typeCounts?.[value] : typeCounts?.total}/></span>}
          </button>)}
          {typeCountsError&&<small className="search-category-error" role="status">{typeCountsError}</small>}
        </div>
        <div className="search-controls-actions">
          <details className="search-filter-picker">
            <summary aria-label={`筛选匹配来源，当前：${matchSourceLabel}`} data-tooltip={`筛选：${matchSourceLabel}`} data-active={matchSource !== 'all' ? 'true' : 'false'}>
              <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3.5 5h17l-6.5 7v5l-4 2v-7L3.5 5Z" /></svg>
            </summary>
            <div className="search-match-filter-options" role="group" aria-label="匹配来源筛选">
              {matchSources.map(([value,label,path]) => <button key={value} type="button"
                className={matchSource === value ? 'selected' : ''} aria-label={label} aria-pressed={matchSource === value}
                disabled={value !== 'all' && !committed}
                onClick={event => { changeFilter({source:value}); const picker = event.currentTarget.closest('details'); if (picker) picker.open = false; }}>
                <svg viewBox="0 0 24 24" aria-hidden="true"><path d={path} /></svg>
                <span>{label}</span>{matchSource === value&&<svg className="match-filter-check" viewBox="0 0 16 16" aria-hidden="true"><path d="m3 8 3 3 7-7"/></svg>}
              </button>)}
            </div>
          </details>
          <SortControls sort={sort} descending={descending} onSortChange={value=>changeFilter({sort:value})}
          onDirectionChange={()=>changeFilter({descending:!descending})}/></div>
      </div>
    </section>
    <div className="search-results-area">
    <div className="search-results-scroll">
      {(draft.trim() !== committed || filtersPending) && <p className="search-pending">输入或筛选尚未提交；以下仍是上一次搜索的结果。点击“搜索”应用当前条件。</p>}
      {loading && <p className="search-state" role="status">正在读取本地索引…</p>}
      {error && <p className="error" role="alert">{error} <button onClick={() => void run(current)}>重试</button></p>}
      {!loading && !error && !visibleLocal.length && <p className="search-state">{committed ? matchSource === 'filename' ? '没有主标题匹配的资源。' : matchSource === 'content' ? '没有内部文件路径匹配的资源。' : '本地索引中没有符合条件的资源。' : '本地索引暂无资源。'}</p>}
      {visibleLocal.map(item => <TorrentRow key={item.hash} item={item} open={() => nav(`/torrent/${item.hash}`)}/>)}
      {mode === 'peers' && <section className="search-remote">
        <div className="remote-search-heading"><h2>Rats 节点结果</h2>{remoteLoading && <button onClick={() => stopRemote(true)}>取消远端搜索</button>}</div>
        <p role="status">{remoteStatus || '提交关键词后查询已连接的 Rats 节点。'}</p>
        <p>节点可能不支持分类及安全搜索过滤；旧协议候选需要自行核实。</p>
        {visibleRemote.length > 0 && <><h3>已确认属于本次搜索</h3>{visibleRemote.map(item =>
          <TorrentRow key={item.hash} item={item} source="Rats 节点" open={() => void openRemote(item)}/>)}</>}
        {visibleCandidates.length > 0 && <><h3>旧协议候选（需自行核实）</h3>{visibleCandidates.map(item =>
          <TorrentRow key={item.hash} item={item} source="旧协议候选" open={() => void openRemote(item)}/>)}</>}
      </section>}
    </div>
    {showPagination && <div className="search-page-controls unified-search-pagination" aria-label="搜索结果分页">
      <button disabled={loading || page <= 1} onClick={() => void run({...current, page:page-1})}>上一页</button>
      <span>{totalPages!==null ? `第 ${page} 页 / 共 ${totalPages} 页` : `第 ${page} 页 / 总页数未知`}</span>
      <button disabled={loading || !hasMore} onClick={() => void run({...current, page:page+1})}>下一页</button>
    </div>}
    </div>
  </div>;
}
function TorrentRow({item:t,open,source}:{item:Torrent;open:()=>void;source?:string}){return <article className="torrent-row"><div><button className="linktitle" onClick={open}>{t.name||'未命名资源'}</button><small>{source||'本地索引'} · {t.contentType||'类型未知'} · <CompactNumber value={t.files||0}/> 个文件 · {t.hash?.slice(0,12)}…</small>{t.matchingPaths?.length ? <small className="search-matched-path">命中路径：{t.matchingPaths[0]}</small>:null}</div><b>{size(t.size)}</b><button onClick={open}>查看详情</button><a href={`magnet:?xt=urn:btih:${t.hash}`}>磁力链接</a></article>}
function size(n=0){if(!n)return '—';const u=['B','KB','MB','GB','TB'];const i=Math.min(Math.floor(Math.log(n)/Math.log(1024)),4);return `${(n/1024**i).toFixed(i?1:0)} ${u[i]}`}
function databaseSize(value:unknown){return typeof value==='number'&&Number.isFinite(value)&&value>=0?(value===0?'0 B':size(value)):'暂不可用'}
function Detail(){const {hash=''}=useParams();const location=useLocation();const navigate=useNavigate();useTitle('资源详情');const [t,setT]=useState<Torrent>();const [err,setErr]=useState('');const back=()=>{if(location.key==='default')navigate('/search');else navigate(-1)};useEffect(()=>{api<Torrent>('torrent.get',{hash,files:true}).then(setT).catch(e=>setErr(String(e)))},[hash]);return <><PageTitle title="资源详情" sub={hash} action={<button className="detail-back" onClick={back}><svg viewBox="0 0 24 24" aria-hidden="true"><path d="m14.5 5-7 7 7 7M8 12h12"/></svg>返回</button>}/>{err&&<p className="error">{err}</p>}{t&&<section className="panel pad"><h2>{t.name}</h2><p>InfoHash：{t.hash}</p><p>大小：{size(t.size)} · 文件：<CompactNumber value={t.files}/></p><a href={`magnet:?xt=urn:btih:${t.hash}`}>磁力链接</a><h3>文件列表</h3>{(t.files_list||[]).map((f,i)=><div className="data-row" key={`${f.path}-${i}`}><span>{f.path}</span><b>{size(f.size)}</b></div>)}</section>}</>}
function Browse({mode}:{mode:'discover'|'history'}){
  const title=mode==='history'?'搜索历史':'发现资源';useTitle(title);
  const [list,setList]=useState<any[]>([]);const [err,setErr]=useState('');const [loading,setLoading]=useState(true);const nav=useNavigate();
  const [sort,setSort]=useState('relevance');const [descending,setDescending]=useState(true);
  const load=useCallback(async(kind=mode==='history'?'search.history':'search.top')=>{setLoading(true);setErr('');try{setList(await api<any[]>(kind,{limit:100})||[])}catch(e){setErr(String(e))}finally{setLoading(false)}},[mode]);useEffect(()=>{void load()},[load]);
  const visibleList=useMemo(()=>{
    if(mode!=='discover')return list;
    if(sort==='relevance')return descending?list:[...list].reverse();
    const compare=(a:any,b:any)=>{
      if(sort==='name')return String(a.name||'').localeCompare(String(b.name||''),'zh-CN',{numeric:true,sensitivity:'base'});
      if(sort==='added'){
        const time=(value:any)=>{if(typeof value==='string'&&Number.isNaN(Number(value)))return Date.parse(value)||0;const n=Number(value||0);return n&&n<1e12?n*1000:n};
        return time(a.added)-time(b.added);
      }
      const key=sort==='size'?'size':'seeders';return Number(a[key]||0)-Number(b[key]||0);
    };
    return [...list].sort((a,b)=>descending?-compare(a,b):compare(a,b));
  },[list,mode,sort,descending]);
  const date=(value:any)=>{const n=Number(value||0);return n?new Intl.DateTimeFormat('zh-CN',{year:'numeric',month:'2-digit',day:'2-digit'}).format(new Date(n<1e12?n*1000:n)):'—'};
  const open=(x:any)=>{if(mode==='history')nav(`/search?query=${encodeURIComponent(String(x.query||''))}`);else if(x.hash)nav(`/torrent/${x.hash}`)};
  return <div className={mode==='discover'?'discover-page':'history-page'}>{mode==='history'?<div className="toolbar history-toolbar"><button disabled={loading} onClick={()=>void load()}>刷新</button></div>:<div className="toolbar discover-toolbar"><div className="discover-tabs"><button className="discover-tab" onClick={()=>void load('search.top')}><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 2.8c.5 3.2-2.8 4.3-2.8 7.1 0 1.2.7 2.1 1.7 2.5-.2-1.9 1.1-3.1 2.1-4.1 2.1 2 3.8 4.2 3.8 7.1a4.8 4.8 0 0 1-9.6 0c0-2.3 1-4.1 2.3-5.8C5.3 11.2 4 14 4 16.1a8 8 0 0 0 16 0c0-5-3.1-9.7-8-13.3Z"/></svg>热门</button><button className="discover-tab" onClick={()=>void load('search.recent')}><svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="8.5"/><path d="M12 7v5l3.5 2"/></svg>最近加入</button></div><div className="discover-actions"><SortControls sort={sort} descending={descending} label="发现资源排序方式" onSortChange={setSort} onDirectionChange={()=>setDescending(value=>!value)}/><button disabled={loading} onClick={()=>void load()}>刷新</button></div></div>}{!loading&&!err&&<div className="browse-count">{mode==='history'?<>搜索词 <b>{list.length}</b> 条</>:<>资源 <b>{list.length}</b> 条</>}</div>}{err&&<p className="error" role="alert">{err}</p>}{loading&&<p>正在读取…</p>}{!loading&&!err&&!list.length&&<p className="empty">暂无数据。</p>}{!!list.length&&<div className={`table-wrap browse-list${mode==='discover'?' discover-list':' history-list'}`}><table><thead><tr><th>资源 / 搜索词</th><th>大小</th><th>{mode==='history'?'搜索次数':'做种数'}</th><th>{mode==='history'?'最后搜索日期':'加入日期'}</th><th>操作</th></tr></thead><tbody>{visibleList.map((x,i)=><tr key={x.hash||x.query||i}><td>{x.name||x.query||x.title||'未知'}{x.hash&&<small className="block">{x.hash}</small>}</td><td>{size(Number(x.size||0))}</td><td><CompactNumber value={Number(mode==='history'?x.count:x.seeders||0)}/>{mode==='history'?' 次':' 个'}</td><td>{date(mode==='history'?x.lastSearchedAt:x.added)}</td><td><button onClick={()=>open(x)}>{mode==='history'?'搜索':'查看详情'}</button></td></tr>)}</tbody></table></div>}</div>
}
type SearchHistoryEntry = {query:string; count:number; lastSearchedAt:number|string};
function SearchHistoryPage(){
  useTitle('搜索历史');
  const nav=useNavigate();
  const [entries,setEntries]=useState<SearchHistoryEntry[]>([]);
  const [filter,setFilter]=useState('');
  const [loading,setLoading]=useState(true);
  const [busy,setBusy]=useState(false);
  const [error,setError]=useState('');
  const [notice,setNotice]=useState('');
  const load=useCallback(async()=>{
    setLoading(true);setError('');
    try{setEntries(await api<SearchHistoryEntry[]>('search.history',{limit:100})||[])}
    catch(cause){setError(`读取搜索历史失败：${String(cause)}`)}
    finally{setLoading(false)}
  },[]);
  useEffect(()=>{void load()},[load]);
  const filtered=useMemo(()=>{
    const needle=filter.trim().toLocaleLowerCase();
    const matches=needle?entries.filter(entry=>entry.query.toLocaleLowerCase().includes(needle)):entries;
    const millis=(value:number|string)=>{
      const numeric=Number(value);
      if(Number.isFinite(numeric)&&numeric>0)return numeric<1e12?numeric*1000:numeric;
      const parsed=Date.parse(String(value||''));return Number.isFinite(parsed)?parsed:0;
    };
    return [...matches].sort((a,b)=>millis(b.lastSearchedAt)-millis(a.lastSearchedAt));
  },[entries,filter]);
  const timestamp=(value:number|string)=>{
    const numeric=Number(value);
    const dateValue=Number.isFinite(numeric)&&numeric>0?new Date(numeric<1e12?numeric*1000:numeric):new Date(String(value));
    return Number.isNaN(dateValue.getTime())?'时间未知':new Intl.DateTimeFormat('zh-CN',{
      year:'numeric',month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit',hour12:false
    }).format(dateValue);
  };
  const remove=async(entry:SearchHistoryEntry)=>{
    if(!window.confirm(`删除搜索词“${entry.query}”的历史记录？`))return;
    setBusy(true);setError('');setNotice('');
    try{const result=await api<{removed:boolean}>('search.history.remove',{query:entry.query});if(!result.removed){await load();setNotice('记录已不存在，列表已刷新。');return}setEntries(current=>current.filter(item=>item.query!==entry.query));setNotice('已删除这条搜索历史。')}
    catch(cause){setError(`删除失败：${String(cause)}`)}
    finally{setBusy(false)}
  };
  const clear=async()=>{
    if(!entries.length||!window.confirm(`将清空全部 ${entries.length} 条搜索历史，此操作不可撤销。确认清空？`))return;
    setBusy(true);setError('');setNotice('');
    try{const result=await api<{cleared:boolean}>('search.history.clear');if(!result.cleared){await load();setNotice('没有可清空的历史记录，列表已刷新。');return}setEntries([]);setFilter('');setNotice('搜索历史已清空。')}
    catch(cause){setError(`清空失败：${String(cause)}`)}
    finally{setBusy(false)}
  };
  const searchAgain=(entry:SearchHistoryEntry)=>nav(`/search?query=${encodeURIComponent(entry.query)}&recordHistory=1`);
  return <section className="history-screen">
    <div className="history-tools">
      <div className="history-summary"><b>{entries.length}</b><span>/ 100 条已保存</span>{filter.trim()&&<small>筛选到 {filtered.length} 条</small>}</div>
      <div className="history-tool-actions"><button type="button" onClick={()=>void load()} disabled={loading||busy}>刷新</button><button type="button" className="history-clear" onClick={()=>void clear()} disabled={!entries.length||busy}>清空历史</button></div>
    </div>
    <label className="history-search"><svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="10.8" cy="10.8" r="6.8"/><path d="m16 16 5 5"/></svg><input type="search" value={filter} onChange={event=>setFilter(event.target.value)} placeholder="筛选历史关键词" aria-label="筛选历史关键词"/><span>本地筛选</span></label>
    {error&&<p className="error" role="alert">{error}<button type="button" onClick={()=>void load()}>重试</button></p>}
    {notice&&!error&&<p className="history-notice" role="status">{notice}</p>}
    {loading?<p className="history-empty" role="status">正在读取搜索历史…</p>:!error&&!entries.length?<div className="history-empty"><b>还没有搜索记录</b><span>提交关键词搜索后，记录会显示在这里。</span></div>:!error&&!filtered.length?<div className="history-empty"><b>没有匹配的历史关键词</b><span>换一个关键词试试。</span></div>:!error&&<div className="history-rows" aria-label="搜索历史列表">
      {filtered.map(entry=><article className="history-row" key={entry.query}>
        <div className="history-query"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 7v5l3.5 2M20 12a8 8 0 1 1-2.35-5.65M20 4v5h-5"/></svg><strong title={entry.query}>{entry.query}</strong></div>
        <div className="history-meta"><time>{timestamp(entry.lastSearchedAt)}</time><span>搜索 <CompactNumber value={Number(entry.count)||0}/> 次</span></div>
        <div className="history-row-actions"><button type="button" className="history-run" disabled={busy} onClick={()=>searchAgain(entry)}><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M20 11a8 8 0 1 0 1 4M20 4v7h-7"/></svg>重新搜索</button><button type="button" className="history-remove" disabled={busy} aria-label={`删除 ${entry.query}`} onClick={()=>void remove(entry)}><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 7h16M10 11v6m4-6v6M6 7l1 13h10l1-13M9 7V4h6v3"/></svg></button></div>
      </article>)}
    </div>}
  </section>;
}
function Dashboard(){
  useTitle('系统总览');
  const [d,setD]=useState<any>({});
  const [error,setError]=useState('');
  const [mapRequestError,setMapRequestError]=useState('');
  const [events,setEvents]=useState<any[]>([]);
  const [lastUpdated,setLastUpdated]=useState<Date|null>(null);
  const [databaseAvailable,setDatabaseAvailable]=useState<boolean|undefined>(undefined);
  const refresh=useCallback(async()=>{
    const methods=[api('stats.get'),api('stats.p2pStatus'),api('database.status'),api('peers.list'),api('download.list'),ready(),api('features.status'),api('stats.crawler'),api('stats.networkMap')];
    const values=await Promise.allSettled(methods);
    const keys=['stats','p2p','sync','peers','downloads','ready','features','crawler','networkMap'];
    const next:any={};
    values.forEach((value,index)=>{if(value.status==='fulfilled')next[keys[index]]=value.value});
    next.ready=values[5].status==='fulfilled'&&values[5].value;
    setD((previous:any)=>({...previous,...next}));
    setDatabaseAvailable(values[0].status==='fulfilled');
    if(values[0].status==='fulfilled'&&values[1].status==='fulfilled')setLastUpdated(new Date());
    setError(values.slice(0,-1).some(value=>value.status==='rejected')?'部分状态暂不可用；已保留上次成功读取的数据。':'');
    setMapRequestError(values[8].status==='rejected'?'地图数据暂不可用，请检查地图接口。':'');
  },[]);
  useEffect(()=>{
    void refresh();
    const timer=setInterval(()=>void refresh(),15000);
    let dead=false;
    let socket:WebSocket|undefined;
    let retry:ReturnType<typeof setTimeout>;
    const connect=async()=>{
      const auth=await requestToken();
      if(dead||!auth)return;
      socket=new WebSocket(wsUrl());
      socket.onopen=()=>socket?.send(JSON.stringify({method:'auth',params:{token:auth}}));
      socket.onmessage=e=>{
        try{
          const frame=JSON.parse(e.data);
          if(frame.data?.authenticated)return;
          if(frame.data?.authenticated===false||frame.success===false&&frame.error?.toLowerCase?.().includes('auth')){
            clearToken();
            setError('API 令牌无效；令牌已清除，请重新连接。');
            return;
          }
          if(frame.event)setEvents(items=>[{time:new Date(frame.time||Date.now()).toLocaleTimeString(),event:frame.event,outcome:frame.data?.outcome,detail:frame.data?.name||frame.data?.hash||JSON.stringify(frame.data||{})},...items].slice(0,20));
        }catch{}
      };
      socket.onclose=event=>{
        if(event.code===1008){clearToken();window.dispatchEvent(new Event('rats-auth-failed'))}
        if(!dead)retry=setTimeout(connect,5000);
      };
    };
    void connect();
    return()=>{dead=true;clearInterval(timer);clearTimeout(retry);socket?.close()};
  },[refresh]);

  const sync=d.sync;
  const crawler=d.crawler;
  const features=d.features;
  const count=(value:unknown)=><CompactNumber value={typeof value==='number'?value:undefined}/>;
  const eventTitle=(event:any)=>event.event==='trawlQueued'?'线索入队':event.event==='trawlStarted'?'开始调查':event.event==='trawlMetadata'?(event.outcome==='received'?'取得元数据':event.outcome==='metadata timeout'?'元数据超时':event.outcome==='observationStopped'?'调查停止':'调查结束'):event.event==='torrentIndexed'?'资源入库':event.event==='torrentBackfilled'?'资料补齐':event.event==='trawlOmitted'?'部分线索省略':event.event;
  type Tone='ok'|'off'|'fault'|'unknown';
  const runtime=(state?:string):[string,Tone]=>state==='running'?['运行中','ok']:state==='failed'?['异常','fault']:state==='starting'?['启动中','unknown']:state==='stopping'?['停止中','unknown']:state==='stopped'?['已关闭','off']:['未知','unknown'];
  const booleanState=(value:unknown):[string,Tone]=>typeof value==='boolean'?(value?['运行中','ok']:['已关闭','off']):['未知','unknown'];
  const statuses:[string,string,Tone][]=[
    ['API',d.ready===true?'就绪':'不可用',d.ready===true?'ok':'fault'],
    ['数据库',databaseAvailable===undefined?'未知':databaseAvailable?'就绪':'不可用',databaseAvailable===undefined?'unknown':databaseAvailable?'ok':'fault'],
    ['DHT Socket',...booleanState(features?.dhtRunning??d.p2p?.dhtRunning)],
    ['Crawler',...runtime(features?.crawlerState)],
    ['Rats P2P',...booleanState(features?.p2pRunning??d.p2p?.running)],
    ['共享网络',...runtime(features?.networkState)],
  ];
  const metrics:[string,unknown,ReactNode][]=[
    ['本地索引资源',d.stats?.torrents,<>数据库占用 {databaseSize(d.stats?.databaseSizeBytes)}</>],
    ['索引文件数',d.stats?.files,`资源标称总大小 ${size(d.stats?.size)}`],
    ['Rats P2P 已连接节点',d.p2p?.peerCount,d.p2p?(d.p2p.running?'P2P 运行中':'P2P 已关闭'):'状态不可用'],
    ['DHT 路由节点',d.p2p?.dhtNodes,d.p2p?(d.p2p.dhtRunning?'DHT 运行中':'DHT 已关闭'):'状态不可用'],
  ];
  return <div className="overview overview-v2">
    <div className="metric-grid" aria-label="核心指标">{metrics.map(([label,value,detail])=><article key={label}><span>{label}</span><strong>{count(value)}</strong><small>{detail}</small></article>)}</div>

    <div className="overview-main">
      <section className="panel overview-map"><div className="panel-title"><h2>DHT / P2P 世界网络</h2><Link to="/console/network">网络详情 →</Link></div><Suspense fallback={<div className="network-map-loading">正在载入世界地图…</div>}><NetworkActivityMap snapshot={d.networkMap} crawlerActivity={d.crawler} requestError={mapRequestError} dhtTotal={d.p2p?.dhtNodes} peerTotal={d.p2p?.peerCount} running={d.p2p?.dhtRunning}/></Suspense></section>
      <section className="panel overview-collection" aria-label="采集概况"><div className="panel-title"><h2>采集概况</h2><span>实时状态 · 本进程累计</span></div>
        <div className="overview-phase"><h3>DHT 线索发现</h3>
          <div className="data-row"><span>BEP 51 请求</span><b>{count(crawler?.sampleRequests)}</b></div>
          <div className="data-row"><span>有效响应</span><b>{count(crawler?.sampleResponses)}</b></div>
          <div className="data-row"><span>Hash 样本（含重复）</span><b>{count(crawler?.sampleHashes)}</b></div>
          <div className="data-row"><span>Hash 入队（含重试及被动来源）</span><b>{count(crawler?.hashesEnqueued)}</b></div>
        </div>
        <div className="overview-phase"><h3>BEP 9 元数据获取</h3>
          <div className="data-row"><span>在途任务</span><b>{count(crawler?.activeFetches)}</b></div>
          <div className="data-row"><span>等待队列</span><b>{count(crawler?.queuedFetches)}</b></div>
          <div className="data-row"><span>成功 / 失败（累计）</span><b>{count(crawler?.fetchSuccesses)} / {count(crawler?.fetchErrors)}</b></div>
        </div>
        <div className="overview-phase"><h3>索引入库</h3>
          <div className="data-row"><span>新入库或文件补齐（合计）</span><b>{count(crawler?.indexedOrBackfilled)}</b></div>
          <p className="overview-phase-note">元数据成功不等于实际入库；失败数不含停采取消。</p>
        </div>
      </section>
    </div>

    <div className="overview-bottom">
      <section className="panel overview-events"><div className="panel-title"><h2>实时事件</h2><Link to="/console/events">查看全部 →</Link></div>{events.length?events.slice(0,5).map((event,index)=><div className="event-row" key={index}><time>{event.time}</time><b>{eventTitle(event)}</b><span title={event.detail}>{event.detail}</span></div>):<p className="empty">本次会话暂无事件；这里仅展示 WebSocket 实时消息。</p>}</section>
      <section className="panel overview-sync"><div className="panel-title"><h2>索引与同步</h2><Link to="/console/index">查看详情 →</Link></div>
        <div className="data-row"><span>当前操作</span><b>{sync?.operation??'—'}</b></div>
        <div className="data-row"><span>同步进度</span><b>{sync?`${sync.processed??'—'} / ${sync.total??'—'}`:'—'}</b></div>
        <div className="data-row"><span>快照状态</span><b>{sync?(sync.snapshot?.valid?'有效':'暂无有效快照'):'—'}</b></div>
        <div className="data-row"><span>数据库共享</span><b>{sync?(sync.sharing?'已启用':'未启用'):'—'}</b></div>
        <div className="data-row"><span>下载任务</span><b>{Array.isArray(d.downloads)?count(d.downloads.length):'—'}</b></div>
      </section>
    </div>

    <section className="overview-status" aria-label="系统状态">
      <div className="overview-status-heading"><strong>系统状态</strong><span>{lastUpdated?`更新于 ${lastUpdated.toLocaleTimeString('zh-CN')}`:'尚未取得最新数据'}</span></div>
      <div className="overview-status-items">{statuses.map(([label,value,tone])=><div className={`overview-status-item is-${tone}`} key={label}><span className="overview-status-dot" aria-hidden="true"/><span>{label}</span><b>{value}</b></div>)}</div>
      {error&&<p className="overview-status-error" role="alert">{error}</p>}
    </section>
  </div>;
}
const countFields=new Set(['torrents','files','peers','peercount','dhtnodes','seeders','leechers','seeds','leechs','count','total','processed','inserted','merged','waiting','sending','requests','responses','activefetches','queuedfetches','fetchsuccesses','fetcherrors','indexedorbackfilled','completedpieces','totalpieces','pieces','piececount','connections','retries','failurecount','successcount','unread','matches']);
const sizeFields=new Set(['bytes','size','totalbytes','downloaded','uploaded']);
const fieldLabels:Record<string,string>={bytes:'数据量',inserted:'新增记录',merged:'合并记录',operation:'当前操作',path:'数据路径',peer:'对端节点',processed:'已处理',rejected:'已拒绝',running:'运行状态',serve:'对外提供',generating:'生成中',sending:'发送中',total:'总数',waiting:'等待中',sharing:'共享状态',snapshot:'数据库快照',fresh:'是否最新',torrents:'资源数量',valid:'有效状态',stage:'当前阶段',totalBytes:'总大小',totalbytes:'总大小',files:'文件数量',peers:'连接节点',size:'总大小',seeders:'做种者',leechers:'下载者',status:'状态',name:'名称',hash:'InfoHash',progress:'进度',downloaded:'已下载',uploaded:'已上传',rate:'传输速率',paused:'暂停状态',complete:'完成状态'};
function fieldLabel(key:string){return fieldLabels[key]||key.replace(/([a-z0-9])([A-Z])/g,'$1 $2').replace(/[_-]+/g,' ').replace(/^./,s=>s.toUpperCase())}
function JsonValue({value,keyName='',depth=0}:{value:any;keyName?:string;depth?:number}):ReactNode{
  if(typeof value==='number')return countFields.has(keyName.toLowerCase())?<CompactNumber value={value}/>:sizeFields.has(keyName.toLowerCase())?size(value):String(value);
  if(typeof value==='boolean')return value?'是':'否';
  if(value==null||typeof value==='string')return value===''?'—':String(value);
  if(Array.isArray(value)){
    if(!value.length)return <span className="value-empty">暂无记录</span>;
    return <div className="data-list">{value.map((item,index)=>typeof item==='object'&&item!==null?<section className="data-item" key={index}><div className="data-item-title">{keyName==='0'?'记录':fieldLabel(keyName)} {index+1}</div><JsonValue value={item} keyName={keyName} depth={depth+1}/></section>:<div className="data-field" key={index}><span>{index+1}</span><b><JsonValue value={item} keyName={keyName} depth={depth+1}/></b></div>)}</div>;
  }
  if(typeof value==='object')return <div className={`data-fields${depth?' nested':''}`}>{Object.entries(value).map(([key,item])=>item!==null&&typeof item==='object'?<details className="field-group" key={key} open={depth<1}><summary>{fieldLabel(key)}{Array.isArray(item)&&<small><CompactNumber value={item.length}/></small>}</summary><JsonValue value={item} keyName={key} depth={depth+1}/></details>:<div className="data-field" key={key}><span>{fieldLabel(key)}</span><b><JsonValue value={item} keyName={key} depth={depth+1}/></b></div>)}</div>;
  return '—';
}
function DataPanel({title,rows}:{title:string;rows:[string,unknown][]}){return <section className="panel">{title&&<h2>{title}</h2>}{rows.map(([k,v])=><div className="data-row" key={k}><span>{k}</span><b><JsonValue value={v} keyName={k}/></b></div>)}</section>}
function Network(){
  useTitle('网络状态');const [state,setState]=useState<any>({});const [err,setErr]=useState('');const [loading,setLoading]=useState(false);
  const refresh=useCallback(async()=>{setLoading(true);try{const [p2p,database,peers]=await Promise.all([api('stats.p2pStatus'),api('database.status'),api('peers.list')]);setState({p2p,database,peers});setErr('')}catch(e){setErr(String(e))}finally{setLoading(false)}},[]);useEffect(()=>{void refresh()},[refresh]);
  const p=state.p2p,db=state.database,peers=state.peers||[];
  return <><PageTitle title="网络状态"/>{err&&<p className="error" role="alert">{err}</p>}<section className="panel network-panel"><div className="panel-title"><h2><span aria-hidden="true">◉</span> 网络状态</h2></div><div className="network-content"><div className="network-summary">{[['Rats 节点网络',p?.running?'运行中':'已停止'],['DHT 网络',p?.dhtRunning?'运行中':'已停止'],['DHT 节点',p?.dhtNodes],['已连接节点',p?.peerCount]].map(([k,v])=><article key={String(k)}><strong>{k}</strong><span>{typeof v==='number'?<CompactNumber value={v}/>:v??'—'}</span></article>)}</div><section className="network-section"><h3>索引共享</h3><div className="network-sharing"><div><span>完整数据库共享授权</span><b>{db?.sharing?'已启用':'未启用'}</b></div><div><span>对外提供任务</span><b>{db?.serve?.generating||(db?.serve?.waiting||0)>0||(db?.serve?.sending||0)>0?'处理中':'无'}</b></div><div><span>本地数据库操作</span><b>{db?.running?(db.operation||'进行中'):'无'}</b></div></div></section><section className="network-section"><h3>连接节点</h3>{!peers.length?<p className="empty">暂无连接节点</p>:peers.map((peer:any,i:number)=><div className="network-peer" key={peer.peerId||peer.address||i}><span className="network-peer-icon" aria-hidden="true">▣</span><span className="network-peer-id">{peer.peerId||peer.address||peer.host||'未知节点'}{peer.port?`:${peer.port}`:''}</span><b>已连接</b></div>)}</section><button className="network-refresh" onClick={()=>void refresh()} disabled={loading}>↻ 刷新</button></div></section></>
}
function Settings(){
  useTitle('基础设置');
  const [c,setC]=useState<any>(); const [f,setF]=useState<any>(); const [stats,setStats]=useState<any>();
  const [msg,setMsg]=useState(''); const [busy,setBusy]=useState(false);const [retryReset,setRetryReset]=useState(false);
  const [confirm,setConfirm]=useState<{kind:'setting'|'runtime'|'reset';key:string;value?:boolean;previous?:boolean;method?:string;prompt:string}|null>(null);
  const [retry,setRetry]=useState<{key:string;value:boolean}|null>(null);
  const resetValues={networkEnabled:true,indexer:true,remoteSearch:true,indexExchange:true,btFileDownload:true,safeSearch:true,p2pReplication:false,p2pReplicationServer:false,databaseSharing:false,btFileUpload:false,resumeBtSession:false,mdns:false,independentDirect:false};
  const load=useCallback(async()=>{try{const [a,b,d]=await Promise.all([api('config.get'),api('features.status'),api('stats.p2pStatus').catch(()=>null)]);setC(a);setF(b);setStats(d);return {config:a,status:b,stats:d}}catch(e){setMsg(String(e));throw e}},[]);
  useEffect(()=>{void load();const id=setInterval(()=>{void Promise.all([api('features.status').then(setF),api('stats.p2pStatus').then(setStats).catch(()=>{})]).catch(()=>{})},2000);return()=>clearInterval(id)},[load]);
  const persist=async(key:string,value:boolean)=>{setBusy(true);setMsg('正在保存设置…');try{await api('config.set',{[key]:value});setRetry(null);try{const latest=await load();setMsg(latest.status?.restartRequired?'已保存，待重启':'已保存')}catch(e){setMsg(`设置已保存，但状态刷新失败：${e}`)}}catch(e){await load().catch(()=>{});setRetry({key,value});setMsg(`保存失败：${e}。配置可能已应用，但未持久化；可重试保存。`)}finally{setBusy(false)}};
  const confirmText=(key:string,value:boolean)=>{
    if(key==='networkEnabled')return value?'保存后下次启动程序会自动启动共享网络；本次不会立即联网。确认保存？':'关闭自动启动偏好不会停止当前网络。确认保存？';
    if(!value)return '';
    if(key==='mdns')return '开启后，下次启动网络时会向局域网广播设备地址和名称。确认保存？';
    if(key==='independentDirect')return '开启后，macOS DHT IPv4 将在下次启动网络时使用物理接口直连，外部节点可能看到真实公网 IP。确认保存？';
    if(key==='indexExchange'){const children=[c?.p2pReplicationServer?'提供 Feed 与随机索引':'',c?.databaseSharing?'提供完整数据库':''].filter(Boolean);return `开启对外索引共享将允许响应远端搜索与详情查询。${children.length?`已保留的子授权也会恢复：${children.join('、')}。`:''}确认保存？`;}
    if(key==='p2pReplicationServer')return '允许向其他 Rats 节点提供 Feed 与随机索引（含文件列表）。确认保存？';
    if(key==='databaseSharing')return '允许向其他 Rats 节点提供完整数据库。确认保存？';
    if(key==='btFileUpload')return '允许上传已获得的资源文件块及做种；已有任务也会受此授权影响。确认保存？';
    if(key==='resumeBtSession')return '下次启动将按下载和上传授权恢复 BT 会话中的任务。确认保存？';
    return '';
  };
  const change=(key:string,value:boolean)=>{if(busy||!c)return;const previous=!!c[key];if(previous===value)return;const prompt=confirmText(key,value);if(prompt){setC({...c,[key]:previous});setConfirm({kind:'setting',key,value,previous,prompt})}else void persist(key,value)};
  const runRuntime=(method:string)=>{if(busy)return;const prompt=method==='network.start'?'启动共享网络会打开 DHT、Rats P2P 和 BT 底层，并可能进行 DHT Bootstrap、STUN、PEX 与已知节点重连。不会改变文件传输或对外共享授权。确认启动？':method==='network.stop'?'停止共享网络将停止 DHT、Rats P2P、BT 连接及相关任务；本地数据库和搜索保持可用。确认停止？':method==='crawler.start'&&!f?.networkRunning?'采集需要共享网络。确认先启动网络，再启动采集？这可能进行 DHT Bootstrap、STUN、PEX 与已知节点重连。': '';if(prompt)setConfirm({kind:'runtime',key:'',method,prompt});else void executeRuntime(method)};
  const executeRuntime=async(method:string)=>{setBusy(true);setMsg(method==='network.start'?'正在启动网络…':method==='network.stop'?'正在停止网络…':method==='crawler.start'?'正在启动采集…':'正在停止采集…');try{if(method==='crawler.start'&&!f?.networkRunning)await api('network.start');await api(method);await load();setMsg('运行操作已完成')}catch(e){await load();setMsg(`运行操作失败：${e}`)}finally{setBusy(false)}};
  const saveReset=async()=>{setBusy(true);setMsg('正在恢复默认配置…');try{await api('config.set',resetValues);setRetry(null);setRetryReset(false);try{const latest=await load();setMsg(latest.status?.restartRequired?'13 项默认配置已保存，待重启':'13 项默认配置已保存；启动偏好不会启动当前网络或采集。')}catch(e){setMsg(`默认配置已保存，但状态刷新失败：${e}`)}}catch(e){await load().catch(()=>{});setRetryReset(true);setMsg(`恢复默认配置未能确认写盘：${e}。配置可能已应用但未持久化；可重试整组保存。`)}finally{setBusy(false)}};
  const applyConfirm=()=>{if(!confirm)return;const pending=confirm;setConfirm(null);if(pending.kind==='setting'&&pending.value!==undefined)void persist(pending.key,pending.value);else if(pending.kind==='runtime'&&pending.method)void executeRuntime(pending.method);else if(pending.kind==='reset')void saveReset()};
  const runtimeText=(key:string)=>({remoteSearch:f?.remoteSearchRunning?'远程搜索条件已具备（不代表正在搜索）':'远程搜索当前不可用',p2pReplication:f?.replicationRunning?'自动同步服务运行中':'自动同步服务已停止',indexExchange:f?.indexSharingRunning?'索引查询应答已启用':'索引查询应答未启用',p2pReplicationServer:f?.indexSharingServing?'对外提供任务处理中':'当前无对外提供任务',databaseSharing:f?.databaseSharingRunning?'完整数据库共享已启用':'完整数据库共享未运行',btFileDownload:`引擎${f?.btEngineReady?'已就绪':'未就绪'} · 实际${f?.btFileDownloadAllowed?'允许':'禁止'}`,btFileUpload:`引擎${f?.btEngineReady?'已就绪':'未就绪'} · 实际${f?.btFileUploadAllowed?'允许':'禁止'}`,mdns:f?.mdnsRunning?'mDNS 正在运行':'mDNS 已停止',independentDirect:f?.independentDirectSocketBound?'DHT IPv4 已绑定物理接口':'当前 Socket 未绑定物理接口'} as Record<string,string>)[key];
  const fields=(title:string,items:[string,string][])=> <details className="settings-group" open><summary>{title}</summary>{items.map(([key,label])=><label className="setting" key={key}><input type="checkbox" checked={!!c?.[key]} disabled={!c||busy||!!confirm||!!retry||(['p2pReplicationServer','databaseSharing'].includes(key)&&!c.indexExchange)} onChange={e=>change(key,e.target.checked)}/><span>{label}{runtimeText(key)&&<small className="setting-state">{runtimeText(key)}</small>}</span><small>配置：{c?c[key]?'开启':'关闭':'未知'}</small></label>)}</details>;
  const stateLabel=(state:string)=>({stopped:'已停止',starting:'正在启动',running:'运行中',stopping:'正在停止',failed:'启动失败'} as Record<string,string>)[state]||'状态未知';
  return <><PageTitle title="基础设置"/><section className="panel pad"><h2>运行控制</h2><div className="grid2"><div><b>共享网络：{stateLabel(f?.networkState)}</b><p>DHT Socket {f?.dhtRunning?'运行中':'已停止'} · <CompactNumber value={stats?.dhtNodes}/> 个节点 · Rats P2P {f?.p2pRunning?'运行中':'已停止'} · <CompactNumber value={f?.peerCount??stats?.peerCount}/> 个连接</p><button disabled={!f||busy||!!confirm||f?.networkState==='running'||f?.networkState==='starting'||f?.networkState==='stopping'} onClick={()=>runRuntime('network.start')}>启动网络</button> <button disabled={!f||busy||!!confirm||f?.networkState!=='running'} onClick={()=>runRuntime('network.stop')}>停止网络</button>{f?.lastError&&<p className="error">{f.lastError}</p>}</div><div><b>DHT Crawler：{stateLabel(f?.crawlerState)}</b><p>启动偏好与当前运行状态分开显示。{f?.crawlerPendingFetches?<> <CompactNumber value={f.crawlerPendingFetches}/> 个请求待结束。</>:''}</p><button disabled={!f||busy||!!confirm||f?.crawlerState==='running'||f?.crawlerState==='starting'||f?.networkState==='starting'||f?.networkState==='stopping'} onClick={()=>runRuntime('crawler.start')}>启动采集</button> <button disabled={!f||busy||!!confirm||f?.crawlerState!=='running'} onClick={()=>runRuntime('crawler.stop')}>停止采集</button>{f?.crawlerLastError&&<p className="error">{f.crawlerLastError}</p>}</div></div><p>已配置：下次启动网络 {c?.networkEnabled?'开启':'关闭'} · 下次自动采集 {c?.indexer?'开启':'关闭'}{f?.crawlerForcedByCli?' · --spider 覆盖采集启动偏好':''}</p></section>{fields('行为授权',[['remoteSearch','允许向 Rats 节点发送搜索词'],['p2pReplication','主动获取远端 Feed 与随机索引'],['indexExchange','对外索引共享'],['p2pReplicationServer','提供 Feed 与随机索引（含文件列表）'],['databaseSharing','提供完整数据库'],['btFileDownload','允许资源文件下载'],['btFileUpload','允许文件块上传与做种']])}{fields('启动偏好',[['networkEnabled','下次启动程序时自动启动共享网络'],['indexer','下次启动共享网络时自动采集'],['resumeBtSession','下次启动程序时自动恢复 BT 会话']])}{fields('高级网络配置',[['independentDirect','DHT IPv4 独立直连（macOS；需重启网络）'],['mdns','允许局域网 mDNS 广播（需重启网络）']])}{fields('本地偏好',[['safeSearch','安全搜索'],['searchHistory','记录搜索历史']])}<div className="toolbar"><button disabled={busy||!!confirm} onClick={()=>setConfirm({kind:'reset',key:'',prompt:'将重置 13 项设置：开启远程搜索、普通索引查询应答、资源文件下载和安全搜索；关闭主动 Feed/随机索引同步、Feed 与随机索引提供、完整数据库共享、BT 上传与做种、BT 会话恢复、mDNS 和 DHT Independent Direct。会设置下次启动自动联网和下次网络启动时自动采集，但不会启动当前网络或 Crawler。其他设置、API 令牌、节点身份、数据库和下载文件保持不变。确认恢复？'})}>恢复默认配置</button><button onClick={()=>void load()}>刷新状态</button>{retry&&<button disabled={busy} onClick={()=>void persist(retry.key,retry.value)}>重试保存“{retry.key}”</button>}{retryReset&&<button disabled={busy} onClick={()=>void saveReset()}>重试恢复默认配置</button>}<button type="button" onClick={()=>{clearToken();location.reload()}}>忘记 API 令牌</button></div>{msg&&<p role="status">{msg}</p>}{confirm&&<div className="modal" role="dialog" aria-modal="true" aria-label="确认操作"><section><h2>确认操作</h2><p>{confirm.prompt}</p><button className="primary" disabled={busy} onClick={applyConfirm}>确认</button><button onClick={()=>setConfirm(null)}>取消</button></section></div>}</>}
function Watch(){
  useTitle('资源关注');
  const [d,setD]=useState<any>(null);
  const [kw,setKw]=useState('');
  const [unreadOnly,setUnreadOnly]=useState(false);
  const [selectedRule,setSelectedRule]=useState<number|null>(null);
  const [err,setErr]=useState('');
  const [cursors,setCursors]=useState<number[]>([0]);
  const [pageIndex,setPageIndex]=useState(1);
  const [loading,setLoading]=useState(false);
  const [action,setAction]=useState('');
  const requestId=useRef(0);
  const pageSize=6;
  const before=cursors[pageIndex-1]??0;
  const load=useCallback(async()=>{
    const request=++requestId.current;
    setLoading(true);
    try {
      const page=await api<any>('watch.list',{unreadOnly,before,ruleId:selectedRule||0});
      if(request===requestId.current){setD(page);setErr('')}
    } catch(e) {
      if(request===requestId.current)setErr(String(e));
    } finally {
      if(request===requestId.current)setLoading(false);
    }
  },[unreadOnly,before,selectedRule]);
  useEffect(()=>{void load();const changed=()=>void load();window.addEventListener('watchChanged',changed);return()=>window.removeEventListener('watchChanged',changed)},[load]);
  const rules=(d?.rules||[]) as any[];
  const totalUnread=rules.reduce((sum:number,r:any)=>sum+Number(r.unread||0),0);
  const activeRules=rules.filter((r:any)=>r.enabled===true||Number(r.enabled)===1).length;
  const selected=rules.find((r:any)=>Number(r.id)===selectedRule);
  const scopedUnread=selected?Number(selected.unread||0):totalUnread;
  const matchTotal=!d?0:selectedRule!==null
    ? Number(unreadOnly?selected?.unread:selected?.total)||0
    : rules.reduce((sum:number,r:any)=>sum+(Number(unreadOnly?r.unread:r.total)||0),0);
  const totalPages=Math.max(1,Math.ceil(matchTotal/pageSize));
  useEffect(()=>{if(d&&pageIndex>totalPages){setCursors([0]);setPageIndex(1)}},[d,pageIndex,totalPages]);
  const pageRows=(d?.matches||[]) as any[];
  const visibleMatches=pageRows.slice(0,pageSize);
  const hasNextPage=pageRows.length>pageSize||!!d?.more;
  const knownLastPage=cursors.length;
  const resetPages=()=>{setCursors([0]);setPageIndex(1)};
  const run=async(method:string,p:any)=>{
    setAction(method);setErr('');
    try {
      await api(method,p);
      if(method==='watch.add')setKw('');
      if(method==='watch.delete'&&Number(p.id)===selectedRule){setSelectedRule(null);resetPages();}
      else await load();
    } catch(e) {setErr(String(e))}
    finally {setAction('')}
  };
  const markAllRead=()=>{
    if(!scopedUnread)return;
    const scope=selected?`「${selected.keyword}」规则`:'所有规则';
    if(window.confirm(`将${scope}下的 ${scopedUnread.toLocaleString('zh-CN')} 条未读提醒全部标记为已读？`))
      void run('watch.readAll',selected?{ruleId:selected.id}:{});
  };
  const next=()=>{
    if(pageIndex<knownLastPage){setPageIndex(pageIndex+1);return}
    const lastVisible=visibleMatches[visibleMatches.length-1];
    if(hasNextPage&&lastVisible){setCursors(xs=>[...xs,Number(lastVisible.id)]);setPageIndex(pageIndex+1)}
  };
  return <div className="watch-page">
    <section className="watch-overview" aria-label="关注概况">
      <p>关键词只匹配规则启用后新入库资源的名称，不回扫已有索引。</p>
      <div className="watch-summary">
        <div><span>关注规则</span><b>{d?<CompactNumber value={rules.length}/>: '—'}</b></div>
        <div><span>正在启用</span><b>{d?<CompactNumber value={activeRules}/>: '—'}</b></div>
        <div><span>未读提醒</span><b>{d?<CompactNumber value={totalUnread}/>: '—'}</b></div>
      </div>
    </section>
    {err&&<p className="error" role="alert">{err}</p>}
    <div className="watch-workspace">
      <section className="panel watch-rules-panel">
        <div className="panel-title"><h2>我的关注</h2><span>{d?`${rules.length} 条规则`: '读取中'}</span></div>
        <form className="watch-add-form" onSubmit={e=>{e.preventDefault();void run('watch.add',{keyword:kw.trim()})}}>
          <input aria-label="关注关键词" value={kw} onChange={e=>setKw(e.target.value)} placeholder="添加关键词" maxLength={256}/>
          <button className="primary" disabled={!kw.trim()||!!action}>添加</button>
        </form>
        <div className="watch-rule-list">
          {rules.map((r:any)=>{
            const enabled=r.enabled===true||Number(r.enabled)===1;
            const selectedRuleActive=Number(r.id)===selectedRule;
            return <article className={`watch-rule${selectedRuleActive?' selected':''}`} key={r.id}>
              <button type="button" className="watch-rule-select" aria-pressed={selectedRuleActive}
                onClick={()=>{setSelectedRule(selectedRuleActive?null:Number(r.id));resetPages()}}>
                <span className="watch-rule-top"><b>{r.keyword}</b><span className={`watch-state${enabled?' active':''}`}>{enabled?'监看中':'已暂停'}</span></span>
                <span className="watch-rule-counts"><span><CompactNumber value={r.total}/> 条匹配</span><span><CompactNumber value={r.unread}/> 条未读</span></span>
              </button>
              <div className="watch-rule-actions">
                <button type="button" disabled={!!action} onClick={()=>void run('watch.enable',{id:r.id,enabled:!enabled})}>{enabled?'暂停':'启用'}</button>
                <button type="button" className="watch-delete" disabled={!!action} onClick={()=>window.confirm(`删除「${r.keyword}」及其全部 ${Number(r.total||0).toLocaleString('zh-CN')} 条提醒？资源仍保留在索引中。`)&&void run('watch.delete',{id:r.id})}>删除</button>
              </div>
            </article>;
          })}
          {!rules.length&&d&&!loading&&<p className="empty">还没有关注规则。添加关键词后，探险者会留意之后新入库的资源。</p>}
          {!d&&loading&&<p className="empty">正在读取关注规则…</p>}
        </div>
      </section>
      <section className="panel watch-matches-panel">
        <div className="watch-matches-heading">
          <div><h2>匹配提醒</h2>{(selected||unreadOnly)&&<p>{selected&&<>规则：<b>{selected.keyword}</b></>}{unreadOnly?' · 仅未读':''}</p>}</div>
          <button type="button" className="watch-clear-rule" disabled={!selectedRule} onClick={()=>{setSelectedRule(null);resetPages()}}>查看全部规则</button>
        </div>
        <div className="watch-match-controls">
          <div className="watch-tabs" role="group" aria-label="提醒范围">
            <button type="button" className={!unreadOnly?'selected':''} aria-pressed={!unreadOnly} onClick={()=>{setUnreadOnly(false);resetPages()}}>全部</button>
            <button type="button" className={unreadOnly?'selected':''} aria-pressed={unreadOnly} onClick={()=>{setUnreadOnly(true);resetPages()}}>未读</button>
          </div>
          <button type="button" className="watch-mark-all" disabled={!scopedUnread||!!action||loading} onClick={markAllRead}>全部标记已读{selected?'（当前规则）':''}</button>
        </div>
        {loading&&<p className="watch-loading" role="status">正在更新提醒…</p>}
        <div className="watch-match-list" aria-live="polite">
          {visibleMatches.map((m:any)=><article className={`watch-match${!m.seen?' unread':''}`} key={m.id}>
            <span className="watch-unread-mark" aria-hidden="true"/>
            <div className="watch-match-main">
              <Link className="watch-match-name" to={`/torrent/${m.hash}`}>{m.name||m.hash}</Link>
              {m.name&&<code>{m.hash}</code>}
              <div className="watch-match-meta"><span>命中 <b>{m.keyword}</b></span><time dateTime={new Date(Number(m.time)).toISOString()}>{new Date(Number(m.time)).toLocaleString()}</time></div>
            </div>
            <div className="watch-match-actions">
              <span className={`watch-read-state${!m.seen?' unread':''}`}>{m.seen?'已读':'未读'}</span>
              <button type="button" disabled={!!m.seen||!!action} onClick={()=>void run('watch.read',{id:m.id})}>{m.seen?'已读':'标记已读'}</button>
            </div>
          </article>)}
          {!d?.matches?.length&&!loading&&<p className="watch-empty">{unreadOnly?'当前筛选范围内没有未读提醒。':selected?'该关注规则还没有匹配资源。':'暂无匹配资源。'}</p>}
        </div>
        {matchTotal>0&&<nav className="watch-pager" aria-label="提醒分页">
          <button type="button" aria-label="上一页" disabled={pageIndex<=1||loading} onClick={()=>setPageIndex(pageIndex-1)}>‹</button>
          <span className="watch-page-current" aria-live="polite">第 {pageIndex} 页 / 共 {totalPages} 页</span>
          <button type="button" aria-label="下一页" disabled={(!hasNextPage&&pageIndex>=knownLastPage)||pageIndex>=totalPages||loading} onClick={next}>›</button>
        </nav>}
      </section>
    </div>
  </div>;
}
function Trawl(){
  useTitle('Trawl · 探险者活动流');const [rows,setRows]=useState<any[]>([]);const [connection,setConnection]=useState('连接中');const [crawler,setCrawler]=useState('状态未知');const [filter,setFilter]=useState('全部');const [paused,setPaused]=useState(false);const socketRef=useRef<WebSocket|null>(null);const pausedRef=useRef(false);const buffer=useRef<any[]>([]);
  useEffect(()=>{pausedRef.current=paused},[paused]);
  useEffect(()=>{let dead=false;let retry:ReturnType<typeof setTimeout>;let poll:ReturnType<typeof setInterval>;let delay=1000;
    const readState=()=>api<any>('features.status').then(x=>{if(!dead)setCrawler(({running:'采集中',starting:'启动中',stopping:'停止中',stopped:'已停止',failed:'启动失败'} as any)[x.crawlerState]||'状态未知')}).catch(()=>{if(!dead)setCrawler('状态未知')});
    const connect=async()=>{const auth=await requestToken();if(dead)return;if(!auth){setConnection('未认证');retry=setTimeout(connect,3000);return}let socket:WebSocket;try{socket=new WebSocket(wsUrl());socketRef.current=socket}catch{setConnection('连接失败，正在重连');retry=setTimeout(connect,delay);delay=Math.min(delay*2,15000);return}
      socket.onopen=()=>socket.send(JSON.stringify({method:'auth',params:{token:auth}}));
      socket.onmessage=e=>{try{const frame=JSON.parse(e.data);if(frame.data?.authenticated&&frame.success){setConnection('实时连接中');delay=1000;void readState();clearInterval(poll);poll=setInterval(()=>void readState(),15000);return}if(frame.data?.authenticated===false||frame.success===false&&frame.error?.toLowerCase?.().includes('auth')){clearToken();setConnection('认证失败，令牌已清除；访问 API 时重新连接');return}if(frame.event==='trawlState'){setCrawler(({Active:'采集中',Stopping:'停止中',Stopped:'已停止'} as any)[frame.data?.status]||'状态未知');return}if(frame.event){const row={event:frame.event,time:frame.time||Date.now(),data:frame.data||{}};buffer.current=[row,...buffer.current].slice(0,200);if(!pausedRef.current)setRows(buffer.current)}}catch{}};
      socket.onclose=event=>{clearInterval(poll);if(dead)return;if(event.code===1008){clearToken();window.dispatchEvent(new Event('rats-auth-failed'));setConnection('认证失败，令牌已清除；访问 API 时重新连接')}else setConnection('连接断开，正在重连');retry=setTimeout(connect,delay);delay=Math.min(delay*2,15000)};socket.onerror=()=>setConnection('连接错误，正在重连');
    };void connect();return()=>{dead=true;clearTimeout(retry);clearInterval(poll);socketRef.current?.close()}
  },[]);
  const cats=['全部','资源发现','调查中','入库','调查失败','资料补齐','系统事件'];
  const category=(row:any)=>row.event==='trawlQueued'||row.event==='trawlOmitted'?'资源发现':row.event==='trawlStarted'||row.event==='trawlMetadata'&&row.data.outcome==='received'?'调查中':row.event==='torrentIndexed'?'入库':row.event==='torrentBackfilled'?'资料补齐':row.event==='trawlMetadata'?'调查失败':'系统事件';
  const title=(r:any)=>r.event==='torrentIndexed'?'新资源已入库':r.event==='torrentBackfilled'?'旧资源资料已补齐':r.event==='trawlQueued'?'线索已入队':r.event==='trawlStarted'?'开始调查':r.event==='trawlMetadata'?(r.data.outcome==='received'?'已取得元数据':'调查结束'):r.event==='trawlOmitted'?'部分线索提示已省略':r.event;
  const filtered=rows.filter(x=>filter==='全部'||category(x)===filter);
  return <><PageTitle title="Trawl · 探险者活动流" sub={`${connection} · Crawler ${crawler}`} action={<button onClick={()=>{setPaused(v=>{if(v)setRows(buffer.current);return !v})}}>{paused?'恢复列表':'暂停列表'}</button>}/><div className="toolbar">{cats.map(c=><button className={c===filter?'selected':''} key={c} onClick={()=>setFilter(c)}>{c}</button>)}</div>{!filtered.length&&<p className="empty">当前会话暂无事件；刷新页面不会恢复未持久化的历史。</p>}{filtered.map((r,i)=><article className="event" key={`${r.event}-${r.time}-${i}`}><time>{new Date(r.time).toLocaleTimeString()}</time><b>{title(r)}</b><span>{r.data.name||r.data.hash||JSON.stringify(r.data)}</span>{/^[0-9a-f]{40}$/i.test(r.data.hash||'')&&(r.event==='torrentIndexed'||r.event==='torrentBackfilled')&&<Link to={`/torrent/${r.data.hash}`}>查看资源</Link>}</article>)}</>
}
function DataPage({mode}:{mode:'index'|'downloads'}){
  const title=mode==='index'?'索引与同步':'下载任务';useTitle(title);const [d,setD]=useState<any>();const [err,setErr]=useState('');
  const load=()=>Promise.all(mode==='downloads'?[api('download.list')]:[api('database.status'),api('stats.get')]).then(setD).catch(e=>setErr(String(e)));
  useEffect(()=>{void load()},[]);
  const sections=mode==='index'?[['数据库状态',d?.[0]],['索引统计',d?.[1]]] as [string,unknown][]:[['下载任务',d?.[0]]] as [string,unknown][];
  return <><PageTitle title={title} sub="RATS SEARCH · LIVE DATA" action={<button onClick={()=>void load()}>刷新</button>}/><p className="data-page-hint">只读显示 API 当前返回的数据。</p>{err&&<p className="error" role="alert">{err}</p>}{d===undefined&&!err?<p className="empty">正在读取…</p>:sections.map(([name,value])=><section className="panel data-panel" key={name}><div className="panel-title"><h2>{name}</h2></div>{mode==='downloads'&&Array.isArray(value)&&value.length===0?<p className="empty">当前没有下载任务。</p>:value===undefined?<p className="empty">暂无数据。</p>:<div className="data-panel-content"><JsonValue value={value} keyName={mode==='downloads'?'下载任务':name}/></div>}</section>)}</>;
}
export default App;
