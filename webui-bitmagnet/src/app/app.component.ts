import { Component } from "@angular/core";
import { RouterOutlet } from "@angular/router";
import { MatIconRegistry } from "@angular/material/icon";
import { DomSanitizer } from "@angular/platform-browser";
import { LayoutComponent } from "./layout/layout.component";
import { initializeIcons } from "./app.icons";

@Component({
  selector: "app-root",
  standalone: true,
  imports: [RouterOutlet, LayoutComponent],
  templateUrl: "./app.component.html",
  styleUrl: "./app.component.scss",
})
export class AppComponent {
  title = "Rats Search";
  constructor(iconRegistry: MatIconRegistry, domSanitizer: DomSanitizer) {
    initializeIcons(iconRegistry, domSanitizer);
    const legacy = window.location.hash.replace(/^#\/?/, "");
    const target = legacy.startsWith("torrents/permalink/")
      ? `/torrent/${legacy.slice("torrents/permalink/".length)}`
        : legacy === "torrents" || (legacy === "" && (window.location.pathname === "/" || window.location.pathname === ""))
        ? "/search"
        : legacy === "dashboard"
          ? "/console"
          : legacy.startsWith("dashboard/")
            ? `/console/${legacy.slice("dashboard/".length)}`
            : "";
    if (target) window.history.replaceState(null, "", target);
  }
}
