# Rats Search console preview

This directory keeps the bitmagnet Angular Web UI and adapts its search, detail,
health and dashboard components to the Rats Search HTTP API. The original Rats
Search `webui/` remains available as a fallback.

Upstream UI: [bitmagnet](https://github.com/bitmagnet-io/bitmagnet), source
revision `e31b30d7351c269dc801876f92ebc59d31fee545`. Its MIT license is in
`LICENSE.bitmagnet`. The Rats Search license and copyright remain in the
repository root.

## Native Mac development

Run Rats Search with its existing test configuration on port 18095. Then:

```sh
cd webui-bitmagnet
npm ci --ignore-scripts
npm run start -- --host 127.0.0.1 --port 14200 --proxy-config proxy.conf.json
```

On this Mac, the initial `npm ci` download stalled after the Angular packages
were extracted. The current preview uses the installed CLI directly:

```sh
node node_modules/@angular/cli/bin/ng.js serve --host 127.0.0.1 --port 14200 --proxy-config proxy.conf.json
```

Open <http://127.0.0.1:14200/>. Angular's dev server updates the page after
front-end edits without recompiling the C++ application. The proxy sends `/api`
and `/readyz` to the local Rats Search process. The Angular router uses hash
URLs, so a static build can also be served by the existing Rats HTTP server:

```sh
npm run build
# Start Rats Search with --webui-dir pointing to webui-bitmagnet/dist/bitmagnet/browser
```

This preview implements search, resource details and file lists, health,
statistics, P2P/DHT status, connected peers, and selected configuration
switches. Bitmagnet-only metadata enrichment, tags, queue management, and
advanced facets are left out because the corresponding Rats APIs do not exist.
