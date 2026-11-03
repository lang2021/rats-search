import { Component, OnInit } from "@angular/core";
import { AppModule } from "../app.module";
import { ratsCall } from "./rats-api";

type Stats = { torrents: number; files: number; size: number; peers: number };
type P2p = {
  peerCount: number;
  dhtNodes: number;
  dhtRunning: boolean;
  running: boolean;
};

@Component({
  selector: "app-rats-overview",
  standalone: true,
  imports: [AppModule],
  template: `
    <div class="stats">
      <mat-card
        ><mat-card-content
          ><mat-icon>inventory_2</mat-icon
          ><strong>{{ stats?.torrents ?? "—" }}</strong
          ><span>索引资源</span></mat-card-content
        ></mat-card
      >
      <mat-card
        ><mat-card-content
          ><mat-icon>description</mat-icon
          ><strong>{{ stats?.files ?? "—" }}</strong
          ><span>文件</span></mat-card-content
        ></mat-card
      >
      <mat-card
        ><mat-card-content
          ><mat-icon>hub</mat-icon><strong>{{ p2p?.peerCount ?? "—" }}</strong
          ><span>连接节点</span></mat-card-content
        ></mat-card
      >
      <mat-card
        ><mat-card-content
          ><mat-icon>lan</mat-icon><strong>{{ p2p?.dhtNodes ?? "—" }}</strong
          ><span>DHT 节点</span></mat-card-content
        ></mat-card
      >
    </div>
    <p *ngIf="error" class="error">{{ error }}</p>
    <button mat-stroked-button (click)="refresh()">
      <mat-icon>refresh</mat-icon>刷新统计
    </button>
  `,
  styles: [
    `
      .stats {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
        gap: 16px;
        margin: 20px 0;
      }
      mat-card-content {
        display: flex;
        flex-direction: column;
        gap: 7px;
        padding: 20px;
      }
      mat-icon {
        opacity: 0.75;
      }
      strong {
        font-size: 2rem;
        line-height: 1;
      }
      span {
        opacity: 0.7;
      }
      .error {
        color: #d32f2f;
      }
    `,
  ],
})
export class RatsOverviewComponent implements OnInit {
  stats?: Stats;
  p2p?: P2p;
  error = "";

  ngOnInit(): void {
    void this.refresh();
  }

  async refresh(): Promise<void> {
    try {
      [this.stats, this.p2p] = await Promise.all([
        ratsCall<Stats>("stats.get"),
        ratsCall<P2p>("stats.p2pStatus"),
      ]);
      this.error = "";
    } catch (error) {
      this.error = String(error);
    }
  }
}
