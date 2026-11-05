import {
  ChangeDetectorRef,
  ChangeDetectionStrategy,
  Component,
  inject,
  OnDestroy,
  OnInit,
} from "@angular/core";
import { ActivatedRoute, Params, Router } from "@angular/router";
import {
  BehaviorSubject,
  combineLatestWith,
  Observable,
  Subscription,
} from "rxjs";
import { map } from "rxjs/operators";
import { TranslocoService } from "@jsverse/transloco";
import { FormControl } from "@angular/forms";
import { SelectionModel } from "@angular/cdk/collections";
import { Apollo } from "apollo-angular";
import { ErrorsService } from "../errors/errors.service";
import { GraphQLModule } from "../graphql/graphql.module";
import { PaginatorComponent } from "../paginator/paginator.component";
import { BreakpointsService } from "../layout/breakpoints.service";
import * as generated from "../graphql/generated";
import { intParam, stringListParam, stringParam } from "../util/query-string";
import { AppModule } from "../app.module";
import { DocumentTitleComponent } from "../layout/document-title.component";
import { ratsCall, ratsSafeSearch, RatsTorrent, requestRatsToken } from "../rats/rats-api";
import { ratsApiBaseUrl } from "../../environments/environment";
import { takeQueuedLocalSearch } from "../rats/search-navigation";
import { IntEstimatePipe } from "../pipes/int-estimate.pipe";
import { FilesizePipe } from "../pipes/filesize.pipe";
import { TorrentsBulkActionsComponent } from "./torrents-bulk-actions.component";
import { contentTypeList, contentTypeMap } from "./content-types";
import {
  allColumns,
  compactColumns,
  TorrentsTableComponent,
} from "./torrents-table.component";
import {
  emptyResult,
  TorrentsSearchDatasource,
} from "./torrents-search.datasource";
import {
  ContentTypeSelection,
  defaultOrderBy,
  FacetInfo,
  facets,
  inactiveFacet,
  isDefaultOrdering,
  orderByOptions,
  OrderBySelection,
  TorrentSearchControls,
  TorrentSelection,
  TorrentsSearchController,
  TorrentTab,
  torrentTabNames,
  TorrentTabSelection,
} from "./torrents-search.controller";

