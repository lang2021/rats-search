import { Component, OnInit } from "@angular/core";
import { FormsModule } from "@angular/forms";
import { AppModule } from "../app.module";
import { DocumentTitleComponent } from "../layout/document-title.component";
import { ratsCall } from "./rats-api";

type Config = {
  networkEnabled: boolean;
  independentDirect: boolean;
  resumeBtSession: boolean;
  btFileDownload: boolean;
  btFileUpload: boolean;
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
  independentDirectSupported: boolean;
  independentDirectDesired: boolean;
  independentDirectSocketBound: boolean;
  mdnsRunning: boolean;
  p2pRunning: boolean;
  btEngineReady: boolean;
  btFileDownloadAllowed: boolean;
  btFileUploadAllowed: boolean;
  btSessionRestoreDesired: boolean;
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
        <div class="status-strip" *ngIf="status as s" aria-label="当前运行状态">
          <span>网络 {{ s.networkRunning ? '运行中' : '已停止' }}</span>
          <span>DHT {{ s.dhtRunning ? '运行中' : '已停止' }}</span>
          <span>Rats P2P {{ s.p2pRunning ? '运行中' : '已停止' }}</span>
          <span *ngIf="s.restartRequired" class="attention">有设置待重启</span>
          <span *ngIf="edited" class="attention">有未保存修改</span>
        </div>
        <p *ngIf="status?.lastError" class="attention">启动错误：{{ status?.lastError }}</p>

        <mat-accordion class="settings-groups" multi>
          <mat-expansion-panel class="settings-panel" expanded>
            <mat-expansion-panel-header>
              <mat-panel-title>01 共享网络基础</mat-panel-title>
              <mat-panel-description>联网总闸与当前运行状态</mat-panel-description>
            </mat-expansion-panel-header>
            <div class="setting-row">
              <mat-checkbox [(ngModel)]="c.networkEnabled" (ngModelChange)="markEdited()">启用网络功能</mat-checkbox>
              <p class="hint">关闭后立即停止 DHT、Rats P2P 和 BT 网络；重新开启需重启。此项不会自动授权对外共享或文件传输。</p>
              <p class="state" *ngIf="status as s">配置 {{ c.networkEnabled ? '开启' : '关闭' }} · 实际 {{ s.networkRunning ? '运行中' : '已停止' }}<span *ngIf="s.restartRequired"> · 待重启</span></p>
            </div>
            <mat-expansion-panel class="advanced-panel">
              <mat-expansion-panel-header>
                <mat-panel-title>高级网络设置</mat-panel-title>
                <mat-panel-description>端口 · mDNS · DHT 直连</mat-panel-description>
              </mat-expansion-panel-header>
              <p class="state">当前配置端口：HTTP {{ c.httpPort }} · P2P {{ c.p2pPort }} · DHT {{ c.dhtPort }}</p>
              <div class="setting-row">
                <mat-checkbox [(ngModel)]="c.mdns" (ngModelChange)="markEdited()">局域网 mDNS 广播</mat-checkbox>
                <p class="state" *ngIf="status as s">实际 {{ s.mdnsRunning ? '运行中' : '已停止' }} · 更改后需重启</p>
              </div>
              <div class="setting-row" *ngIf="status as s">
                <ng-container *ngIf="s.independentDirectSupported; else unsupportedDirect">
                  <mat-checkbox [(ngModel)]="c.independentDirect" (ngModelChange)="markEdited()">DHT IPv4 独立直连（macOS）</mat-checkbox>
                  <p class="state">实际 {{ s.independentDirectSocketBound ? 'DHT IPv4 Socket 已绑定物理接口' : '未绑定' }} · 更改后需重启</p>
                </ng-container>
                <ng-template #unsupportedDirect><p class="state">此平台不支持 DHT IPv4 独立直连；已保存的配置不会在此平台生效。</p></ng-template>
              </div>
              <p class="hint">独立直连仅约束 macOS DHT IPv4；Rats P2P、BT、STUN Socket 不受约束。端口为只读配置值。</p>
            </mat-expansion-panel>
          </mat-expansion-panel>

          <mat-expansion-panel class="settings-panel">
            <mat-expansion-panel-header>
              <mat-panel-title>02 DHT 资源采集</mat-panel-title>
              <mat-panel-description>发现网络与元数据采集分开显示</mat-panel-description>
            </mat-expansion-panel-header>
            <div class="setting-row">
              <mat-checkbox [(ngModel)]="c.indexer" (ngModelChange)="markEdited()">采集 DHT 资源元数据</mat-checkbox>
              <p class="hint">关闭采集任务不等于关闭 DHT 节点网络；元数据获取不下载资源文件。</p>
              <p class="state" *ngIf="status as s">配置 {{ c.indexer ? '开启' : '关闭' }} · DHT 网络 {{ s.dhtRunning ? '运行中' : '已停止' }} · 采集任务 {{ s.crawlerRunning ? '运行中' : '已停止' }}</p>
            </div>
          </mat-expansion-panel>

          <mat-expansion-panel class="settings-panel">
            <mat-expansion-panel-header>
              <mat-panel-title>03 Rats 协作与索引授权</mat-panel-title>
              <mat-panel-description>主动查询、主动同步与对外提供各自授权</mat-panel-description>
            </mat-expansion-panel-header>
            <div class="setting-grid">
              <div class="setting-row">
                <mat-checkbox [(ngModel)]="c.remoteSearch" (ngModelChange)="markEdited()">允许向 Rats 节点发送搜索词</mat-checkbox>
                <p class="hint">Web 搜索仍默认只查本地；主动选择节点范围后才发送。</p>
                <p class="state" *ngIf="status as s">发起条件 {{ s.remoteSearchRunning ? '已具备' : '未具备' }}，不表示正在搜索</p>
              </div>
              <div class="setting-row">
                <mat-checkbox [(ngModel)]="c.p2pReplication" (ngModelChange)="markEdited()">主动获取远端 Feed 与随机索引</mat-checkbox>
                <p class="hint">不要求向外共享本地索引。</p>
                <p class="state" *ngIf="status as s">同步任务 {{ s.replicationRunning ? '运行中' : '已停止' }}</p>
              </div>
            </div>
            <div class="setting-row sharing-row">
              <mat-checkbox [(ngModel)]="c.indexExchange" (ngModelChange)="markEdited()">对外索引共享</mat-checkbox>
              <p class="hint">允许响应节点的资源搜索与详情查询。以下两项另需授权：</p>
              <div class="module-children">
                <mat-checkbox [(ngModel)]="c.p2pReplicationServer" (ngModelChange)="markEdited()" [disabled]="!c.indexExchange">提供 Feed 与随机索引（含文件列表）</mat-checkbox>
                <mat-checkbox [(ngModel)]="c.databaseSharing" (ngModelChange)="markEdited()" [disabled]="!c.indexExchange">提供完整数据库</mat-checkbox>
              </div>
              <p class="hint">关闭主授权会停止对外提供，但保留两个子授权的勾选值；再次开启时会恢复。主动搜索和主动同步不受影响。</p>
              <p class="state" *ngIf="status as s">索引应答 {{ s.indexSharingRunning ? '已启用' : '未启用' }} · 完整数据库共享 {{ s.databaseSharingRunning ? '已启用' : '未启用' }} · 对外提供任务 {{ s.indexSharingServing ? '处理中' : '无' }}</p>
              <p class="hint" *ngIf="status?.indexSharingRunning && status?.databaseSharingRunning !== c.databaseSharing">完整数据库共享的实际状态可能受启动参数覆盖，以实际状态为准。</p>
            </div>
          </mat-expansion-panel>

          <mat-expansion-panel class="settings-panel">
            <mat-expansion-panel-header>
              <mat-panel-title>04 BT 文件传输</mat-panel-title>
              <mat-panel-description>下载、上传与会话恢复独立授权</mat-panel-description>
            </mat-expansion-panel-header>
            <div class="setting-grid">
              <mat-checkbox [(ngModel)]="c.btFileDownload" (ngModelChange)="markEdited()">允许资源文件下载</mat-checkbox>
              <mat-checkbox [(ngModel)]="c.btFileUpload" (ngModelChange)="markEdited()">允许文件块上传与做种</mat-checkbox>
            </div>
            <mat-checkbox [(ngModel)]="c.resumeBtSession" (ngModelChange)="markEdited()">下次启动时自动恢复 BT 会话</mat-checkbox>
            <p class="hint">上传包括下载过程中已获得的文件块；会话恢复仍受下载与上传授权约束。关闭文件传输不影响 DHT 元数据采集。</p>
            <p class="state" *ngIf="status as s">引擎 {{ s.btEngineReady ? '已就绪' : '未就绪' }} · 实际下载 {{ s.btFileDownloadAllowed ? '允许' : '禁止' }} · 实际上传 {{ s.btFileUploadAllowed ? '允许' : '禁止' }}。引擎就绪不表示正在传输；任务活动请查看下载列表。</p>
          </mat-expansion-panel>

          <mat-expansion-panel class="settings-panel">
            <mat-expansion-panel-header>
              <mat-panel-title>05 本地偏好</mat-panel-title>
              <mat-panel-description>搜索结果显示</mat-panel-description>
            </mat-expansion-panel-header>
            <mat-checkbox [(ngModel)]="c.safeSearch" (ngModelChange)="markEdited()">安全搜索</mat-checkbox>
            <p class="hint">本地关键词搜索过滤成人分类，并向 Rats 节点请求过滤；旧版节点可能忽略。热门列表不属于关键词搜索。</p>
          </mat-expansion-panel>
        </mat-accordion>

        <p *ngIf="message" role="status">{{ message }}</p>
        <button
          mat-flat-button
          color="primary"
          (click)="confirming = true"
          [disabled]="saving"
        >
          保存设置
        </button>
        <span *ngIf="confirming" class="confirm" role="group" aria-label="确认保存设置">
          确认保存？网络关闭会立即停止相关网络活动；开启网络、mDNS 或 DHT 直连需重启。对外已发送的数据无法撤回；关闭索引共享仍保留子授权勾选值。
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
      .status-strip { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 12px; }
      .status-strip span { border: 1px solid var(--rs-border); padding: 4px 8px; color: var(--rs-text); }
      .attention { color: var(--rs-danger) !important; }
      .settings-groups { display: block; border-top: 1px solid var(--rs-border); }
      .settings-panel, .advanced-panel { border-radius: 0 !important; box-shadow: none !important; border: 1px solid var(--rs-border); border-top: 0; background: var(--rs-surface); color: var(--rs-text); }
      .settings-panel mat-expansion-panel-header { padding: 0 14px; }
      .settings-panel mat-panel-title { color: var(--rs-accent); font-size: 11px; letter-spacing: .15em; }
      .settings-panel mat-panel-description { color: var(--rs-muted); font-size: 11px; }
      .setting-row { padding: 8px 0; border-top: 1px solid var(--rs-border); }
      .setting-row:first-child { border-top: 0; }
      .setting-grid { display: grid; grid-template-columns: repeat(2,minmax(0,1fr)); gap: 0 16px; }
      .setting-grid > mat-checkbox { padding: 8px 0; }
      .hint, .state { margin: 3px 0 5px 28px; color: var(--rs-muted); line-height: 1.5; }
      .state { color: var(--rs-text); }
      .module-children { display: flex; flex-wrap: wrap; gap: 8px 20px; margin: 6px 0 6px 28px; }
      .advanced-panel { margin: 8px 0 0 !important; border-top: 1px solid var(--rs-border); background: var(--rs-raised); }
      .advanced-panel mat-panel-title { color: var(--rs-text); letter-spacing: normal; }
      .advanced-panel .setting-row { margin: 0 12px; }
      .advanced-panel .state, .advanced-panel .hint { margin-left: 12px; }
      .confirm { display: inline-flex; align-items: center; gap: 8px; flex-wrap: wrap; margin: 8px 0; }
      @media (max-width:600px) { .setting-grid { grid-template-columns: 1fr; } .settings-panel mat-panel-description { display: none; } }
    `,
  ],
})
export class SettingsComponent implements OnInit {
  config?: Config;
  status?: FeatureStatus;
  message = "";
  saving = false;
  confirming = false;
  edited = false;

  markEdited(): void {
    this.edited = true;
    this.message = "";
  }

  ngOnInit(): void {
    void this.load();
  }

  async load(): Promise<void> {
    try {
      this.config = await ratsCall<Config>("config.get");
      this.status = await ratsCall<FeatureStatus>("features.status");
      this.message = "";
      this.edited = false;
    } catch (error) {
      this.message = String(error);
    }
  }

  async save(): Promise<void> {
    if (!this.config || !this.confirming) return;
    this.confirming = false;
    this.saving = true;
    try {
      const { networkEnabled, independentDirect, resumeBtSession, btFileDownload, btFileUpload, mdns, indexExchange, remoteSearch, safeSearch, indexer, databaseSharing, p2pReplication, p2pReplicationServer } =
        this.config;
      await ratsCall("config.set", {
        networkEnabled,
        ...(this.status?.independentDirectSupported ? { independentDirect } : {}),
        resumeBtSession,
        btFileDownload,
        btFileUpload,
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
