# Rats Search NAS ARM64 试运行

目标是 Linux/ARM64 Docker 主机。此部署只运行 Rats Search 和由它管理的 Manticore `searchd`；React 页面已包含在镜像中。不要把 Mac 的 `rats.json` 直接复制到 NAS，尤其不能带入 `independentDirect=true`。

本地另行交付的修复包包含可直接导入的 `rats-search-nas-arm64-20261108-mapfix.tar`、`geoip-city.mmdb`、本 Compose 文件和 `SHA256SUMS`；这些镜像与 GeoIP 数据不在 Git 仓库中。从源码检出时需要自行构建镜像并取得符合许可的 GeoIP 数据。

## 在 Mac 上制作镜像包

在项目根目录执行：

```sh
docker buildx build --platform linux/arm64 --load \
  --build-arg UBUNTU_APT_MIRROR=http://mirrors.ustc.edu.cn/ubuntu-ports/ \
  --build-arg NPM_REGISTRY=https://registry.npmmirror.com \
  -t rats-search:nas-arm64 .
docker save rats-search:nas-arm64 > "$HOME/Downloads/rats-search-nas-arm64-20261108-mapfix.tar"
shasum -a 256 "$HOME/Downloads/rats-search-nas-arm64-20261108-mapfix.tar"
```

镜像站参数只影响这次构建，不改变 Mac、NAS 或 Clash 的系统网络配置。APT 仍验证 Ubuntu 仓库签名。若默认软件源可用，可以省略两项镜像参数。镜像包保存在项目目录外，避免误提交。

## 在 NAS 上安装

将镜像包和 `docker-compose.nas.yml` 放在 NAS 的同一目录，然后以有权访问目标存储的 NAS 用户执行：

```sh
mkdir -p rats-data
docker load -i rats-search-nas-arm64-20261108-mapfix.tar
docker compose -f docker-compose.nas.yml up -d
docker compose -f docker-compose.nas.yml ps
```

## 已运行容器更新地图

先确认当前容器的 `/data` 对应 NAS 上的哪个目录，并保留该目录。停止容器后备份整个数据目录；再导入修复版镜像、将 `geoip-city.mmdb` 复制到原数据目录根部，以**同一目录**重新创建容器。Compose 安装可在镜像导入后执行 `docker compose -f docker-compose.nas.yml up -d --force-recreate`。若在绿联 Docker 图形界面安装，继续使用原有 40913→8095/TCP 映射，并增加 40914→8096/TCP；其他端口和数据目录沿用原值。不要选择“删除容器数据”或创建空数据目录。启动后检查 `/assets/ne-110m-world.geojson` 返回 200、地图出现真实定位节点，并在实时事件页确认“实时连接中”。

`./rats-data` 是持久目录，包含 `rats.json`、API 令牌、Manticore 索引、DHT 联系人、Watch/探查数据库以及应用日志。**仅空目录**在首次启动时写入 NAS 配置：开启网络和 Crawler，保持 `independentDirect=false`，禁用 BT 文件下载与上传。已有数据目录和配置不会被入口脚本覆盖。请从 NAS 上的 `rats-data/api-token` 读取令牌，在 `http://NAS地址:8095` 的页面输入；不要把令牌贴入公开日志。

世界地图需要将交付包中的 `geoip-city.mmdb` 放在 `rats-data/geoip-city.mmdb`，并重启容器以加载它。该文件来自 DB-IP City Lite；页面已注明数据来源。不要把文件放在镜像内的 `/app`，因为地图接口只从 `/data` 读取。若 `rats-data/geoip-city.mmdb` 已存在，先核对现有文件，不要直接覆盖。

如果使用 NAS 图形界面手动映射端口，HTTP 和 WebSocket 必须成对映射。比如页面使用宿主端口 **40913 → 容器 8095/TCP**，还须增加 **40914 → 容器 8096/TCP**；React 生产页面按当前页面端口加 1 连接 WebSocket。缺少第二条映射时，总览实时事件与 Trawl 无法接收推送。使用下方 Compose 文件则默认映射宿主 8095/8096。修改端口映射通常需要重建容器，但保留原 `rats-data` 挂载目录。

Compose 对容器总内存设置约 2.5 GiB 硬上限；容器 stdout/stderr 保留 3 份、每份 10 MB。应用自己的日志在 `rats-data`。正式采集前确认 NAS 的 8095/TCP、8096/TCP、4444/TCP/UDP、6881/TCP/UDP 未被占用，并只在可信局域网开放 Web/API；当前入口不提供 TLS。

## 七天试运行观察

每天核对一次：

```sh
docker compose -f docker-compose.nas.yml ps
docker stats --no-stream rats-search
docker inspect --format '{{.RestartCount}} {{.State.Health.Status}}' rats-search
du -sh rats-data
df -h rats-data
docker compose -f docker-compose.nas.yml logs --tail=100 rats-search
```

在 Web 总览确认 API、数据库、DHT Socket、Crawler 状态，记录路由节点、入队、元数据成功/失败和本地索引数。容器健康检查只证明 HTTP API 能响应；数据库、DHT 与采集是否正常仍需看页面状态及应用日志。数据目录所在 NAS 存储的容量/配额需要在 NAS 上设置，容器内存上限不能限制索引文件增长。

Mac Docker Desktop 的网络与 NAS Linux Docker 不同：Mac 上即使 DHT Socket 和 Crawler 已启动，路由节点仍可能为零。NAS 实机首轮验收须看到 DHT 有效响应和路由节点增长；若持续为零，先检查 NAS 的 UDP 出站/入站、DNS 和防火墙，不能仅凭容器健康状态判定采集正常。

停止时使用 `docker compose -f docker-compose.nas.yml stop`，等待应用正常停止 `searchd`。升级镜像时保留整个 `rats-data` 目录，不执行 `down -v` 或删除数据目录。
