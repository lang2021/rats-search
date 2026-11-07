import { Component, OnDestroy, OnInit } from "@angular/core";
import { AppModule } from "../app.module";
import { DocumentTitleComponent } from "../layout/document-title.component";
import { clearRatsAuthToken, requestRatsToken, ratsCall, ratsReady, summarizeRatsEvent } from "../rats/rats-api";
import { ratsApiBaseUrl } from "../../environments/environment";

type Stats = { torrents: number; files: number; size: number | null; peers: number };
type P2p = { peerCount: number; dhtNodes: number; dhtRunning: boolean; running: boolean };
type FeatureStatus = { networkState: string; crawlerState: string };
type CrawlerStats = {
  sampleRequests: number; sampleResponses: number; sampleHashes: number;
  hashesEnqueued: number; activeFetches: number; queuedFetches: number;
  fetchSuccesses: number; fetchErrors: number; indexedOrBackfilled: number;
};
type Sync = { operation?: string; running?: boolean; sharing?: boolean; processed?: number; total?: number; snapshot?: { valid?: boolean; torrents?: number }; serve?: { sending?: number; waiting?: number } };
type Peer = { peerId?: string; address?: string; host?: string };
type EventRow = { time: string; event: string; detail: string };

@Component({
  selector: "app-dashboard-home",
  templateUrl: "./dashboard-home.component.html",
  styleUrl: "./dashboard-home.component.scss",
  standalone: true,
  imports: [AppModule, DocumentTitleComponent],
})
export class DashboardHomeComponent implements OnInit, OnDestroy {
  stats?: Stats; p2p?: P2p; sync?: Sync; peers: Peer[] = []; downloads: unknown[] = [];
  features?: FeatureStatus; crawler?: CrawlerStats;
  ready?: boolean; error = ""; events: EventRow[] = [];
  private timer?: ReturnType<typeof setInterval>;
  private ws?: WebSocket;

  ngOnInit(): void {
    void this.refresh();
    this.timer = setInterval(() => void this.refresh(), 15000);
    this.connectEvents();
  }
  ngOnDestroy(): void { if (this.timer) clearInterval(this.timer); this.ws?.close(); }
  async refresh(): Promise<void> {
    const results = await Promise.allSettled([
      ratsCall<Stats>("stats.get"), ratsCall<P2p>("stats.p2pStatus"),
      ratsCall<Sync>("database.status"), ratsCall<Peer[]>("peers.list"),
      ratsCall<unknown[]>("download.list"), ratsReady(),
      ratsCall<FeatureStatus>("features.status"), ratsCall<CrawlerStats>("stats.crawler"),
    ]);
    if (results[0].status === "fulfilled") this.stats = results[0].value;
    if (results[1].status === "fulfilled") this.p2p = results[1].value;
    if (results[2].status === "fulfilled") this.sync = results[2].value;
    if (results[3].status === "fulfilled") this.peers = results[3].value;
    if (results[4].status === "fulfilled") this.downloads = results[4].value;
    if (results[5].status === "fulfilled") this.ready = results[5].value;
    this.features = results[6].status === "fulfilled" ? results[6].value : undefined;
    this.crawler = results[7].status === "fulfilled" ? results[7].value : undefined;
    this.error = results.some(r => r.status === "rejected") ? "部分指标暂不可用，请检查 API 连接。" : "";
  }
  private connectEvents(): void {
    const base = ratsApiBaseUrl ? new URL(ratsApiBaseUrl) : location;
    const url = `${base.protocol === "https:" ? "wss:" : "ws:"}//${base.hostname}:${Number(base.port || 80) + 1}`;
    try {
      this.ws = new WebSocket(url);
      this.ws.onopen = async () => this.ws?.send(JSON.stringify({ method: "auth", params: { token: await requestRatsToken() } }));
      this.ws.onmessage = message => {
        try {
          const frame = JSON.parse(String(message.data));
          if (frame.data?.authenticated === false || frame.success === false && String(frame.error ?? "").toLowerCase().includes("auth")) {
            clearRatsAuthToken();
            this.error = "WebSocket 认证失败，令牌已清除；访问 API 时重新连接";
            this.ws?.close();
            return;
          }
          const parsed = summarizeRatsEvent(frame);
          if (!parsed) return;
          this.events = [{ time: new Date().toLocaleTimeString(), event: parsed.name, detail: parsed.detail }, ...this.events].slice(0, 20);
        } catch { /* Ignore malformed event frames. */ }
      };
      this.ws.onclose = event => {
        if (event.code === 1008) {
          clearRatsAuthToken();
          this.error = "WebSocket 认证失败，令牌已清除；访问 API 时重新连接";
        }
      };
    } catch { /* Read-only dashboard remains usable without WebSocket. */ }
  }
  formatBytes(value?: number | null): string {
    if (value == null || !Number.isFinite(value)) return "—";
    if (value < 1024) return `${value} B`;
    const units = ["KB", "MB", "GB", "TB"];
    const i = Math.min(Math.floor(Math.log(value) / Math.log(1024)) - 1, units.length - 1);
    return `${(value / 1024 ** (i + 1)).toFixed(1)} ${units[i]}`;
  }
}
