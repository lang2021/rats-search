import { WatchUpdates } from "../rats/watch-updates.service";
import { Component, inject } from "@angular/core";
import { Router } from "@angular/router";
import { Title } from "@angular/platform-browser";
import { ThemeManager } from "../themes/theme-manager.service";
import { VersionComponent } from "../version/version.component";
import { TranslateManager } from "../i18n/translate-manager.service";
import { HealthModule } from "../health/health.module";
import { HealthService } from "../health/health.service";
import { ThemeEmitterComponent } from "../themes/theme-emitter.component";
import { AppModule } from "../app.module";
import { BreakpointsService } from "./breakpoints.service";

@Component({
  selector: "app-layout",
  templateUrl: "./layout.component.html",
  styleUrl: "./layout.component.scss",
  standalone: true,
  imports: [AppModule, HealthModule, ThemeEmitterComponent, VersionComponent],
})
export class LayoutComponent {
  readonly watch = inject(WatchUpdates);
  menuOpen = false;
  themeManager = inject(ThemeManager);
  translateManager = inject(TranslateManager);
  breakpoints = inject(BreakpointsService);
  title = inject(Title);
  router = inject(Router);
  health = inject(HealthService);
  get headerTitle(): string {
    if (this.router.url === "/search" || this.router.url.startsWith("/search?")) return "资源搜索";
    if (this.router.url.startsWith("/torrent/")) return "资源详情";
    return this.title.getTitle().split(" - ")[0];
  }
}
