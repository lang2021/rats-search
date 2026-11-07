# Rats Search React Web UI

Independent React + TypeScript + Vite frontend. The Angular `webui-bitmagnet` app remains unchanged.

```sh
cp .env.example .env.local
npm install
npm run dev
```

The development proxy targets an isolated Rats Search API on `127.0.0.1:18095` and WebSocket on `127.0.0.1:18096`. Keep `networkEnabled` disabled in that test instance. The browser stores the API token in same-origin `localStorage` under the existing `rats-api-token` key, and migrates any legacy `sessionStorage` value on first use. Use the Settings page “Forget API token” action to clear it.

`npm run build` writes the production static app to `dist/`; provide that directory through `--webui-dir` for an isolated production-preview instance.
