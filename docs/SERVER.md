# Server Configuration

This document covers the native Qt server configuration.

---

## Rats Search (Qt)

### Built-in API Server

Rats Search 2.0 includes a built-in REST/WebSocket API server that can be enabled in the settings.

**Enable via UI:** File → Settings → Network → "Enable REST API server"

**Or via configuration file** (`rats.json`):

```json
{
    "restApi": true,
    "httpPort": 8095
}
```

### Configuration File Location

The configuration file `rats.json` is created in the data directory:

| Platform | Location |
|----------|----------|
| Windows | `%APPDATA%/RatsSearch/rats.json` or application directory |
| Linux | `~/.config/RatsSearch/rats.json` or `~/.local/share/RatsSearch/` |
| macOS | `~/Library/Application Support/RatsSearch/rats.json` |

### Configuration Options

```json
{
    "p2pPort": 4444,
    "dhtPort": 6881,
    "httpPort": 8095,
    "restApi": false,
    "networkEnabled": false,
    "indexer": true,
    "trackers": true,
    "darkMode": false,
    "trayOnMinimize": true,
    "trayOnClose": false,
    "confirmExit": true,
    "startMinimized": false
}
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `p2pPort` | int | 4444 | P2P communication port |
| `dhtPort` | int | 6881 | DHT operations port |
| `httpPort` | int | 8095 | REST API server port |
| `restApi` | bool | false | Enable REST API server |
| `networkEnabled` | bool | false | Start the shared network at launch |
| `indexer` | bool | true | Start the DHT crawler when the network is available |
| `trackers` | bool | true | Enable tracker peer checking |
| `darkMode` | bool | false | Use dark theme |
| `uiFontFamily` | string | "" | Interface font family, tried before the theme's own font stack (empty = theme default) |
| `uiFontScale` | int | 100 | Interface text size in percent of the default (clamped to 50–200) |
| `trayOnMinimize` | bool | true | Hide to tray on minimize |
| `trayOnClose` | bool | false | Hide to tray on close |
| `confirmExit` | bool | true | Ask for confirmation before quitting the GUI |
| `startMinimized` | bool | false | Start minimized to tray |
