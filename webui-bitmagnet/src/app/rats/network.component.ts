import { Component, OnInit } from "@angular/core";
import { AppModule } from "../app.module";
import { DocumentTitleComponent } from "../layout/document-title.component";
import { ratsCall } from "./rats-api";

type P2p = {
  peerCount: number;
  dhtNodes: number;
  dhtRunning: boolean;
  running: boolean;
};
type Peer = {
  peerId?: string;
  address?: string;
  host?: string;
  port?: number;
  [key: string]: unknown;
};
type Database = {
  operation?: string;
  running?: boolean;
  sharing?: boolean;
  snapshot?: unknown;
  serve?: { generating?: boolean; waiting?: number; sending?: number };
};

@Component({
  selector: "app-rats-network",
  standalone: true,
  imports: [AppModule, DocumentTitleComponent],
  template: `
    <app-document-title [parts]="['网络状态']" />
    <mat-card class="page">
      <mat-card-header
        ><mat-card-title
          ><mat-icon>hub</mat-icon> 网络状态</mat-card-title
        ></mat-card-header
      >
      <mat-card-content>
        <div class="summary">
          <mat-card
            ><mat-card-content
              ><strong>Rats 节点网络</strong
              ><span>{{
                p2p?.running ? "运行中" : "已停止"
              }}</span></mat-card-content
            ></mat-card
          >
          <mat-card
            ><mat-card-content
              ><strong>DHT 网络</strong
              ><span>{{
                p2p?.dhtRunning ? "运行中" : "已停止"
              }}</span></mat-card-content
            ></mat-card
          >
          <mat-card
            ><mat-card-content
              ><strong>DHT 节点</strong
              ><span>{{ p2p?.dhtNodes ?? "—" }}</span></mat-card-content
            ></mat-card
          >
          <mat-card
            ><mat-card-content
              ><strong>已连接节点</strong
              ><span>{{ p2p?.peerCount ?? "—" }}</span></mat-card-content
            ></mat-card
          >
        </div>
        <h3>索引共享</h3>
        <p>
          完整数据库共享授权：{{ database?.sharing ? "已启用" : "未启用" }}；
          对外提供任务：{{ database?.serve?.generating || (database?.serve?.waiting ?? 0) > 0 || (database?.serve?.sending ?? 0) > 0 ? "处理中" : "无" }}；
          本地数据库操作：{{ database?.running ? (database?.operation || "进行中") : "无" }}
        </p>
        <h3>连接节点</h3>
        <p *ngIf="peers.length === 0">暂无连接节点</p>
        <div *ngFor="let peer of peers" class="peer">
          <mat-icon>computer</mat-icon
          ><span>{{
            peer.peerId || peer.address || peer.host || "未知节点"
          }}</span
          ><small *ngIf="peer.port">:{{ peer.port }}</small>
        </div>
        <p *ngIf="error" class="error">{{ error }}</p>
        <button mat-stroked-button (click)="refresh()">
          <mat-icon>refresh</mat-icon>刷新
        </button>
      </mat-card-content>
    </mat-card>
  `,
  styles: [
    `
      .page { margin: 0; }
      .page > mat-card-content { padding: 16px; }
      .page mat-card-header { min-height: 42px; box-sizing: border-box; padding: 10px 16px; background: var(--rs-raised); border-bottom: 1px solid var(--rs-border); color: var(--rs-accent); }
      .page mat-card-title { font-size: 11px; letter-spacing: .24em; color: var(--rs-accent); }
      .page mat-card-title mat-icon { font-size: 17px; width: 18px; height: 18px; vertical-align: middle; }
      .summary {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(145px, 1fr));
        gap: 10px;
      }
      .summary mat-card-content {
        display: flex;
        flex-direction: column;
        gap: 12px;
        padding: 16px;
      }
      .summary strong { color: var(--rs-subtle); font-size: 10px; font-weight: 500; letter-spacing: .19em; }
      .summary span { color: var(--rs-accent); font: 500 25px ui-monospace,monospace; }
      h3 { margin: 16px 0 0; padding: 12px 14px; background: var(--rs-raised); border: 1px solid var(--rs-border); color: var(--rs-accent); font-size: 10px; letter-spacing: .25em; }
      h3 + p { border: 1px solid var(--rs-border); border-top: 0; margin: 0; padding: 13px 14px; color: var(--rs-muted); font-size: 12px; }
      .peer {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 9px 14px; border: 1px solid var(--rs-border); border-top: 0; font-size: 12px;
      }
      .error { color: var(--rs-danger); }
      button { margin-top: 15px; border-radius: 0; border-color: var(--rs-accent); color: var(--rs-accent); }
    `,
  ],
})
export class NetworkComponent implements OnInit {
  p2p?: P2p;
  database?: Database;
  peers: Peer[] = [];
  error = "";

  ngOnInit(): void {
    void this.refresh();
  }

  async refresh(): Promise<void> {
    try {
      [this.p2p, this.database, this.peers] = await Promise.all([
        ratsCall<P2p>("stats.p2pStatus"),
        ratsCall<Database>("database.status"),
        ratsCall<Peer[]>("peers.list"),
      ]);
      this.error = "";
    } catch (error) {
      this.error = String(error);
    }
  }
}
