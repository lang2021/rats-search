import { Component, OnInit } from "@angular/core";
import { ActivatedRoute } from "@angular/router";
import { AppModule } from "../app.module";
import { DocumentTitleComponent } from "../layout/document-title.component";
import { ratsCall } from "./rats-api";
import { TrawlComponent } from "./trawl.component";

type PageKey = "index" | "downloads" | "events";
@Component({
  selector: "app-console-data",
  standalone: true,
  imports: [AppModule, DocumentTitleComponent, TrawlComponent],
  template: `<app-document-title [parts]="[title]" />
    <app-trawl *ngIf="page === 'events'" />
    <main class="page" *ngIf="page !== 'events'">
      <header><div><h1>{{ title }}</h1><small>RATS SEARCH · LIVE DATA</small></div><button mat-stroked-button (click)="refresh()">刷新</button></header>
      <p class="hint">{{ hint }}</p>
      <p *ngIf="error" class="error" role="alert">{{ error }}</p>
      <p *ngIf="loading">正在读取…</p>
      <p *ngIf="!loading && !error && rows.length === 0">当前没有可显示的数据。</p>
      <section *ngFor="let row of rows; let i = index" class="row">
        <b>{{ row.label }}</b><pre>{{ row.value }}</pre>
      </section>
    </main>`,
  styles: [`:host{display:block;color:var(--rs-text)} .page{margin:0;padding:0} header{display:flex;align-items:center;justify-content:space-between;margin-bottom:15px} h1{font-size:22px;font-weight:500;margin:0 0 5px} header small{color:var(--rs-subtle);font-size:10px;letter-spacing:.24em}.hint{color:var(--rs-muted);font-size:12px} .row{background:var(--rs-surface);border:1px solid var(--rs-border);margin:10px 0;overflow:auto}.row b{display:block;background:var(--rs-raised);color:var(--rs-accent);padding:11px 14px;font-size:10px;letter-spacing:.22em}.row pre{white-space:pre-wrap;overflow-wrap:anywhere;color:var(--rs-muted);padding:14px;margin:0;font-size:11px;line-height:1.5}.error{color:var(--rs-danger)}button{border-radius:0;border-color:var(--rs-accent);color:var(--rs-accent)}`],
})
export class ConsoleDataComponent implements OnInit {
  page: PageKey = "index";
  title = "索引状态";
  hint = "读取 Rats Search 当前索引与共享状态。";
  rows: { label: string; value: string }[] = [];
  loading = true;
  error = "";

  constructor(route: ActivatedRoute) {
    this.page = (route.snapshot.data["page"] ?? "index") as PageKey;
    this.title = ({ index: "索引与同步", downloads: "下载任务", events: "Trawl · 探险者活动流" })[this.page];
    this.hint = this.page === "events" ? "仅显示本次打开页面后收到的实时事件；不保存历史日志。" : "只读显示 API 当前返回的数据。";
  }
  ngOnInit(): void { if (this.page !== "events") void this.refresh(); }
  async refresh(): Promise<void> {
    this.loading = true; this.error = "";
    if (this.page === "events") { this.rows = []; this.loading = false; return; }
    try {
      const methods = this.page === "downloads" ? ["download.list"] : ["database.status", "stats.get"];
      const values = await Promise.all(methods.map((m) => ratsCall(m)));
      this.rows = methods.map((label, i) => ({ label, value: JSON.stringify(values[i], null, 2) }));
    } catch (e) { this.error = String(e); }
    finally { this.loading = false; }
  }
}
