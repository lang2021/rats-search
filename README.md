# Rats Search · WebNAS Fork

这是基于 [librats/rats-search](https://github.com/librats/rats-search) 开发的个人 fork，重点是把 Rats Search 做成可长期运行、可通过浏览器管理的 DHT 元数据索引服务，并为 ARM64 NAS 部署做适配。它不是上游项目的官方版本，也不代表上游已经包含本文列出的改动。

项目由 C++/Qt 后端、[librats](https://github.com/librats/librats) 网络库、Manticore Search 索引和 React Web 控制台组成。索引保存 Torrent 元数据和文件路径，不下载所指向的视频、软件或其他内容文件。发现 Hash、进入调查队列、取得 BEP 9 元数据和成功写入索引是不同阶段。

## 这个 fork 做了什么

- **换用 React Web 控制台。** 现役前端使用 React、TypeScript 和 Vite，提供本地资源搜索、文件路径匹配、详情、搜索历史、资源关注以及系统管理页面。Angular、Vue 和旧 Electron Web 已从当前构建中移除，仍可在 Git 历史中查阅。
- **扩展资源发现和调查。** 在现有 DHT 流程中接入 BEP 51 主动样本与被动 DHT 线索，增加队列去重、Peer 补充和对不完整记录的选择性元数据补齐。网络发现策略仍由正常 Crawler 工作流执行；Watch 关键词不会触发额外 DHT 搜索。
- **增加持久化资源关注。** 用户可管理关键词规则，并查看新入库资源名称命中规则后产生的提醒。规则和提醒保存在本机 `watch.sqlite`；创建规则不会回扫旧索引。
- **建设采集控制台。** 总览展示索引和采集统计；DHT/P2P 地图使用随前端提供的 Natural Earth 底图，并可选用本地 DB-IP City Lite 数据库显示近似位置；Trawl 展示当前 Web 会话收到的实时事件，不是长期事件历史。
- **补充本地数据管理。** 提供本地索引分页浏览、数据库磁盘占用统计、可移植 `.ratsdb` 导入导出，以及 REST API 和鉴权 WebSocket。
- **准备 Linux/ARM64 Docker 部署。** 单个容器运行 Rats Search 和由它管理的 Manticore，`/data` 保存配置、索引、DHT 联系人、Watch/Explorer 数据、API 令牌和日志。公开的试用镜像见 [NAS ARM64 预发布包](https://github.com/lang2021/rats-search/releases/tag/nas-arm64-map-watchfix-20261110)。

## 当前状态

这是一份仍在开发和验收中的 fork。Mac 原生运行、React/Vite 构建和本地 ARM64 容器曾分别进行验证；这些结果不等于任意 NAS 上已完成验收。部署时要保留同一个 `/data` 挂载，并映射 HTTP `8095/tcp` 与 WebSocket `8096/tcp`。地图近似定位还需要把许可符合要求的 `geoip-city.mmdb` 放入持久化数据目录；地图底图可加载不代表 GeoIP 已加载或当前存在可定位节点。

**磁力探查（Magnet Explorer）仍属未完成实验功能。** 页面及后端接口已存在，但真实 Peer 发现、元数据获取和持久线索复用还没有达到可承诺的稳定程度。请勿将其视作完整的 BitTorrent 客户端或下载器。

其他尚未完成或部署中观察到的问题记录在[待完成与复盘](docs/待完成与复盘.md)。其中 NAS 地图、实时事件和角标等状态会随镜像、端口映射及数据目录变化；请以当前运行实例的 API 和日志为准。

## 仓库组成

- `src/`：Qt 应用、Crawler、REST/WebSocket、索引和持久化服务。
- `src/librats/`：独立 Git 子模块，包含 DHT、BitTorrent 和 Rats P2P 网络实现。
- `webui-react/`：现役 React + TypeScript + Vite 前端。
- `imports/`：Manticore 各平台运行二进制子模块。
- `tests/`：C++ 自动化测试。
- `docker/`、`Dockerfile`、`docker-compose.nas.yml`：容器构建和 NAS 部署配置。

## 本机开发

### 构建 C++ 后端

需要 C++17 编译器、CMake、Ninja、Qt 6（Core、Gui、Widgets、Network、Sql、WebSockets 等模块）以及 CMake 检查到的依赖。克隆时初始化子模块：

```sh
git clone --branch feat/webui-nas --recurse-submodules https://github.com/lang2021/rats-search.git
cd rats-search
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### 启动 React 开发页

```sh
cd webui-react
npm ci
npm run dev
```

Vite 默认把 REST、`/readyz` 和 WebSocket 代理到隔离开发后端 `127.0.0.1:18095/18096`。可在本机未跟踪的环境文件中设置 `RATS_API_ORIGIN` 与 `RATS_WS_ORIGIN` 指向其他测试实例。前端不会自动启动后端。生产静态文件通过 `npm run build` 生成到 `webui-react/dist`。

### 运行后端

先构建 React 静态文件，再使用独立数据目录启动：

```sh
./build/bin/RatsSearch --console \
  --data-dir ./rats-data \
  --webui-dir ./webui-react/dist \
  --api-bind 127.0.0.1
```

首次启动会在数据目录生成 `api-token`。在浏览器打开后输入该令牌；不要把令牌提交到仓库、URL 或日志。HTTP API 默认使用 `8095`，WebSocket 默认使用 `8096`。网络与 Crawler 是否启动由数据目录中的配置和命令行选项决定；启动前应检查 `rats.json`，不要将个人 Mac 网络设置直接复制到 Linux/NAS。

运行测试：

```sh
cmake -S . -B build -G Ninja -DRATS_SEARCH_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Docker 与 NAS

Docker 镜像包含 C++ 程序、React 生产静态文件和匹配目标架构的 Manticore。首次运行时，仅当 `/data` 是空目录，入口脚本才写入面向容器的初始配置；原有数据目录不会被这一步覆盖。镜像更新时继续挂载同一份 `/data`，不要删除卷或换成空目录，否则应用会像新安装一样从空索引开始。

ARM64 Compose 示例位于 [`docker-compose.nas.yml`](docker-compose.nas.yml)，具体安装步骤见 [`docs/NAS_ARM64.md`](docs/NAS_ARM64.md)。示例包含 2.5 GiB 容器内存上限、日志轮转、健康检查与重启策略。健康检查只说明 HTTP 服务可响应，不能证明 WebSocket、GeoIP、DHT 或采集均正常。默认 API 不提供 TLS，只应在可信网络访问或置于 TLS 反向代理之后。

NAS 包含 Linux/ARM64 镜像 tar、GeoIP 文件和 SHA256 清单；导入方式和当前文件名以 [GitHub 预发布附件](https://github.com/lang2021/rats-search/releases/tag/nas-arm64-map-watchfix-20261110)为准。GeoIP 数据单独提供，不嵌入数据库或配置；放入现有挂载目录的 `geoip-city.mmdb` 后，需要重启应用加载。节点位置是 IP 地理位置近似值，不是精确地址。

## 主要 API 能力

API 使用 Bearer 令牌鉴权。当前接口围绕以下用途组织：

- 本地 Torrent 与文件路径搜索、资源详情、分页浏览、搜索历史。
- Crawler、网络、索引统计、内容类型和网络地图快照。
- Watch 规则、未读数、提醒读取状态。
- 磁力探查会话、取消和快照查询（实验功能）。
- Manticore 数据导入导出、P2P 同步、配置和下载任务管理。

浏览器实时事件通过单独的 WebSocket 监听端口推送；Trawl 页面显示当前会话事件。完整请求参数和响应字段请以 [`docs/API.md`](docs/API.md) 和当前后端路由实现为准，旧版 API 说明可能需要继续校对。

## 许可证与上游

项目主仓库采用 MIT License，见 [`LICENSE`](LICENSE)。`src/librats` 与 `imports` 是独立子模块，各自的代码、二进制和数据遵循其上游许可与通知文件。该 fork 对上游项目的改造由本仓库提交历史记录；请勿将本 fork 的功能或 NAS 包装描述成上游官方支持。
