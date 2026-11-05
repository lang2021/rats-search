import { ApolloLink, Observable } from "@apollo/client/core";
import { ratsCall, ratsReady, ratsSafeSearch, RatsTorrent } from "./rats-api";

type SearchInput = {
  queryString?: string;
  infoHashes?: string[];
  limit?: number;
  page?: number;
  orderBy?: Array<{ field: string; descending?: boolean }>;
  facets?: { contentType?: { filter?: Array<string | null> } };
};

type FileInput = { infoHashes: string[]; limit?: number; page?: number };

const sortFields: Record<string, string> = {
  published_at: "added",
  updated_at: "added",
  files_count: "files",
  size: "size",
  seeders: "seeders",
  leechers: "leechers",
  name: "name",
};

function date(ms: number): string {
  return new Date(ms || Date.now()).toISOString();
}

function content(t: RatsTorrent) {
  const type = t.contentType || null;
  const timestamp = date(t.added);
  return {
    id: t.hash,
    infoHash: t.hash,
    contentType: type,
    title: t.name,
    torrent: {
      infoHash: t.hash,
      name: t.name,
      size: t.size,
      filesStatus:
        t.files === 0 ? "no_info" : t.files === 1 ? "single" : "multi",
      filesCount: t.files,
      hasFilesInfo: t.files > 0,
      singleFile: t.files === 1,
      fileType: null,
      sources: [{ key: "rats", name: "Rats Search" }],
      seeders: t.seeders,
      leechers: t.leechers,
      tagNames: t.contentCategory ? [t.contentCategory] : [],
      magnetUri: `magnet:?xt=urn:btih:${t.hash}`,
      createdAt: timestamp,
      updatedAt: timestamp,
    },
    content: null,
    languages: null,
    episodes: null,
    video3d: null,
    videoCodec: null,
    videoModifier: null,
    videoResolution: null,
    videoSource: null,
    seeders: t.seeders,
    leechers: t.leechers,
    publishedAt: timestamp,
    createdAt: timestamp,
    updatedAt: timestamp,
  };
}

async function search(input: SearchInput, signal: AbortSignal) {
  if (input.infoHashes?.length) {
    const items = await Promise.all(
      input.infoHashes.map(async (hash) => {
        try {
          return content(
            await ratsCall<RatsTorrent>("torrent.get", { hash }, signal),
          );
        } catch {
          return null;
        }
      }),
    );
    const found = items.filter((item) => item !== null);
    return {
      items: found,
      totalCount: found.length,
      totalCountIsEstimate: false,
      hasNextPage: false,
      aggregations: { contentType: [] },
    };
  }

  const query = (input.queryString ?? "").trim();
  const limit = Math.min(Math.max(input.limit ?? 20, 1), 100);
  const offset = (Math.max(input.page ?? 1, 1) - 1) * limit;
  const selectedType = input.facets?.contentType?.filter?.[0] ?? "";
  const order = input.orderBy?.[0];
  const params: Record<string, unknown> = {
    limit: limit + 1,
    offset,
    contentType: selectedType,
  };
  let method = "search.top";
  if (query) {
    method = "search.torrents";
    params["query"] = query;
    params["localOnly"] = true;
    params["safeSearch"] = await ratsSafeSearch(signal);
    params["sort"] = sortFields[order?.field ?? ""] ?? "";
    params["orderDesc"] = order?.descending ?? true;
  } else {
    params["index"] = offset;
    params["type"] = selectedType;
    delete params["contentType"];
  }

  const results = await ratsCall<RatsTorrent[]>(method, params, signal);
  const hasNextPage = results.length > limit;
  const page = results.slice(0, limit);
  const counts = new Map<string | null, number>();
  for (const item of page) {
    const key = item.contentType || null;
    counts.set(key, (counts.get(key) ?? 0) + 1);
  }
  return {
    items: page.map(content),
    totalCount: offset + page.length + (hasNextPage ? 1 : 0),
    totalCountIsEstimate: true,
    hasNextPage,
    aggregations: {
      contentType: [...counts].map(([value, count]) => ({
        value,
        label: value ?? "Unknown",
        count,
        isEstimate: true,
      })),
    },
  };
}

async function files(input: FileInput, signal: AbortSignal) {
  const hash = input.infoHashes?.[0];
  if (!hash) return { items: [], totalCount: 0, hasNextPage: false };
  const torrent = await ratsCall<RatsTorrent>(
    "torrent.get",
    { hash, files: true },
    signal,
  );
  const all = torrent.files_list ?? [];
  const limit = Math.max(input.limit ?? 10, 1);
  const offset = (Math.max(input.page ?? 1, 1) - 1) * limit;
  return {
    items: all.slice(offset, offset + limit).map((file, index) => ({
      infoHash: hash,
      index: offset + index + 1,
      path: file.path,
      size: file.size,
      fileType: null,
      createdAt: date(torrent.added),
      updatedAt: date(torrent.added),
    })),
    totalCount: all.length,
    hasNextPage: offset + limit < all.length,
  };
}

async function health(signal: AbortSignal) {
  const [ready, p2p, database] = await Promise.all([
    ratsReady(signal),
    ratsCall<{ running: boolean; dhtRunning: boolean }>(
      "stats.p2pStatus",
      {},
      signal,
    ),
    ratsCall<{ torrents: number }>("stats.database", {}, signal),
  ]);
  const timestamp = new Date().toISOString();
  const checks = [
    { key: "api", status: ready ? "up" : "down", timestamp, error: null },
    {
      key: "database",
      status: database ? "up" : "down",
      timestamp,
      error: null,
    },
    { key: "p2p", status: p2p.running ? "up" : "down", timestamp, error: null },
    {
      key: "dht",
      status: p2p.dhtRunning ? "up" : "down",
      timestamp,
      error: null,
    },
  ];
  return {
    health: {
      status: checks.every((check) => check.status === "up") ? "up" : "down",
      checks,
    },
    workers: { listAll: { workers: [] } },
  };
}

async function route(
  name: string,
  variables: Record<string, unknown>,
  signal: AbortSignal,
): Promise<Record<string, unknown>> {
  switch (name) {
    case "TorrentContentSearch":
      return {
        torrentContent: {
          search: await search(
            (variables["input"] ?? {}) as SearchInput,
            signal,
          ),
        },
      };
    case "TorrentFiles":
      return {
        torrent: {
          files: await files(variables["input"] as FileInput, signal),
        },
      };
    case "HealthCheck":
      return health(signal);
    default:
      throw new Error(`Unsupported bitmagnet operation: ${name}`);
  }
}

/** Keeps bitmagnet's Apollo-driven components while reading the Rats REST API. */
export const ratsApolloLink = new ApolloLink(
  (operation) =>
    new Observable((observer) => {
      const controller = new AbortController();
      route(operation.operationName, operation.variables, controller.signal)
        .then((data) => {
          observer.next({ data });
          observer.complete();
        })
        .catch((error) => observer.error(error));
      return () => controller.abort();
    }),
);
