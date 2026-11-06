import { Injectable, OnDestroy, signal } from "@angular/core";
import { Subject } from "rxjs";
import { ratsApiBaseUrl } from "../../environments/environment";
import { ratsAuthToken, ratsCall } from "./rats-api";

@Injectable({ providedIn: "root" })
export class WatchUpdates implements OnDestroy {
  readonly unread = signal<number | null>(null);
  readonly changes = new Subject<void>();
  private socket?: WebSocket;
  private refreshTimer?: ReturnType<typeof setTimeout>;
  private revision = 0;
  private readonly retry = setInterval(() => this.connect(), 10000);

  constructor() { this.connect(); }

  refresh(): void {
    if (this.refreshTimer) return;
    const revision = ++this.revision;
    this.refreshTimer = setTimeout(async () => {
      this.refreshTimer = undefined;
      this.changes.next();
      try {
        const unread = await ratsCall<number>("watch.unread");
        if (revision === this.revision) this.unread.set(unread);
      } catch {
        if (revision === this.revision) this.unread.set(null);
      }
    }, 100);
  }

  connect(): void {
    const token = ratsAuthToken();
    if (this.socket || !token) return;
    const url = new URL(ratsApiBaseUrl || location.origin);
    const secure = url.protocol === "https:";
    const port = Number(url.port || (secure ? 443 : 80)) + 1;
    const socket = new WebSocket(`${secure ? "wss" : "ws"}://${url.hostname}:${port}`);
    this.socket = socket;
    socket.onopen = () => socket.send(JSON.stringify({ method: "auth", params: { token } }));
    socket.onmessage = ({data}) => {
      const frame = JSON.parse(data);
      if (frame.data?.authenticated || frame.event === "watchChanged") this.refresh();
    };
    socket.onclose = () => {
      ++this.revision;
      this.socket = undefined;
      this.unread.set(null);
    };
    this.refresh();
  }

  ngOnDestroy(): void {
    clearInterval(this.retry);
    clearTimeout(this.refreshTimer);
    this.socket?.close();
    this.changes.complete();
  }
}
