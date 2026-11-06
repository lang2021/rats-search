import { Component, OnInit } from "@angular/core";
import { ActivatedRoute, Router } from "@angular/router";
import { AppModule } from "../app.module";
import { DocumentTitleComponent } from "../layout/document-title.component";
import { ratsCall } from "./rats-api";
import { queueLocalSearch } from "./search-navigation";

type Item = Record<string, unknown>;
@Component({
  selector: "app-browse",
  standalone: true,
  imports: [AppModule, DocumentTitleComponent],
  template: `<app-document-title [parts]="[title]" />
    <main class="page"><header><h1>{{ title }}</h1><small>RATS SEARCH · LOCAL INDEX</small></header>
      <nav *ngIf="page === 'discover'"><button mat-button (click)="load('top')">热门</button><button mat-button (click)="load('recent')">最近加入</button></nav>
      <p class="hint">{{ hint }}</p><p *ngIf="error" class="error" role="alert">{{error}}</p>
      <p *ngIf="loading">正在读取…</p><p *ngIf="!loading && !error && items.length === 0">暂无数据。</p>
      <div class="table-wrap" *ngIf="items.length"><table><thead><tr><th>资源 / 搜索词</th><th>大小</th><th>次数 / 加入时间</th><th>操作</th></tr></thead><tbody>
        <tr *ngFor="let item of items"><td>{{name(item)}}</td><td>{{size(item)}}</td><td>{{meta(item)}}</td><td><button mat-button (click)="open(item)">{{page === 'history' ? '搜索' : '查看详情'}}</button></td></tr>
      </tbody></table></div>
    </main>`,
  styles: [`:host{display:block;color:var(--rs-text)}.page{margin:0;padding:0}header{display:flex;justify-content:space-between;align-items:end;margin-bottom:16px}h1{font-size:22px;font-weight:500;margin:0}header small{font-size:10px;letter-spacing:.24em;color:var(--rs-subtle)}nav{display:flex;border:1px solid var(--rs-border);background:var(--rs-surface);padding:5px;width:max-content;gap:4px}nav button{border-radius:0;color:var(--rs-accent)}.hint{color:var(--rs-muted);font-size:12px}.table-wrap{overflow:auto;background:var(--rs-surface);border:1px solid var(--rs-border)}table{width:100%;border-collapse:collapse}th,td{text-align:left;padding:11px 14px;border-bottom:1px solid var(--rs-border);font-size:12px}th{background:var(--rs-raised);color:var(--rs-accent);font-size:10px;letter-spacing:.2em}td button{color:var(--rs-accent)}.error{color:var(--rs-danger)}@media(max-width:600px){header{align-items:start;flex-direction:column;gap:7px}}`],
})
export class BrowseComponent implements OnInit {
  page = "discover"; title = "发现资源"; hint = "热门与最近收录内容来自本机索引。"; items: Item[] = []; loading = true; error = "";
  constructor(route: ActivatedRoute, private router: Router) { this.page = route.snapshot.data["page"] ?? "discover"; this.title = this.page === "history" ? "搜索历史" : "发现资源"; this.hint = this.page === "history" ? "显示本机保存的搜索历史。" : this.hint; }
  ngOnInit(): void { void this.load(this.page === "history" ? "history" : "top"); }
  async load(which: string): Promise<void> { this.loading = true; this.error = ""; try { this.items = await ratsCall<Item[]>(which === "history" ? "search.history" : which === "recent" ? "search.recent" : "search.top", { limit: 50 }); } catch (e) { this.error = String(e); } finally { this.loading = false; } }
  name(x: Item): string { return String(x["name"] ?? x["query"] ?? x["title"] ?? "未知"); }
  size(x: Item): string { const n = Number(x["size"] ?? 0); if (!n) return "—"; const units = ["B", "KB", "MB", "GB", "TB"]; const i = Math.min(Math.floor(Math.log(n) / Math.log(1024)), units.length - 1); return `${(n / 1024 ** i).toFixed(i ? 1 : 0)} ${units[i]}`; }
  meta(x: Item): string { if (this.page === "history") { const stamp = Number(x["lastSearchedAt"] ?? 0); return `${Number(x["count"] ?? 0)} 次 · ${stamp ? new Date(stamp).toLocaleString() : "—"}`; } const value = Number(x["added"] ?? 0); return value ? new Date(value < 1e12 ? value * 1000 : value).toLocaleString() : "—"; }
  open(x: Item): void { if (this.page === "history") { queueLocalSearch(this.name(x)); void this.router.navigate(["/search"]); } else if (x["hash"]) void this.router.navigate(["/torrent", x["hash"]]); }
}
