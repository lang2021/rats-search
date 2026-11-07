import { Component, ElementRef, OnDestroy, OnInit, ViewChild } from "@angular/core";
import { FormsModule } from "@angular/forms";
import { AppModule } from "../app.module";
import { DocumentTitleComponent } from "../layout/document-title.component";
import { clearRatsAuthToken, ratsAuthToken, ratsCall } from "./rats-api";

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
  searchHistory: boolean;
  indexer: boolean;
  databaseSharing: boolean;
  p2pReplication: boolean;
  p2pReplicationServer: boolean;
  httpPort: number;
  p2pPort: number;
  dhtPort: number;
};

type SettingKey = { [K in keyof Config]: Config[K] extends boolean ? K : never }[keyof Config];
type PendingChange = { key: SettingKey; value: boolean; previous: boolean; prompt: string };
type RuntimeAction = "start-network" | "stop-network" | "start-crawler" | "stop-crawler";

const settingLabels: Record<SettingKey, string> = {
  networkEnabled: "网络功能",
  independentDirect: "DHT IPv4 独立直连",
  resumeBtSession: "自动恢复 BT 会话",
  btFileDownload: "资源文件下载",
  btFileUpload: "文件块上传与做种",
  mdns: "mDNS 广播",
  indexExchange: "对外索引共享",
  remoteSearch: "Rats 远程搜索",
  safeSearch: "安全搜索",
  searchHistory: "搜索历史",
  indexer: "DHT 元数据采集",
  databaseSharing: "完整数据库共享",
  p2pReplication: "主动获取远端索引",
  p2pReplicationServer: "提供 Feed 与随机索引",
};

const resettableDefaults: Partial<Config> = {
  networkEnabled: true,
  indexer: true,
  remoteSearch: true,
  indexExchange: true,
  btFileDownload: true,
  safeSearch: true,
  p2pReplication: false,
  p2pReplicationServer: false,
  databaseSharing: false,
  btFileUpload: false,
  resumeBtSession: false,
  mdns: false,
  independentDirect: false,
};

type FeatureStatus = {
  networkDesired: boolean;
  networkRunning: boolean;
  networkState: "stopped" | "starting" | "running" | "stopping" | "failed";
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
  crawlerState: "stopped" | "starting" | "running" | "stopping" | "failed";
  crawlerPendingFetches: number;
  crawlerConfigured: boolean;
  crawlerForcedByCli: boolean;
  crawlerOverrideActive: boolean;
  crawlerLastError: string;
  replicationRunning: boolean;
  databaseSharingRunning: boolean;
  indexExchangeRunning: boolean;
  indexSharingRunning: boolean;
  indexSharingServing: boolean;
  remoteSearchRunning: boolean;
  peerCount: number;
  lastError: string;
};

