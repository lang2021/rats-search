import { Component, OnDestroy, OnInit } from "@angular/core";
import { AppModule } from "../app.module";
import { ratsApiBaseUrl } from "../../environments/environment";
import { ratsCall, requestRatsToken, summarizeRatsEvent } from "./rats-api";

const categories = ["全部", "资源发现", "调查中", "入库", "调查失败", "资料补齐", "系统事件"] as const;
type Category = typeof categories[number];
type Activity = {
  time: number; category: Category; title: string; detail: string; hash?: string; link?: string;
};
const failures: Record<string, string> = {
  "metadata timeout": "元数据超时",
  "bittorrent stopped": "已取消 · BT 引擎停止",
  "shutting down": "已取消 · 网络关闭中",
  observationStopped: "采集已停止，本轮不再接收该任务结果",
  "invalid info hash": "Hash 无效",
  "torrent already active without metadata": "已有任务尚无元数据",
  "failed to create metadata task": "无法创建元数据任务",
  "BT metadata engine unavailable": "元数据引擎不可用",
  "bittorrent not running": "BT 引擎未运行",
  "invalid or mismatched BEP 9 metadata": "元数据校验失败",
};

function activity(frame: {event?: string; time?: number; data?: Record<string, unknown>}): Activity | undefined {
  const data = frame.data ?? {};
  const hash = typeof data["hash"] === "string" ? data["hash"] : "";
  const time = typeof frame.time === "number" ? frame.time : Date.now();
  const base = { time, hash: hash.slice(0, 12) };
  switch (frame.event) {
    case "trawlQueued":
      return { ...base, category: "资源发现", title: "线索已入队",
        detail: String(data["source"]) + (data["hasPeer"] ? " · 含资源 Peer 线索" : "") };
    case "trawlStarted":
      return { ...base, category: "调查中", title: "开始获取元数据",
        detail: data["hasPeer"] ? "已有可尝试的 Peer" : "按 Hash 查找 Peer" };
    case "trawlMetadata":
      if (data["outcome"] === "received")
        return { ...base, category: "调查中", title: "已取得元数据", detail: "等待索引处理" };
      return { ...base, category: "调查失败", title: "调查结束",
        detail: failures[String(data["outcome"])] ?? "元数据获取失败，原因未确定" };
    case "torrentIndexed":
    case "torrentBackfilled": {
      const backfill = frame.event === "torrentBackfilled";
      return { time, hash, category: backfill ? "资料补齐" : "入库",
        title: backfill ? "旧资源文件列表已补齐" : "新资源已入库",
        detail: [data["name"], typeof data["size"] === "number" ? data["size"].toLocaleString() + " B" : "",
          typeof data["files"] === "number" ? data["files"] + " 个文件" : ""].filter(Boolean).join(" · "),
        link: /^[0-9a-f]{40}$/i.test(hash) ? "/torrent/" + hash : undefined };
    }
    case "trawlOmitted":
      return { time, category: "资源发现", title: "部分入队提示已省略", detail: String(data["count"]) + " 条" };
    default: {
      const system = summarizeRatsEvent(frame);
      if (system) return { time, category: "系统事件", title: system.name, detail: system.detail };
      return undefined;
    }
  }
}

@Component({
  selector: "app-trawl", standalone: true, imports: [AppModule],
  templateUrl: "./trawl.component.html", styleUrl: "./trawl.component.scss",
})
export class TrawlComponent implements OnInit, OnDestroy {
  readonly categories = categories;
  readonly icons: Record<Category, string> = {
    "全部": "list", "资源发现": "radar", "调查中": "search", "入库": "inventory_2",
    "调查失败": "info", "资料补齐": "playlist_add_check", "系统事件": "settings",
  };
  filter: Category = "全部";
  connection = "连接中";
  crawler = "状态未知";
  paused = false;
  rows: Activity[] = [];
  private buffer: Activity[] = [];
  private ws?: WebSocket;
  private retry?: ReturnType<typeof setTimeout>;
  private render?: ReturnType<typeof setTimeout>;
  private poll?: ReturnType<typeof setInterval>;
  private destroyed = false;
  private stateRevision = 0;
  private delay = 1000;

