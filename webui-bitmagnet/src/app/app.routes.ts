import { Routes } from "@angular/router";

const search = () => import("./torrents/torrents.component").then((c) => c.TorrentsComponent);
const detail = () => import("./torrents/torrent-permalink.component").then((c) => c.TorrentPermalinkComponent);
const consoleView = () => import("./dashboard/dashboard.component").then((c) => c.DashboardComponent);

export const routes: Routes = [
  { path: "", pathMatch: "full", redirectTo: "search" },
  { path: "search", loadComponent: search, children: [
    { path: "", loadComponent: () => import("./torrents/torrents-search.component").then((c) => c.TorrentsSearchComponent) },
    { path: "permalink/:infoHash", redirectTo: "/torrent/:infoHash", pathMatch: "full" },
    { path: ":infoHash", loadComponent: detail },
  ] },
  { path: "torrent/:infoHash", loadComponent: detail },
  { path: "discover", data: { page: "discover" }, loadComponent: () => import("./rats/browse.component").then((c) => c.BrowseComponent) },
  { path: "history", data: { page: "history" }, loadComponent: () => import("./rats/browse.component").then((c) => c.BrowseComponent) },
  { path: "console", loadComponent: consoleView, children: [
    { path: "", loadComponent: () => import("./dashboard/dashboard-home.component").then((c) => c.DashboardHomeComponent) },
    { path: "network", loadComponent: () => import("./rats/network.component").then((c) => c.NetworkComponent) },
    { path: "settings", loadComponent: () => import("./rats/settings.component").then((c) => c.SettingsComponent) },
    { path: "search", loadComponent: search, children: [
      { path: "", loadComponent: () => import("./torrents/torrents-search.component").then((c) => c.TorrentsSearchComponent) },
    ] },
    { path: "index", data: { page: "index" }, loadComponent: () => import("./rats/console-data.component").then((c) => c.ConsoleDataComponent) },
    { path: "downloads", data: { page: "downloads" }, loadComponent: () => import("./rats/console-data.component").then((c) => c.ConsoleDataComponent) },
    { path: "events", data: { page: "events" }, loadComponent: () => import("./rats/console-data.component").then((c) => c.ConsoleDataComponent) },
  ] },
  { path: "torrents", redirectTo: "search", pathMatch: "full" },
  { path: "dashboard", redirectTo: "console", pathMatch: "full" },
  { path: "**", loadComponent: () => import("./not-found/not-found.component").then((c) => c.NotFoundComponent) },
];