type P2pStats = { dhtNodes: number; peerCount: number };

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
        <section class="runtime-panel" *ngIf="status as s" aria-label="运行控制">
          <h2>运行控制</h2>
          <div class="runtime-grid">
            <div class="runtime-card">
              <strong>共享网络</strong>
              <span role="status" [class.attention]="s.networkState === 'failed'">{{ networkStateLabel(s.networkState) }}</span>
              <div class="runtime-actions">
                <button mat-flat-button color="primary" (click)="requestRuntime('start-network')" [disabled]="runtimeDisabled || s.networkState === 'running' || s.networkState === 'starting' || s.networkState === 'stopping'">启动网络</button>
                <button mat-stroked-button (click)="requestRuntime('stop-network')" [disabled]="runtimeDisabled || s.networkState !== 'running'">停止网络</button>
              </div>
            </div>
            <div class="runtime-card">
              <strong>DHT Socket</strong>
              <span role="status">{{ s.dhtRunning ? '运行中' : '已停止' }}</span>
              <small>已发现节点 {{ p2pStats?.dhtNodes ?? '—' }}；0 个节点不代表 Socket 启动失败</small>
            </div>
            <div class="runtime-card">
              <strong>Rats P2P</strong>
              <span role="status">{{ s.p2pRunning ? '运行中' : '已停止' }}</span>
              <small>当前连接 {{ s.peerCount }} 个节点</small>
            </div>
            <div class="runtime-card">
              <strong>DHT 资源采集</strong>
              <span role="status" [class.attention]="s.crawlerState === 'failed'">{{ crawlerStateLabel(s.crawlerState) }}</span>
              <small *ngIf="s.crawlerState === 'stopping'">{{ s.crawlerPendingFetches }} 个元数据请求待结束；期间可能仍有网络通信</small>
              <div class="runtime-actions">
                <button mat-flat-button color="primary" (click)="requestRuntime('start-crawler')" [disabled]="runtimeDisabled || s.networkState === 'starting' || s.networkState === 'stopping' || s.crawlerState === 'running' || s.crawlerState === 'starting' || s.crawlerState === 'stopping'">启动采集</button>
                <button mat-stroked-button (click)="requestRuntime('stop-crawler')" [disabled]="runtimeDisabled || s.crawlerState !== 'running'">停止采集</button>
              </div>
            </div>
          </div>
          <p class="runtime-note">启动共享网络可能触发 DHT Bootstrap、STUN、PEX 和已知节点重连；不会自动授权文件传输或对外索引共享。停止采集不会停止仍在使用的共享网络。</p>
          <p *ngIf="s.lastError" class="attention" role="alert">{{ s.networkState === 'failed' ? '网络启动错误' : '网络提示' }}：{{ s.lastError }}</p>
          <p *ngIf="s.crawlerLastError" class="attention" role="alert">采集错误：{{ s.crawlerLastError }}</p>
          <p *ngIf="s.restartRequired" class="attention" role="status">高级网络配置已保存，待重新启动网络生效</p>
          <p *ngIf="saving || runtimeBusy || retry" role="status" [class.attention]="!!retry">{{ runtimeBusy ? runtimeBusyLabel() : saving ? '正在保存设置' : '有设置未持久化' }}</p>
        </section>

        <mat-accordion class="settings-groups" multi>
          <mat-expansion-panel class="settings-panel" expanded>
            <mat-expansion-panel-header>
              <mat-panel-title>01 行为授权</mat-panel-title>
              <mat-panel-description>允许哪些网络行为；不表示正在运行</mat-panel-description>
            </mat-expansion-panel-header>
            <p class="group-hint">授权更改可立即影响行为；主动同步还会启停后台服务。是否正在执行请看实际状态或任务页面。</p>
            <div class="setting-grid">
              <div class="setting-row">
                <mat-slide-toggle [(ngModel)]="c.remoteSearch" (ngModelChange)="change('remoteSearch', $event)" [disabled]="controlsDisabled">允许向 Rats 节点发送搜索词</mat-slide-toggle>
                <p class="hint">Web 搜索仍默认只查本地；主动选择节点范围后才发送。</p>
                <p class="state" *ngIf="status as s">发起条件 {{ s.remoteSearchRunning ? '已具备' : '未具备' }}，不表示正在搜索</p>
              </div>
              <div class="setting-row">
                <mat-slide-toggle [(ngModel)]="c.p2pReplication" (ngModelChange)="change('p2pReplication', $event)" [disabled]="controlsDisabled">主动获取远端 Feed 与随机索引</mat-slide-toggle>
                <p class="hint">网络已运行时，开启会启动后台同步服务，关闭会停止；不要求向外共享本地索引。</p>
                <p class="state" *ngIf="status as s">自动同步服务 {{ s.replicationRunning ? '运行中' : '已停止' }}；不表示正在传输</p>
              </div>
            </div>
            <div class="setting-row sharing-row">
              <mat-slide-toggle [(ngModel)]="c.indexExchange" (ngModelChange)="change('indexExchange', $event)" [disabled]="controlsDisabled">对外索引共享</mat-slide-toggle>
              <p class="hint">允许响应节点的资源搜索与详情查询。以下两项另需授权：</p>
              <div class="module-children">
                <mat-slide-toggle [(ngModel)]="c.p2pReplicationServer" (ngModelChange)="change('p2pReplicationServer', $event)" [disabled]="controlsDisabled || !c.indexExchange">提供 Feed 与随机索引（含文件列表）</mat-slide-toggle>
                <mat-slide-toggle [(ngModel)]="c.databaseSharing" (ngModelChange)="change('databaseSharing', $event)" [disabled]="controlsDisabled || !c.indexExchange">提供完整数据库</mat-slide-toggle>
              </div>
              <p class="hint">关闭主授权会停止对外提供，但保留两个子授权的勾选值；再次开启时会恢复。主动搜索和主动同步不受影响。</p>
              <p class="state" *ngIf="status as s">索引应答 {{ s.indexSharingRunning ? '已启用' : '未启用' }} · 完整数据库共享 {{ s.databaseSharingRunning ? '已启用' : '未启用' }} · 对外提供任务 {{ s.indexSharingServing ? '处理中' : '无' }}</p>
              <p class="hint" *ngIf="status?.indexSharingRunning && status?.databaseSharingRunning !== c.databaseSharing">完整数据库共享的实际状态可能受启动参数覆盖，以实际状态为准。</p>
            </div>
            <h3 class="group-subtitle">BT 文件传输授权</h3>
            <div class="setting-grid">
              <mat-slide-toggle [(ngModel)]="c.btFileDownload" (ngModelChange)="change('btFileDownload', $event)" [disabled]="controlsDisabled">允许资源文件下载</mat-slide-toggle>
              <mat-slide-toggle [(ngModel)]="c.btFileUpload" (ngModelChange)="change('btFileUpload', $event)" [disabled]="controlsDisabled">允许文件块上传与做种</mat-slide-toggle>
            </div>
            <p class="hint">上传包括下载过程中已获得的文件块。关闭文件传输不影响 DHT 元数据采集。</p>
            <p class="state" *ngIf="status as s">引擎 {{ s.btEngineReady ? '已就绪' : '未就绪' }} · 实际下载 {{ s.btFileDownloadAllowed ? '允许' : '禁止' }} · 实际上传 {{ s.btFileUploadAllowed ? '允许' : '禁止' }}。引擎就绪不表示正在传输；任务活动请查看下载列表。</p>
          </mat-expansion-panel>

          <mat-expansion-panel class="settings-panel">
            <mat-expansion-panel-header>
              <mat-panel-title>02 启动偏好</mat-panel-title>
              <mat-panel-description>下次启动生效；不控制本次运行</mat-panel-description>
            </mat-expansion-panel-header>
            <div class="setting-row">
              <mat-slide-toggle [(ngModel)]="c.networkEnabled" (ngModelChange)="change('networkEnabled', $event)" [disabled]="controlsDisabled">下次启动程序时自动启动共享网络</mat-slide-toggle>
              <p class="state">已配置 {{ confirmedConfig?.networkEnabled ? '开启' : '关闭' }}；当前网络 {{ status ? networkStateLabel(status.networkState) : '未知' }}</p>
            </div>
            <div class="setting-row">
              <mat-slide-toggle [(ngModel)]="c.indexer" (ngModelChange)="change('indexer', $event)" [disabled]="controlsDisabled">下次启动共享网络时自动采集</mat-slide-toggle>
              <p class="state">已配置 {{ confirmedConfig?.indexer ? '开启' : '关闭' }}；当前采集 {{ status ? crawlerStateLabel(status.crawlerState) : '未知' }}<span *ngIf="status?.crawlerForcedByCli">；--spider 启动参数会在下次启动网络时启用采集</span><span *ngIf="status?.crawlerOverrideActive">；本次运行已手动控制</span></p>
            </div>
            <div class="setting-row">
              <mat-slide-toggle [(ngModel)]="c.resumeBtSession" (ngModelChange)="change('resumeBtSession', $event)" [disabled]="controlsDisabled">下次启动程序时自动恢复 BT 会话</mat-slide-toggle>
              <p class="hint">恢复仍受下载、上传授权及任务原本的暂停状态约束。</p>
            </div>
          </mat-expansion-panel>

          <mat-expansion-panel class="settings-panel">
            <mat-expansion-panel-header>
              <mat-panel-title>03 高级网络配置</mat-panel-title>
              <mat-panel-description>端口 · 局域网发现 · DHT IPv4 直连</mat-panel-description>
            </mat-expansion-panel-header>
            <p class="state">当前配置端口：HTTP {{ c.httpPort }} · P2P {{ c.p2pPort }} · DHT {{ c.dhtPort }}；端口为只读配置值</p>
            <div class="setting-row">
              <mat-slide-toggle [(ngModel)]="c.mdns" (ngModelChange)="change('mdns', $event)" [disabled]="controlsDisabled">允许局域网 mDNS 广播</mat-slide-toggle>
              <p class="state" *ngIf="status as s">当前 {{ s.mdnsRunning ? '运行中' : '已停止' }}；更改后需重新启动网络</p>
            </div>
            <div class="setting-row" *ngIf="status as s">
              <ng-container *ngIf="s.independentDirectSupported; else unsupportedDirect">
                <mat-slide-toggle [(ngModel)]="c.independentDirect" (ngModelChange)="change('independentDirect', $event)" [disabled]="controlsDisabled">DHT IPv4 独立直连（macOS）</mat-slide-toggle>
                <p class="state">当前 {{ s.independentDirectSocketBound ? 'DHT IPv4 Socket 已绑定物理接口' : '未绑定' }}；更改后需重新启动网络</p>
              </ng-container>
              <ng-template #unsupportedDirect><p class="state">此平台不支持 DHT IPv4 独立直连；已保存的配置不会在此平台生效。</p></ng-template>
            </div>
            <p class="hint">独立直连仅约束 macOS DHT IPv4；Rats P2P、BT、STUN Socket 不受约束。</p>
          </mat-expansion-panel>

          <mat-expansion-panel class="settings-panel">
            <mat-expansion-panel-header>
              <mat-panel-title>04 本地偏好</mat-panel-title>
              <mat-panel-description>搜索结果与历史记录</mat-panel-description>
            </mat-expansion-panel-header>
            <mat-slide-toggle [(ngModel)]="c.safeSearch" (ngModelChange)="change('safeSearch', $event)" [disabled]="controlsDisabled">安全搜索</mat-slide-toggle>
            <p class="hint">本地关键词搜索过滤成人分类，并向 Rats 节点请求过滤；旧版节点可能忽略。热门列表不属于关键词搜索。</p>
            <div class="setting-row">
              <mat-slide-toggle [(ngModel)]="c.searchHistory" (ngModelChange)="change('searchHistory', $event)" [disabled]="controlsDisabled">记录搜索历史</mat-slide-toggle>
              <p class="hint">关闭后停止新增历史记录；已有历史不会自动删除。</p>
            </div>
          </mat-expansion-panel>
        </mat-accordion>

        <p *ngIf="message" role="status" [class.attention]="messageIsError">{{ message }}</p>
        <button mat-stroked-button (click)="requestResetDefaults()" [disabled]="controlsDisabled">恢复默认配置</button>
        <button mat-button type="button" (click)="forgetApiToken()">忘记 API 令牌</button>
        <dialog *ngIf="pending as change" #confirmDialog class="confirm" aria-label="确认更改设置" (cancel)="cancelChange()">
          <span>{{ change.prompt }}</span>
          <button mat-flat-button color="primary" autofocus (click)="confirmChange()">确认更改</button>
          <button mat-button (click)="cancelChange()">取消</button>
        </dialog>
        <dialog *ngIf="pendingResetDefaults" #resetDialog class="confirm" aria-label="确认恢复默认配置" (cancel)="$event.preventDefault(); cancelResetDefaults()">
          <span>将重置 13 项：开启远程搜索、普通索引查询应答、资源文件下载和安全搜索；关闭主动 Feed/随机索引同步、Feed 与随机索引提供、完整数据库共享、BT 上传与做种、BT 会话恢复、mDNS 和 DHT Independent Direct。并设置为下次启动程序时自动启动共享网络、下次启动共享网络时自动启动 DHT 采集。开启普通索引查询意味着允许其他 Rats 节点查询本机索引；允许文件下载意味着你明确添加下载任务后才可下载资源文件。以上网络和采集设置只改启动偏好，本次不会启动网络或 Crawler。其他设置、API 令牌、节点身份、数据库及下载文件保持不变。确认恢复？</span>
          <button mat-flat-button color="primary" autofocus (click)="confirmResetDefaults()">恢复这 13 项</button>
          <button mat-button (click)="cancelResetDefaults()">取消</button>
        </dialog>
        <dialog *ngIf="pendingRuntime as action" #runtimeDialog class="confirm" aria-label="确认网络操作" (cancel)="$event.preventDefault(); cancelRuntime()">
          <span>{{ action.prompt }}</span>
          <button mat-flat-button color="primary" autofocus (click)="confirmRuntime()">确认操作</button>
          <button mat-button (click)="cancelRuntime()">取消</button>
        </dialog>
        <button *ngIf="retry as failed" mat-stroked-button (click)="retrySave()" [disabled]="saving">重试保存「{{ settingLabel(failed.key) }}」</button>
        <button *ngIf="retryResetDefaults" mat-stroked-button (click)="retryReset()" [disabled]="saving">重试恢复默认配置</button>
        <button mat-button (click)="load()" [disabled]="saving || !!pending">刷新状态</button>
      </mat-card-content>
      <mat-card-content *ngIf="!config">{{
        message || "读取中…"
      }}</mat-card-content>
    </mat-card>
  `,
  styles: [
    `
      .page { margin: 0; }
      .page mat-card-header { padding: 11px 16px; background: var(--rs-raised); border-bottom: 1px solid var(--rs-border); }
      .page mat-card-title { font-size: 11px; letter-spacing: .22em; color: var(--rs-accent); }
      .page mat-card-title mat-icon { font-size: 17px; width: 18px; height: 18px; vertical-align: middle; }
      mat-card-content { padding: 16px; color: var(--rs-muted); font-size: 12px; }
      .runtime-panel { border: 1px solid var(--rs-border); margin-bottom: 14px; color: var(--rs-text); }
      .runtime-panel h2 { margin: 0; padding: 10px 14px; background: var(--rs-raised); border-bottom: 1px solid var(--rs-border); color: var(--rs-accent); font-size: 11px; letter-spacing: .15em; }
      .runtime-panel > p { margin: 9px 14px; line-height: 1.5; }
      .runtime-grid { display: grid; grid-template-columns: repeat(4,minmax(0,1fr)); }
      .runtime-card { display: flex; flex-direction: column; gap: 7px; min-width: 0; padding: 12px 14px; border-right: 1px solid var(--rs-border); }
      .runtime-card:last-child { border-right: 0; }
      .runtime-card strong { color: var(--rs-muted); font-size: 11px; font-weight: 500; }
      .runtime-card > span { color: var(--rs-accent); font-size: 17px; }
      .runtime-card small { color: var(--rs-muted); line-height: 1.4; }
      .runtime-note { border-top: 1px solid var(--rs-border); color: var(--rs-muted); }
      .attention { color: var(--rs-danger) !important; }
      .settings-groups { display: block; border-top: 1px solid var(--rs-border); }
      .settings-panel, .advanced-panel { border-radius: 0 !important; box-shadow: none !important; border: 1px solid var(--rs-border); border-top: 0; background: var(--rs-surface); color: var(--rs-text); }
      .settings-panel mat-expansion-panel-header { padding: 0 14px; }
      .settings-panel mat-panel-title { color: var(--rs-accent); font-size: 11px; letter-spacing: .15em; }
      .settings-panel mat-panel-description { color: var(--rs-muted); font-size: 11px; }
      .setting-row { padding: 8px 0; border-top: 1px solid var(--rs-border); }
      .runtime-actions { display: flex; flex-wrap: wrap; align-items: center; gap: 8px; margin-bottom: 10px; color: var(--rs-text); }
      .runtime-actions button { margin-right: 0; }
      .setting-row:first-child { border-top: 0; }
      .group-hint { margin: 10px 0; color: var(--rs-muted); }
      .group-subtitle { margin: 12px 0 0; padding: 9px 0; border-top: 1px solid var(--rs-border); color: var(--rs-accent); font-size: 11px; letter-spacing: .1em; }
      .setting-grid { display: grid; grid-template-columns: repeat(2,minmax(0,1fr)); gap: 0 16px; }
      .setting-grid > mat-slide-toggle { padding: 8px 0; }
      mat-slide-toggle {
        --mdc-switch-selected-track-color: var(--rs-accent);
        --mdc-switch-selected-handle-color: var(--rs-on-accent);
        --mdc-switch-selected-hover-track-color: var(--rs-accent);
        --mdc-switch-selected-focus-track-color: var(--rs-accent);
        --mdc-switch-unselected-track-color: var(--rs-raised);
        --mdc-switch-unselected-handle-color: var(--rs-muted);
        --mdc-switch-unselected-hover-track-color: var(--rs-raised);
        --mdc-switch-unselected-focus-track-color: var(--rs-raised);
        --mdc-switch-unselected-outline-color: var(--rs-border);
        --mat-switch-label-text-color: var(--rs-text);
        --mat-switch-selected-focus-state-layer-color: var(--rs-accent);
        --mat-switch-unselected-focus-state-layer-color: var(--rs-accent);
      }
      .hint, .state { margin: 3px 0 5px 28px; color: var(--rs-muted); line-height: 1.5; }
      .state { color: var(--rs-text); }
      .module-children { display: flex; flex-wrap: wrap; gap: 8px 20px; margin: 6px 0 6px 28px; }
      button { border-radius: 0; margin-right: 8px; }
      .confirm { box-sizing: border-box; width: min(460px,calc(100% - 32px)); padding: 18px; border: 1px solid var(--rs-accent); background: var(--rs-surface); color: var(--rs-text); box-shadow: 0 12px 36px rgba(0,0,0,.35); }
      .confirm::backdrop { background: rgba(0,0,0,.7); }
      .confirm span { display: block; margin-bottom: 16px; line-height: 1.6; }
      @media (max-width:1100px) { .runtime-grid { grid-template-columns: repeat(2,minmax(0,1fr)); } .runtime-card:nth-child(2) { border-right: 0; } .runtime-card:nth-child(-n+2) { border-bottom: 1px solid var(--rs-border); } }
      @media (max-width:600px) { .setting-grid, .runtime-grid { grid-template-columns: 1fr; } .runtime-card { border-right: 0; border-bottom: 1px solid var(--rs-border); } .runtime-card:last-child { border-bottom: 0; } .settings-panel mat-panel-description { display: none; } }
    `,
  ],
})
export class SettingsComponent implements OnInit, OnDestroy {
  @ViewChild("confirmDialog") confirmDialog?: ElementRef<HTMLDialogElement>;
  @ViewChild("runtimeDialog") runtimeDialog?: ElementRef<HTMLDialogElement>;
  @ViewChild("resetDialog") resetDialog?: ElementRef<HTMLDialogElement>;

  config?: Config;
  confirmedConfig?: Config;
  status?: FeatureStatus;
  p2pStats?: P2pStats;
  message = "";
  messageIsError = false;
  saving = false;
  pending?: PendingChange;
  pendingRuntime?: { action: RuntimeAction; prompt: string };
  pendingResetDefaults = false;
  runtimeBusy?: RuntimeAction;
  retry?: { key: SettingKey; value: boolean };
  retryResetDefaults = false;
  private previousFocus?: HTMLElement;
  private statusPoll?: ReturnType<typeof setInterval>;

  get controlsDisabled(): boolean {
    return this.saving || !!this.pending || !!this.pendingResetDefaults || !!this.pendingRuntime || !!this.runtimeBusy || !!this.retry || this.retryResetDefaults;
  }

  get runtimeDisabled(): boolean {
    return this.saving || !!this.pending || !!this.pendingRuntime || !!this.runtimeBusy;
  }

  networkStateLabel(state: FeatureStatus["networkState"]): string {
    return ({ stopped: "已停止", starting: "正在启动", running: "运行中", stopping: "正在停止", failed: "启动失败" })[state];
  }

  crawlerStateLabel(state: FeatureStatus["crawlerState"]): string {
    return ({ stopped: "已停止", starting: "正在启动", running: "运行中", stopping: "正在停止", failed: "启动失败" })[state];
  }

  runtimeBusyLabel(): string {
    return ({ "start-network": "正在启动网络", "stop-network": "正在停止网络", "start-crawler": "正在启动采集", "stop-crawler": "正在停止采集" })[this.runtimeBusy ?? "start-network"];
  }

  requestRuntime(action: RuntimeAction): void {
    if (!this.status || this.runtimeDisabled) return;
    const prompt = action === "start-network"
      ? "启动共享网络会打开 DHT、Rats P2P 和 BT 底层，并可能进行 DHT Bootstrap、STUN、PEX 与已知节点重连。不会改变文件传输、远程搜索或对外共享授权。确认启动？"
      : action === "stop-network"
        ? "停止共享网络将停止 DHT、Rats P2P、BT 连接及相关任务；本地数据库和搜索保持可用。确认停止？"
        : action === "start-crawler" && !this.status.networkRunning
          ? "采集需要共享网络。确认先启动共享网络，再启动采集？启动网络可能进行 DHT Bootstrap、STUN、PEX 与已知节点重连；不会改变文件传输或对外共享授权。"
          : undefined;
    if (prompt) {
      this.previousFocus = document.activeElement as HTMLElement;
      this.pendingRuntime = { action, prompt };
      setTimeout(() => this.runtimeDialog?.nativeElement.showModal(), 0);
    } else {
      void this.performRuntime(action);
    }
  }

  confirmRuntime(): void {
    if (!this.pendingRuntime) return;
    const action = this.pendingRuntime.action;
    this.pendingRuntime = undefined;
    void this.performRuntime(action);
  }

  cancelRuntime(): void {
    this.pendingRuntime = undefined;
    this.restoreFocus();
  }

  private async performRuntime(action: RuntimeAction): Promise<void> {
    this.runtimeBusy = action;
    this.message = "";
    try {
      if (action === "start-crawler" && !this.status?.networkRunning)
        this.status = await ratsCall<FeatureStatus>("network.start");
      const method = ({ "start-network": "network.start", "stop-network": "network.stop", "start-crawler": "crawler.start", "stop-crawler": "crawler.stop" })[action];
      this.status = await ratsCall<FeatureStatus>(method);
      if (action === "start-crawler" && this.status.crawlerState !== "running")
        throw new Error(this.status.crawlerLastError || "采集没有进入运行状态");
      this.message = action === "stop-crawler" && this.status.crawlerState === "stopping"
        ? "采集已停止接收新任务，正在等待已有元数据请求结束。"
        : "运行操作已完成";
      this.messageIsError = false;
    } catch (error) {
      try { await this.refreshRuntimeStatus(); } catch { /* Preserve the last known status. */ }
      this.message = `运行操作失败：${String(error)}`;
      this.messageIsError = true;
    } finally {
      this.runtimeBusy = undefined;
      this.restoreFocus();
    }
  }

  settingLabel(key: SettingKey): string {
    return settingLabels[key];
  }

  change(key: SettingKey, value: boolean): void {
    if (!this.config || !this.confirmedConfig) return;
    const previous = this.confirmedConfig[key];
    if (this.controlsDisabled) {
      this.config = { ...this.config, [key]: previous };
      return;
    }
    if (previous === value) return;
    this.message = "";
    this.messageIsError = false;
    const prompt = this.confirmationPrompt(key, value);
    if (prompt) {
      this.previousFocus = document.activeElement as HTMLElement;
      this.pending = { key, value, previous, prompt };
      setTimeout(() => this.confirmDialog?.nativeElement.showModal(), 0);
      return;
    }
    void this.persist(key, value);
  }

  confirmChange(): void {
    if (!this.pending) return;
    const { key, value } = this.pending;
    this.pending = undefined;
    void this.persist(key, value);
  }

  cancelChange(): void {
    if (!this.pending || !this.config) return;
    const { key, previous } = this.pending;
    this.config = { ...this.config, [key]: previous };
    this.pending = undefined;
    this.restoreFocus();
  }

  private restoreFocus(): void {
    const control = this.previousFocus;
    this.previousFocus = undefined;
    if (control) setTimeout(() => control.focus(), 0);
  }

  retrySave(): void {
    if (!this.retry || this.saving) return;
    void this.persist(this.retry.key, this.retry.value);
  }

  requestResetDefaults(): void {
    if (this.controlsDisabled) return;
    this.previousFocus = document.activeElement as HTMLElement;
    this.pendingResetDefaults = true;
    setTimeout(() => this.resetDialog?.nativeElement.showModal(), 0);
  }

  cancelResetDefaults(): void {
    this.pendingResetDefaults = false;
    this.restoreFocus();
  }

  confirmResetDefaults(): void {
    if (!this.pendingResetDefaults) return;
    this.pendingResetDefaults = false;
    void this.persistResetDefaults();
  }

  retryReset(): void {
    if (!this.retryResetDefaults || this.saving) return;
    void this.persistResetDefaults();
  }

  private async persistResetDefaults(): Promise<void> {
    this.saving = true;
    this.message = "";
    let saved = false;
    try {
      await ratsCall("config.set", resettableDefaults);
      saved = true;
      this.retryResetDefaults = false;
      this.retry = undefined;
      await this.refreshState();
      this.message = this.status?.restartRequired
        ? "13 项默认配置已保存。已授权项按现有规则即时生效；网络与采集启动偏好下次启动时生效。"
        : "13 项默认配置已保存。已授权项按现有规则即时生效；启动偏好不会启动当前网络或采集。";
      this.messageIsError = false;
    } catch (error) {
      if (!saved) {
        this.retryResetDefaults = true;
        this.retry = undefined;
        try { await this.refreshState(); } catch { /* Keep the last known values. */ }
        this.message = `恢复默认配置未能确认写盘：${String(error)}。配置可能已在运行时应用但未持久化；可重试整组保存。`;
      } else {
        this.retryResetDefaults = false;
        this.message = `默认配置已保存，但状态刷新失败：${String(error)}。请刷新状态。`;
      }
      this.messageIsError = true;
    } finally {
      this.saving = false;
      this.restoreFocus();
    }
  }

  private confirmationPrompt(key: SettingKey, value: boolean): string | undefined {
    if (key === "networkEnabled")
      return value
        ? "保存后，下次启动程序会自动启动共享网络；本次不会立即联网，也不会授权索引共享或文件传输。确认保存？"
        : "关闭自动启动偏好不会停止当前网络；需要停止当前网络请使用“停止网络”。确认保存？";
    if (!value) return undefined;
    switch (key) {
      case "mdns": return "开启 mDNS 后，下次启动会向局域网广播设备地址和名称。确认保存？";
      case "independentDirect": return "开启后，macOS DHT IPv4 将在下次启动时使用物理接口直连，外部节点可能看到真实公网 IP。其他 Socket 不受此设置约束。确认保存？";
      case "indexExchange": {
        const restored = [
          this.config?.p2pReplicationServer ? "提供 Feed 与随机索引" : "",
          this.config?.databaseSharing ? "提供完整数据库" : "",
        ].filter(Boolean);
        return `开启对外索引共享将允许响应远端搜索与详情查询。${restored.length ? `已保留的子授权也会恢复：${restored.join("、")}。` : ""}确认保存？`;
      }
      case "p2pReplicationServer": return "允许向其他 Rats 节点提供 Feed 与随机索引（含文件列表）。确认保存？";
      case "databaseSharing": return "允许向其他 Rats 节点提供完整数据库。确认保存？";
      case "btFileUpload": return "允许上传已获得的资源文件块及做种；已有任务也会受此授权影响。确认保存？";
      case "resumeBtSession": return "下次启动将按下载和上传授权恢复 BT 会话中的任务。确认保存？";
      default: return undefined;
    }
  }

  ngOnInit(): void {
    void this.load();
    this.statusPoll = setInterval(() => {
      if (this.config && ratsAuthToken() && !this.runtimeDisabled)
        void this.refreshRuntimeStatus().catch(() => { /* A manual refresh reports connection errors. */ });
    }, 2000);
  }

  ngOnDestroy(): void {
    if (this.statusPoll) clearInterval(this.statusPoll);
  }

  async load(): Promise<void> {
    try {
      await this.refreshState();
      if (!this.retry) {
        this.message = "";
        this.messageIsError = false;
      }
    } catch (error) {
      this.message = String(error);
      this.messageIsError = true;
    }
  }

  forgetApiToken(): void {
    clearRatsAuthToken();
    location.reload();
  }

  private async refreshState(): Promise<void> {
    const [config, status, stats] = await Promise.all([
      ratsCall<Config>("config.get"),
      ratsCall<FeatureStatus>("features.status"),
      ratsCall<P2pStats>("stats.p2pStatus").catch(() => undefined),
    ]);
    this.config = config;
    this.confirmedConfig = { ...config };
    this.status = status;
    this.p2pStats = stats;
  }

  private async refreshRuntimeStatus(): Promise<void> {
    const [status, stats] = await Promise.all([
      ratsCall<FeatureStatus>("features.status"),
      ratsCall<P2pStats>("stats.p2pStatus").catch(() => undefined),
    ]);
    this.status = status;
    this.p2pStats = stats;
  }

  private async persist(key: SettingKey, value: boolean): Promise<void> {
    this.saving = true;
    this.message = "";
    let saved = false;
    try {
      await ratsCall("config.set", { [key]: value });
      saved = true;
      await this.refreshState();
      this.retry = undefined;
      this.message = this.status?.restartRequired ? "已保存，待重启" : "已保存";
      this.messageIsError = false;
    } catch (error) {
      const detail = String(error).replace(/[.!。]+$/, "");
      if (!saved) {
        this.retry = { key, value };
        try { await this.refreshState(); } catch { /* Keep the last visible state. */ }
        this.message = `保存失败：${detail}。运行时可能已应用，但未持久化；请重试保存。`;
      } else {
        this.retry = undefined;
        this.message = `设置已保存，但无法刷新运行状态：${detail}。请刷新状态。`;
      }
      this.messageIsError = true;
    } finally {
      this.saving = false;
      this.restoreFocus();
    }
  }
}