  ngOnInit(): void { void this.connect(); }
  ngOnDestroy(): void {
    this.destroyed = true;
    clearTimeout(this.retry);
    clearTimeout(this.render);
    clearInterval(this.poll);
    this.ws?.close();
  }
  select(category: Category): void { this.filter = category; }
  get hasMatches(): boolean { return this.rows.some(row => row.category === this.filter); }
  togglePause(): void {
    this.paused = !this.paused;
    if (!this.paused) this.rows = this.buffer.slice();
  }
  private add(row: Activity): void {
    this.buffer.unshift(row);
    if (this.buffer.length > 200) {
      // Reserve up to 40 places for confirmed database writes.
      let protectedWrites = 0;
      let victim = 0;
      for (let i = 0; i < this.buffer.length; i++) {
        if (this.buffer[i].link && ++protectedWrites <= 40) continue;
        victim = i;
      }
      this.buffer.splice(victim, 1);
    }
    if (!this.paused && !this.render)
      this.render = setTimeout(() => {
        this.render = undefined;
        if (!this.paused) this.rows = this.buffer.slice();
      }, 150);
  }
  private async readState(socket: WebSocket): Promise<void> {
    const revision = this.stateRevision;
    try {
      const state = await ratsCall<{crawlerState: string}>("features.status");
      if (this.ws === socket && this.stateRevision === revision)
        this.crawler = ({running:"采集中", stopping:"停止中", stopped:"已停止", failed:"启动失败",
          starting:"启动中"} as Record<string,string>)[state.crawlerState] ?? "状态未知";
    } catch {
      if (this.ws === socket && this.stateRevision === revision) this.crawler = "状态未知";
    }
  }
  private reconnect(): void {
    this.connection = "连接断开，等待重连";
    this.crawler = "状态未知";
    this.retry = setTimeout(() => {
      this.connection = "正在重连";
      void this.connect();
    }, this.delay);
    this.delay = Math.min(15000, this.delay * 2);
  }
  private async connect(): Promise<void> {
    const token = await requestRatsToken();
    if (this.destroyed) return;
    if (!token) { this.connection = "未认证，刷新页面可重新连接"; return; }
    const base = ratsApiBaseUrl ? new URL(ratsApiBaseUrl) : location;
    const secure = base.protocol === "https:";
    try {
      const socket = new WebSocket((secure ? "wss://" : "ws://") + base.hostname + ":" +
        (Number(base.port || (secure ? 443 : 80)) + 1));
      this.ws = socket;
      socket.onopen = () => socket.send(JSON.stringify({method:"auth", params:{token}}));
      socket.onmessage = message => {
        if (this.destroyed || this.ws !== socket) return;
        try {
          const frame = JSON.parse(message.data);
          if (frame.success && frame.data?.authenticated) {
            this.connection = "实时连接"; this.delay = 1000;
            void this.readState(socket);
            clearInterval(this.poll);
            this.poll = setInterval(() => void this.readState(socket), 15000);
          } else if (frame.event === "trawlState") {
            this.stateRevision++;
            this.crawler = ({Active:"采集中", Stopping:"停止中", Stopped:"已停止"} as Record<string,string>)[frame.data.status] ?? "状态未知";
          } else {
            const row = activity(frame);
            if (row) this.add(row);
          }
        } catch { /* Ignore malformed frames. */ }
      };
      socket.onclose = event => {
        clearInterval(this.poll);
        this.ws = undefined;
        if (this.destroyed) return;
        if (event.code === 1008) {
          sessionStorage.removeItem("rats-api-token");
          this.connection = "认证失败，请刷新并重新输入令牌"; this.crawler = "状态未知";
        } else this.reconnect();
      };
    } catch { this.reconnect(); }
  }
}