@Component({
  selector: "app-torrents-search",
  templateUrl: "./torrents-search.component.html",
  styleUrl: "./torrents-search.component.scss",
  standalone: true,
  imports: [
    AppModule,
    DocumentTitleComponent,
    GraphQLModule,
    PaginatorComponent,
    TorrentsBulkActionsComponent,
    TorrentsTableComponent,
    IntEstimatePipe,
    FilesizePipe,
  ],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class TorrentsSearchComponent implements OnInit, OnDestroy {
  private route = inject(ActivatedRoute);
  private router = inject(Router);
  private apollo = inject(Apollo);
  private errorsService = inject(ErrorsService);
  private transloco = inject(TranslocoService);
  private changeDetector = inject(ChangeDetectorRef);
  breakpoints = inject(BreakpointsService);

  dataSource: TorrentsSearchDatasource;

  controller: TorrentsSearchController;

  controls = initControls;

  contentTypes = contentTypeList;
  orderByOptions = orderByOptions;

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  facets$: Observable<FacetInfo<any, any>[]>;

  allColumns = allColumns;
  compactColumns = compactColumns;

  queryString = new FormControl("");
  searchScope: "local" | "peers" = "local";
  remoteResults: RatsTorrent[] = [];
  remoteCandidates: RatsTorrent[] = [];
  remoteStatus: "idle" | "loading" | "done" | "error" | "cancelled" = "idle";
  remoteMessage = "";
  remotePeers = 0;
  private remoteAbort?: AbortController;
  private remoteSearchId?: string;
  private remoteWs?: WebSocket;
  private remoteQuery = "";

  result = emptyResult;

  multiSelection = new SelectionModel<string>(true, []);

  private selectedItemsSubject = new BehaviorSubject<
    generated.TorrentContent[]
  >([]);
  selectedItems$ = this.selectedItemsSubject.asObservable();

  private subscriptions = Array<Subscription>();

  constructor() {
    this.controller = new TorrentsSearchController(this.controls);
    this.dataSource = new TorrentsSearchDatasource(
      this.apollo,
      this.errorsService,
      this.controller.params$,
    );
    this.subscriptions.push(
      this.controller.controls$.subscribe((ctrl) => {
        this.controls = ctrl;
      }),
    );
    this.facets$ = this.controller.controls$.pipe(
      combineLatestWith(this.dataSource.result$),
      map(([controls, result]) =>
        facets.map((f) => ({
          ...f,
          ...f.extractInput(controls.facets),
          relevant: false,
          aggregations: f
            .extractAggregations(result.aggregations)
            .map((agg) => ({
              ...agg,
              label: f.resolveLabel(agg, this.transloco),
            })),
        })),
      ),
    );
    this.subscriptions.push(
      this.dataSource.result$.subscribe((result) => {
        this.result = result;
        const infoHashes = new Set(
          result.items.map(({ infoHash }) => infoHash),
        );
        this.multiSelection.deselect(
          ...this.multiSelection.selected.filter(
            (infoHash) => !infoHashes.has(infoHash),
          ),
        );
      }),
    );
  }

  ngOnInit(): void {
    const queuedSearch = takeQueuedLocalSearch();
    this.subscriptions.push(
      this.route.queryParams.subscribe((params) => {
        const next = paramsToControls(params, this.controls.queryString ?? undefined);
        this.queryString.setValue(next.queryString ?? null);
        this.controller.update(() => next);
      }),
      this.controller.controls$.subscribe((ctrl) => {
        void this.router.navigate([], {
          relativeTo: this.route,
          queryParams: controlsToParams(ctrl),
          queryParamsHandling: "replace",
          replaceUrl: true,
        });
      }),
      this.multiSelection.changed.subscribe((selection) => {
        const infoHashes = new Set(selection.source.selected);
        this.selectedItemsSubject.next(
          this.result.items.filter((i) => infoHashes.has(i.infoHash)),
        );
      }),
    );
    if (queuedSearch) {
      this.queryString.setValue(queuedSearch);
      this.controller.setQueryString(queuedSearch);
    }
  }

  ngOnDestroy() {
    this.cancelRemoteSearch();
    this.subscriptions.forEach((subscription) => subscription.unsubscribe());
    this.subscriptions = new Array<Subscription>();
  }

  setSearchScope(scope: "local" | "peers") {
    this.searchScope = scope;
    if (scope === "local") {
      this.cancelRemoteSearch();
      this.remoteResults = [];
      this.remoteCandidates = [];
      this.remoteStatus = "idle";
      this.remoteMessage = "";
    }
  }

  submitSearch() {
    const query = (this.queryString.value ?? "").trim();
    this.controller.setQueryString(query || null);
    if (this.searchScope === "peers") void this.runRemoteSearch(query);
    else this.cancelRemoteSearch();
  }

  refreshSearch() {
    this.dataSource.refresh();
    if (this.searchScope === "peers")
      void this.runRemoteSearch((this.controls.queryString ?? "").trim());
  }

  cancelRemoteSearch() {
    const id = this.remoteSearchId;
    this.remoteSearchId = undefined;
    this.remoteAbort?.abort();
    this.remoteAbort = undefined;
    this.remoteWs?.close();
    this.remoteWs = undefined;
    if (id) void ratsCall("search.cancel", { searchId: id }).catch(() => {});
    if (this.remoteStatus === "loading") {
      this.remoteStatus = "cancelled";
      this.remoteMessage = "远端搜索已取消";
    }
  }

  onQueryEdit() {
    if ((this.queryString.value ?? "").trim() !== this.remoteQuery) {
      this.cancelRemoteSearch();
      this.remoteResults = [];
      this.remoteCandidates = [];
    }
  }

  get visibleRemoteResults(): RatsTorrent[] {
    const local = new Set(this.result.items.map((item) => item.infoHash.toLowerCase()));
    return this.remoteResults.filter((item) => !local.has(item.hash.toLowerCase()));
  }

  get remoteDuplicateCount(): number {
    return this.remoteResults.length - this.visibleRemoteResults.length;
  }

  get visibleRemoteCandidates(): RatsTorrent[] {
    const seen = new Set([
      ...this.result.items.map((item) => item.infoHash.toLowerCase()),
      ...this.remoteResults.map((item) => item.hash.toLowerCase()),
    ]);
    return this.remoteCandidates.filter((item) => !seen.has(item.hash.toLowerCase()));
  }

  async openRemote(item: RatsTorrent) {
    try {
      await ratsCall("torrent.get", { hash: item.hash, peer: item.peer, files: true });
      await this.router.navigate(["/torrent", item.hash]);
    } catch (error) {
      this.remoteStatus = "error";
      this.remoteMessage = error instanceof Error ? error.message : "无法读取远端详情";
      this.changeDetector.markForCheck();
    }
  }

  private async runRemoteSearch(query: string) {
    this.cancelRemoteSearch();
    this.remoteResults = [];
    this.remoteCandidates = [];
    this.remoteQuery = query;
    if (query.length < 3) {
      this.remoteStatus = "error";
      this.remoteMessage = "远端搜索至少需要 3 个字符";
      return;
    }
    const id = crypto.randomUUID();
    const controller = new AbortController();
    this.remoteSearchId = id;
    this.remoteAbort = controller;
    this.remoteStatus = "loading";
    this.remoteMessage = "正在查询已连接的 Rats 节点…";
    try {
      await this.connectRemoteStream(id, controller.signal);
      if (this.remoteSearchId !== id || controller.signal.aborted) return;
      const safeSearch = await ratsSafeSearch(controller.signal);
      if (this.remoteSearchId !== id || controller.signal.aborted) return;
      const response = await ratsCall<{ items: RatsTorrent[]; candidates: RatsTorrent[]; peers: number; windowExpired: boolean }>(
        "search.remote",
        {
          searchId: id,
          query,
          limit: 50,
          sort: this.controls.orderBy.field,
          orderDesc: this.controls.orderBy.descending,
          safeSearch,
        },
        controller.signal,
      );
      if (this.remoteSearchId !== id) return;
      this.mergeRemoteHits(response.items, false);
      this.mergeRemoteHits(response.candidates ?? [], true);
      this.remotePeers = response.peers;
      this.remoteStatus = "done";
      this.remoteMessage = `查询窗口已结束 · 已连接 ${response.peers} 个节点`;
      this.changeDetector.markForCheck();
    } catch (error) {
      if (this.remoteSearchId !== id) return;
      if (error instanceof Error && error.message.includes("permission disabled")) {
        this.remoteResults = [];
        this.remoteCandidates = [];
      }
      this.remoteStatus = controller.signal.aborted ? "cancelled" : "error";
      this.remoteMessage = controller.signal.aborted
        ? "远端搜索已取消"
        : error instanceof Error ? error.message : "远端搜索失败";
      this.changeDetector.markForCheck();
    } finally {
      if (this.remoteSearchId === id) {
        this.remoteSearchId = undefined;
        this.remoteAbort = undefined;
        this.remoteWs?.close();
        this.remoteWs = undefined;
      }
    }
  }

  private async connectRemoteStream(id: string, signal: AbortSignal): Promise<void> {
    const token = await requestRatsToken();
    if (!token || signal.aborted || this.remoteSearchId !== id) return;
    const base = ratsApiBaseUrl ? new URL(ratsApiBaseUrl) : location;
    const port = Number(base.port || (base.protocol === "https:" ? 443 : 80)) + 1;
    const url = `${base.protocol === "https:" ? "wss:" : "ws:"}//${base.hostname}:${port}`;
    try {
      const ws = new WebSocket(url);
      this.remoteWs = ws;
      await new Promise<void>((resolve) => {
        let settled = false;
        const finish = () => {
          if (settled) return;
          settled = true;
          clearTimeout(timeout);
          resolve();
        };
        const timeout = setTimeout(() => { ws.close(); finish(); }, 1500);
        ws.onopen = () => ws.send(JSON.stringify({ method: "auth", params: { token } }));
        ws.onmessage = (message) => {
          let frame: { event?: string; data?: { authenticated?: boolean; searchId?: string; candidateSessionId?: string; torrents?: RatsTorrent[] } };
          try { frame = JSON.parse(String(message.data)); } catch { return; }
          if (frame.data?.authenticated) { finish(); return; }
          if (this.remoteSearchId !== id || signal.aborted || this.searchScope !== "peers") return;
          if (frame.event === "remoteSearchResults" && frame.data?.searchId === id)
            this.mergeRemoteHits(frame.data.torrents ?? [], false);
          if (frame.event === "remoteSearchCandidates" && frame.data?.candidateSessionId === id)
            this.mergeRemoteHits(frame.data?.torrents ?? [], true);
        };
        ws.onerror = finish;
        ws.onclose = finish;
        signal.addEventListener("abort", () => { ws.close(); finish(); }, { once: true });
      });
    } catch { /* The final HTTP response remains available without a live stream. */ }
  }

  private mergeRemoteHits(items: RatsTorrent[], candidate: boolean): void {
    if (!Array.isArray(items)) return;
    const target = candidate ? this.remoteCandidates : this.remoteResults;
    for (const item of items.slice(0, 100)) {
      if (!/^[0-9a-f]{40}$/i.test(item.hash ?? "") || !item.name || !item.peer) continue;
      if (candidate && !this.plausibleCandidate(item)) continue;
      const index = target.findIndex((existing) => existing.hash.toLowerCase() === item.hash.toLowerCase());
      if (index >= 0) {
        if (target[index].isFileMatch && !item.isFileMatch) target[index] = item;
      } else if (target.length < 100) {
        target.push(item);
      }
    }
    this.changeDetector.markForCheck();
  }

  private plausibleCandidate(item: RatsTorrent): boolean {
    const paths = (item as RatsTorrent & { matchingPaths?: string[] }).matchingPaths ?? [];
    const text = [item.name, ...paths.slice(0, 12)].join(" ").toLowerCase();
    return this.remoteQuery.toLowerCase().split(/\s+/).filter(Boolean)
      .every((word) => text.includes(word));
  }
}

const defaultLimit = 20;

const initControls: TorrentSearchControls = {
  page: 1,
  limit: defaultLimit,
  contentType: null,
  orderBy: defaultOrderBy,
  facets: {
    genre: inactiveFacet,
    language: inactiveFacet,
    fileType: inactiveFacet,
    torrentSource: inactiveFacet,
    torrentTag: inactiveFacet,
    videoResolution: inactiveFacet,
    videoSource: inactiveFacet,
  },
};

const paramsToControls = (params: Params, currentQuery?: string): TorrentSearchControls => {
  const queryString = stringParam(params, "query") ?? currentQuery;
  const activeFacets = stringListParam(params, "facets");
  let selectedTorrent: TorrentSelection | undefined;
  const selectedTorrentParam = stringParam(params, "torrent");
  if (selectedTorrentParam) {
    let torrentTabSelection: TorrentTabSelection;
    const strTab = stringParam(params, "tab");
    if (torrentTabNames.includes(strTab as TorrentTab)) {
      torrentTabSelection = strTab as TorrentTab;
    }
    selectedTorrent = {
      infoHash: selectedTorrentParam,
      tab: torrentTabSelection,
    };
  }
  return {
    queryString,
    orderBy: orderByParam(params, !!queryString),
    contentType: contentTypeParam(params),
    limit: intParam(params, "limit") ?? defaultLimit,
    page: intParam(params, "page") ?? 1,
    selectedTorrent,
    facets: facets.reduce<TorrentSearchControls["facets"]>((acc, facet) => {
      const active = activeFacets?.includes(facet.key) ?? false;
      const filter = stringListParam(params, facet.key);
      return facet.patchInput(acc, {
        active,
        filter,
      });
    }, initControls.facets),
  };
};

const controlsToParams = (ctrl: TorrentSearchControls): Params => {
  let page: number | undefined = ctrl.page;
  let limit: number | undefined = ctrl.limit;
  if (page === 1) {
    page = undefined;
  }
  if (limit === defaultLimit) {
    limit = undefined;
  }
  const orderBy = isDefaultOrdering(ctrl) ? undefined : ctrl.orderBy;
  let desc: string | undefined;
  if (orderBy) {
    desc = orderBy.descending ? "1" : "0";
  }
  return {
    page,
    limit,
    content_type: ctrl.contentType,
    order: orderBy?.field,
    desc,
    ...(ctrl.selectedTorrent
      ? {
          torrent: ctrl.selectedTorrent.infoHash,
          tab: ctrl.selectedTorrent.tab ?? undefined,
        }
      : {}),
    ...flattenFacets(ctrl.facets),
  };
};

const contentTypeParam = (params: Params): ContentTypeSelection => {
  const str = stringParam(params, "content_type");
  return str && str in contentTypeMap ? (str as ContentTypeSelection) : null;
};

const orderByParam = (params: Params, hasQuery: boolean): OrderBySelection => {
  let desc: boolean | null = null;
  const strDesc = stringParam(params, "desc");
  if (strDesc === "1") {
    desc = true;
  } else if (strDesc === "0") {
    desc = false;
  }
  const field = stringParam(params, "order");
  for (const opt of orderByOptions) {
    if (opt.field === field) {
      return {
        field,
        descending: desc ?? opt.descending,
      };
    }
  }
  return {
    field: hasQuery ? "relevance" : "published_at",
    descending: desc ?? true,
  };
};

const flattenFacets = (
  ctrl: TorrentSearchControls["facets"],
): Record<string, unknown> => {
  const [activeFacets, filters] = facets.reduce<
    [string[], Record<string, string[]>]
  >(
    (acc, f) => {
      const input = f.extractInput(ctrl);
      if (input.active) {
        return [
          [...acc[0], f.key],
          input.filter
            ? {
                ...acc[1],
                [f.key]: input.filter,
              }
            : acc[1],
        ];
      } else {
        return acc;
      }
    },
    [[], {}],
  );
  return {
    facets: activeFacets.length ? activeFacets.join(",") : undefined,
    ...Object.fromEntries(
      Object.entries(filters).map(([k, values]) => [
        k,
        encodeURIComponent(values.join(",")),
      ]),
    ),
  };
};
