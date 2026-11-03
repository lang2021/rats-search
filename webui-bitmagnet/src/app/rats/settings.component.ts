import { Component, OnInit } from "@angular/core";
import { FormsModule } from "@angular/forms";
import { AppModule } from "../app.module";
import { DocumentTitleComponent } from "../layout/document-title.component";
import { ratsCall } from "./rats-api";

type Config = {
  safeSearch: boolean;
  indexer: boolean;
  databaseSharing: boolean;
  p2pReplication: boolean;
  httpPort: number;
  p2pPort: number;
  dhtPort: number;
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
        <div class="options">
          <mat-checkbox [(ngModel)]="c.safeSearch">安全搜索</mat-checkbox>
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
          (click)="save()"
          [disabled]="saving"
        >
          保存设置
        </button>
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
  message = "";
  saving = false;

  ngOnInit(): void {
    void this.load();
  }

  async load(): Promise<void> {
    try {
      this.config = await ratsCall<Config>("config.get");
      this.message = "";
    } catch (error) {
      this.message = String(error);
    }
  }

  async save(): Promise<void> {
    if (!this.config) return;
    this.saving = true;
    try {
      const { safeSearch, indexer, databaseSharing, p2pReplication } =
        this.config;
      await ratsCall("config.set", {
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
