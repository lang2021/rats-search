import { Component, OnDestroy, OnInit, inject } from "@angular/core";
import { Subscription } from "rxjs";
import { AppModule } from "../app.module";
import { DocumentTitleComponent } from "../layout/document-title.component";
import { ratsCall } from "./rats-api";
import { WatchUpdates } from "./watch-updates.service";

type Rule = { id: number; keyword: string; enabled: number; total: number; unread: number };
type Match = { id: number; hash: string; name: string; time: number; keyword: string; seen: number };
type WatchPage = { rules: Rule[]; matches: Match[]; more: boolean; missedWrite: boolean };

@Component({
  selector: "app-watch", standalone: true, imports: [AppModule, DocumentTitleComponent],
  templateUrl: "./watch.component.html", styleUrl: "./watch.component.scss",
})
export class WatchComponent implements OnInit, OnDestroy {
  private readonly updates = inject(WatchUpdates);
  private subscription?: Subscription;
  private revision = 0;
  cursors = [0];
  page?: WatchPage;
  keyword = "";
  unreadOnly = false;
  busy = false;
  loading = true;
  error = "";
  readError = "";

  ngOnInit(): void {
    this.subscription = this.updates.changes.subscribe(() => void this.load());
    void this.load();
  }
  ngOnDestroy(): void { this.subscription?.unsubscribe(); ++this.revision; }

  async load(): Promise<void> {
    const revision = ++this.revision;
    this.loading = true;
    try {
      const result = await ratsCall<WatchPage>("watch.list", {
        unreadOnly: this.unreadOnly, before: this.cursors[this.cursors.length - 1],
      });
      if (revision !== this.revision) return;
      this.readError = "";
      this.page = result;
      this.updates.connect();
    } catch (e) {
      if (revision === this.revision) this.readError = String(e);
    } finally {
      if (revision === this.revision) this.loading = false;
    }
  }
  filter(value: boolean): void { this.unreadOnly = value; this.cursors = [0]; void this.load(); }
  next(): void {
    if (this.page?.more) this.cursors.push(this.page.matches[this.page.matches.length - 1].id);
    void this.load();
  }
  previous(): void { if (this.cursors.length > 1) this.cursors.pop(); void this.load(); }

  async change(method: string, params: Record<string, unknown>): Promise<void> {
    if (this.busy) return;
    this.busy = true;
    this.error = "";
    try {
      await ratsCall(method, params);
      if (method === "watch.add") this.keyword = "";
      await this.load();
      this.updates.refresh();
    } catch (e) { this.error = String(e); }
    finally { this.busy = false; }
  }
  remove(rule: Rule): void {
    if (confirm(`删除「${rule.keyword}」及其全部 ${rule.total} 条提醒？资源仍保留在索引中。`))
      void this.change("watch.delete", {id: rule.id});
  }
}
