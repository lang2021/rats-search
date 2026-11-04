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
  remoteSearch: boolean;
  safeSearch: boolean;
  indexer: boolean;
  databaseSharing: boolean;
  p2pReplication: boolean;
  p2pReplicationServer: boolean;
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
  indexSharingRunning: boolean;
  indexSharingServing: boolean;
  remoteSearchRunning: boolean;
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
        <p *ngIf="status as s">网络：{{ s.networkRunning ? '运行中' : '已停止' }} · DHT {{ s.dhtRunning ? '运行' : '停止' }} · P2P {{ s.p2pRunning ? '运行' : '停止' }} · BT {{ s.btEngineReady ? '就绪' : '停止' }} · 采集 {{ s.crawlerRunning ? '运行' : '停止' }} · 索引共享 {{ s.indexSharingRunning ? '已启用' : '已停止' }} · 节点搜索 {{ s.remoteSearchRunning ? '已启用' : '已停止' }} · 复制任务 {{ s.replicationRunning ? '运行' : '停止' }} · mDNS {{ s.mdnsRunning ? '运行' : '停止' }} · 数据库共享 {{ s.databaseSharingRunning ? '允许' : '停止' }}<span *ngIf="s.indexSharingServing"> · 正在处理共享任务</span><span *ngIf="s.restartRequired"> · 启用网络需重启程序</span><span *ngIf="s.lastError"> · {{ s.lastError }}</span></p>
        <section class="module-control" aria-label="对外索引共享">
          <mat-checkbox [(ngModel)]="c.indexExchange">对外索引共享</mat-checkbox>
          <p>依赖 Rats P2P。启用后响应其他节点的资源搜索与详情查询；不会发送你的搜索词。Feed、随机条目与完整数据库另需授权。关闭后取消相关对外共享，不影响主动同步；已发送的数据无法撤回。</p>
          <div class="module-children">
            <mat-checkbox [(ngModel)]="c.p2pReplicationServer" [disabled]="!c.indexExchange">允许节点获取 Feed 与随机索引条目（含文件列表）</mat-checkbox>
            <mat-checkbox [(ngModel)]="c.databaseSharing" [disabled]="!c.indexExchange">允许其他节点获取完整数据库</mat-checkbox>
          </div>
          <p *ngIf="status?.indexSharingRunning && status?.databaseSharingRunning !== c.databaseSharing">完整数据库共享的实际状态受启动参数覆盖，以状态栏为准；关闭主开关仍会停止共享。</p>
        </section>
        <section class="module-control" aria-label="主动索引同步">
          <mat-checkbox [(ngModel)]="c.p2pReplication">主动获取远端 Feed 与随机索引条目</mat-checkbox>
          <p>依赖 Rats P2P，不要求向其他节点共享本地索引；关闭后停止自动拉取。手动请求完整数据库仍是单独的操作。</p>
        </section>
        <section class="module-control" aria-label="Rats 节点搜索">
          <mat-checkbox [(ngModel)]="c.remoteSearch">向 Rats 节点发送搜索词（桌面客户端）</mat-checkbox>
          <p>与索引共享授权独立；仅桌面客户端的关键词搜索会广播到已连接节点。当前 Web 搜索仍只查询本地索引。关闭此项不影响本地搜索或按哈希发起的 DHT 查询。</p>
        </section>
        <div class="options">
          <mat-checkbox [(ngModel)]="c.networkEnabled">网络总闸（过渡控制，开启需重启）</mat-checkbox>
          <mat-checkbox [(ngModel)]="c.safeSearch">安全搜索</mat-checkbox>
          <mat-checkbox [(ngModel)]="c.resumeBtSession">保存并恢复 BT 下载与做种会话</mat-checkbox>
          <mat-checkbox [(ngModel)]="c.mdns">局域网 mDNS 广播（重启生效）</mat-checkbox>
          <mat-checkbox [(ngModel)]="c.indexer">DHT 索引爬取</mat-checkbox>
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
          对外共享、主动同步、节点搜索和各项子授权会立即生效；对外已发送的数据无法撤回。网络总闸关闭会立即停止所有网络模块，重新开启需重启程序。
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
      const { networkEnabled, resumeBtSession, mdns, indexExchange, remoteSearch, safeSearch, indexer, databaseSharing, p2pReplication, p2pReplicationServer } =
        this.config;
      await ratsCall("config.set", {
        networkEnabled,
        resumeBtSession,
        mdns,
        indexExchange,
        remoteSearch,
        safeSearch,
        indexer,
        databaseSharing,
        p2pReplication,
        p2pReplicationServer,
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
