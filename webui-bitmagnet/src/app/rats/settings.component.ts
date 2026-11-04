import { Component, OnInit } from "@angular/core";
import { FormsModule } from "@angular/forms";
import { AppModule } from "../app.module";
import { DocumentTitleComponent } from "../layout/document-title.component";
import { ratsCall } from "./rats-api";

type Config = {
  networkEnabled: boolean;
  resumeBtSession: boolean;
  mdns: boolean;
  indexExchange: boolean;
  safeSearch: boolean;
  indexer: boolean;
  databaseSharing: boolean;
  p2pReplication: boolean;
  httpPort: number;
  p2pPort: number;
  dhtPort: number;
};

type FeatureStatus = {
  networkDesired: boolean;
  networkRunning: boolean;
  restartRequired: boolean;
  dhtRunning: boolean;
  mdnsRunning: boolean;
  p2pRunning: boolean;
  btEngineReady: boolean;
  crawlerRunning: boolean;
  replicationRunning: boolean;
  databaseSharingRunning: boolean;
  indexExchangeRunning: boolean;
  peerCount: number;
  lastError: string;
};

@Component({
  selector: "app-rats-settings",
  standalone: true,
  imports: [AppModule, FormsModule, DocumentTitleComponent],
  template: `
    <app-document-title [parts]="['基础设置']" />
    <mat-card class="page">
      <mat-card-header
        ><mat-card-title
          ><mat-icon>settings</mat-icon> 基础设置</mat-card-title
        ></mat-card-header
      >
      <mat-card-content *ngIf="config as c">
        <p>HTTP {{ c.httpPort }} · P2P {{ c.p2pPort }} · DHT {{ c.dhtPort }}</p>
        <p *ngIf="status as s">网络：{{ s.networkRunning ? '运行中' : '已停止' }} · DHT {{ s.dhtRunning ? '运行' : '停止' }} · P2P {{ s.p2pRunning ? '运行' : '停止' }} · BT {{ s.btEngineReady ? '就绪' : '停止' }} · 采集 {{ s.crawlerRunning ? '运行' : '停止' }} · 索引交换 {{ s.indexExchangeRunning ? '运行' : '停止' }} · 复制 {{ s.replicationRunning ? '运行' : '停止' }} · mDNS {{ s.mdnsRunning ? '运行' : '停止' }} · 数据库共享 {{ s.databaseSharingRunning ? '运行' : '停止' }}<span *ngIf="s.restartRequired"> · 启用网络需重启程序</span><span *ngIf="s.lastError"> · {{ s.lastError }}</span></p>
        <div class="options">
          <mat-checkbox [(ngModel)]="c.networkEnabled">启用网络功能（关闭立即停止；开启需重启）</mat-checkbox>
          <mat-checkbox [(ngModel)]="c.safeSearch">安全搜索</mat-checkbox>
          <mat-checkbox [(ngModel)]="c.resumeBtSession">保存并恢复 BT 下载与做种会话</mat-checkbox>
          <mat-checkbox [(ngModel)]="c.mdns">局域网 mDNS 广播（重启生效）</mat-checkbox>
          <mat-checkbox [(ngModel)]="c.indexExchange">允许 P2P 搜索与索引交换</mat-checkbox>
          <mat-checkbox [(ngModel)]="c.indexer">DHT 索引爬取</mat-checkbox>
          <mat-checkbox [(ngModel)]="c.databaseSharing"
            >数据库共享</mat-checkbox
          >
          <mat-checkbox [(ngModel)]="c.p2pReplication"
            >P2P 索引复制</mat-checkbox
          >
        </div>
        <p *ngIf="message">{{ message }}</p>
        <button
          mat-flat-button
          color="primary"
          (click)="confirming = true"
          [disabled]="saving"
        >
          保存设置
        </button>
        <span *ngIf="confirming" class="confirm" role="group" aria-label="确认保存设置">
          启用网络后会启动 DHT 与 P2P；索引交换、下载恢复、局域网广播和数据库共享按各自开关执行。关闭网络会立即停止连接，重新开启需重启程序。
          <button mat-flat-button color="primary" (click)="save()" [disabled]="saving">确认保存</button>
          <button mat-button (click)="confirming = false" [disabled]="saving">取消</button>
        </span>
        <button mat-button (click)="load()">重新读取</button>
      </mat-card-content>
      <mat-card-content *ngIf="!config">{{
        message || "读取中…"
      }}</mat-card-content>
    </mat-card>
  `,
  styles: [
    `
      .page {
        margin: 20px;
      }
      mat-card-content {
        padding: 20px;
      }
      .confirm { display: inline-flex; align-items: center; gap: 8px; flex-wrap: wrap; margin: 8px 0; }
      .options {
        display: flex;
        flex-direction: column;
        gap: 14px;
        margin: 24px 0;
      }
    `,
  ],
})
export class SettingsComponent implements OnInit {
  config?: Config;
  status?: FeatureStatus;
  message = "";
  saving = false;
  confirming = false;

  ngOnInit(): void {
    void this.load();
  }

  async load(): Promise<void> {
    try {
      this.config = await ratsCall<Config>("config.get");
      this.status = await ratsCall<FeatureStatus>("features.status");
      this.message = "";
    } catch (error) {
      this.message = String(error);
    }
  }

  async save(): Promise<void> {
    if (!this.config || !this.confirming) return;
    this.confirming = false;
    this.saving = true;
    try {
      const { networkEnabled, resumeBtSession, mdns, indexExchange, safeSearch, indexer, databaseSharing, p2pReplication } =
        this.config;
      await ratsCall("config.set", {
        networkEnabled,
        resumeBtSession,
        mdns,
        indexExchange,
        safeSearch,
        indexer,
        databaseSharing,
        p2pReplication,
      });
      await this.load();
      this.message = "已保存";
    } catch (error) {
      this.message = String(error);
    } finally {
      this.saving = false;
    }
  }
}
