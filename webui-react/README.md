# Rats Search React Web UI

React + TypeScript + Vite frontend for Rats Search.

```sh
cp .env.example .env.local
npm install
npm run dev
```

The development proxy targets an isolated Rats Search API on `127.0.0.1:18095` and WebSocket on `127.0.0.1:18096`. Keep `networkEnabled` disabled in that test instance. The browser stores the API token in same-origin `localStorage` under the existing `rats-api-token` key, and migrates any legacy `sessionStorage` value on first use. Use the Settings page “Forget API token” action to clear it.

`npm run build` writes the production static app to `dist/`. When Rats Search starts from the repository root without `--webui-dir`, it serves `webui-react/dist`. Use `--webui-dir` to serve a build from another location.

The overview network map uses an optional local GeoIP database. Build the native app with `libmaxminddb` available, then place a [DB-IP City Lite MMDB](https://db-ip.com/db/download/ip-to-city-lite) at `<data-dir>/geoip-city.mmdb` before starting Rats Search. The map still opens without it and labels location data unavailable. It never sends peer addresses to a geolocation service. Its world outlines are bundled Natural Earth public-domain data; the dashboard credits DB-IP for approximate IP locations.
