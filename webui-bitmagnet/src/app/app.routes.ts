import { Routes } from "@angular/router";

export const routes: Routes = [
  {
    path: "",
    pathMatch: "full",
    redirectTo: "torrents",
  },
  {
    path: "torrents",
    loadComponent: () =>
      import("./torrents/torrents.component").then((c) => c.TorrentsComponent),
    children: [
      {
        path: "",
        loadComponent: () =>
          import("./torrents/torrents-search.component").then(
            (c) => c.TorrentsSearchComponent,
          ),
      },
      {
        path: "permalink/:infoHash",
        loadComponent: () =>
          import("./torrents/torrent-permalink.component").then(
            (c) => c.TorrentPermalinkComponent,
          ),
      },
    ],
  },
  {
    path: "dashboard",
    loadComponent: () =>
      import("./dashboard/dashboard.component").then(
        (c) => c.DashboardComponent,
      ),
    children: [
      {
        path: "",
        loadComponent: () =>
          import("./dashboard/dashboard-home.component").then(
            (c) => c.DashboardHomeComponent,
          ),
      },
      {
        path: "network",
        loadComponent: () =>
          import("./rats/network.component").then((c) => c.NetworkComponent),
      },
      {
        path: "settings",
        loadComponent: () =>
          import("./rats/settings.component").then((c) => c.SettingsComponent),
      },
    ],
  },
  {
    path: "console",
    loadComponent: () =>
      import("./dashboard/dashboard.component").then((c) => c.DashboardComponent),
    children: [
      { path: "settings", loadComponent: () =>
          import("./rats/settings.component").then((c) => c.SettingsComponent) },
    ],
  },
  {
    path: "**",
    loadComponent: () =>
      import("./not-found/not-found.component").then(
        (c) => c.NotFoundComponent,
      ),
  },
];
