# Rats Search

Rats Search is a local-first BitTorrent resource indexer and search application. Its C++/Qt backend discovers InfoHashes through the DHT, obtains torrent metadata where peers make it available, and indexes resource names and file lists in Manticore Search. The current Web interface uses React, TypeScript and Vite. A desktop Qt interface and a headless console mode are also available.

The index stores **metadata**, not the referenced media or software. Discovery, a queued Hash, a completed BEP 9 exchange and a successful database write are different stages. A discovered Hash is not a searchable resource until indexing succeeds.

## What is available

- Local search, file-path matches, resource details and magnet links; optional search across connected Rats peers.
- DHT discovery through BEP 51 sampling and passive DHT requests, with deduplication, peer hints and selective completion of partial records.
- A dashboard with collection statistics, a DHT/P2P map, and a session-only Trawl event stream.
- Persistent keyword Watch rules and unread matches for **newly indexed** resource names. Watch does not search the network on its own or scan old records when a rule is created.
- Network, crawler, indexing and transfer controls; REST API and authenticated WebSocket events.
- A Magnet Explorer page for bounded metadata and network-clue inspection. **This feature remains under review**; its real-world results are not yet considered complete.

Downloading or uploading BT content files is controlled separately from metadata acquisition. See [unfinished work and observed limits](docs/待完成与复盘.md) before relying on the NAS map, NAS live events or Magnet Explorer.

## Repository layout

| Path | Purpose |
| --- | --- |
| `src/` | Qt application, crawler, API, indexing and data services |
| `src/librats/` | Separate Git submodule for DHT, BT and Rats P2P protocols |
| `imports/` | Manticore binaries supplied as a Git submodule |
| `webui-react/` | Current React + TypeScript + Vite Web frontend |
| `tests/` | C++ regression tests |
| `docs/` | API, operations and deployment notes |

The former Angular, Vue and Electron frontends remain recoverable from Git history; they are not current build inputs.

## Build and run locally

Install CMake, Ninja, a C++17 compiler, Qt 6 with Network, SQL and WebSockets, and the dependencies described by CMake. Initialize both submodules before building:

```sh
git clone --recurse-submodules https://github.com/lang2021/rats-search.git
cd rats-search
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

For the Web UI, use Node.js and the committed npm lockfile:

```sh
cd webui-react
npm ci
npm run dev
```

Vite provides hot reload. Its development proxy defaults to an **isolated** API at `127.0.0.1:18095` and WebSocket at `127.0.0.1:18096`; set `RATS_API_ORIGIN` and `RATS_WS_ORIGIN` in a local environment file when using other test ports. The backend must be running separately. `npm run build` creates `webui-react/dist`. From the repository root, Rats Search serves that directory by default; `--webui-dir` selects another built directory.

For a headless instance, run the built `RatsSearch` binary with explicit data and Web paths:

```sh
./build/bin/RatsSearch --console --data-dir /path/to/rats-data \
  --webui-dir ./webui-react/dist
```

The actual binary location may differ on packaged desktop builds. Network and crawler startup follow the saved configuration; `--spider` explicitly enables crawling for that run. A first run creates configuration and an API token in the data directory. Open the Web UI on the configured HTTP port (normally `8095`). Read the token from `<data-dir>/api-token` and enter it in the browser; do not put it in a URL, source file or log. REST requests use `Authorization: Bearer <token>`; the WebSocket listens on the next port (normally `8096`) and authenticates after connection. The Web app remembers the token in that site's browser storage until it is forgotten or rejected.

To build and run the C++ tests, configure with `-DRATS_SEARCH_BUILD_TESTS=ON`, then run `ctest --test-dir build --output-on-failure`. Tests are off by default in the normal build.

## Docker and NAS

The Dockerfile builds the C++ application, React static files and an architecture-matched Manticore `searchd` into one Linux image. Rats Search starts and manages `searchd`; `/data` holds the configuration, database, index, DHT contacts, Watch and Explorer stores, token and logs. Keep the **same persistent `/data` mount** when recreating a container.

For the Linux/ARM64 NAS package and core-only Compose configuration, follow [NAS ARM64 instructions](docs/NAS_ARM64.md). The Compose example maps both HTTP `8095/TCP` and WebSocket `8096/TCP`, applies a memory limit, log rotation and a restart policy. If the NAS assigns a different HTTP host port, its WebSocket host port must be the next number because the current production frontend derives the WebSocket address that way. Docker's health check confirms the HTTP service responds; it does not prove that live events, GeoIP, DHT and indexing all work.

The world map uses bundled Natural Earth outlines and an **optional local** DB-IP City Lite database at `<data-dir>/geoip-city.mmdb` for approximate locations. It does not send peer IP addresses to an online geolocation service. Location data is separate from the DHT node count; unlocated nodes remain in the count.

The API uses a token but does not provide TLS. Expose it only on a trusted network or place it behind a TLS reverse proxy. [API reference](docs/API.md) · [React development notes](webui-react/README.md)

## Status and history

The Web migration and NAS deployment are still being validated. The [unfinished-work list](docs/待完成与复盘.md) records confirmed gaps without presenting them as completed features. Existing databases, settings and user data are not part of this repository.

Rats Search builds on [librats](https://github.com/librats/librats) and Manticore Search. See [LICENSE](LICENSE) and the submodule notices for licensing details.
