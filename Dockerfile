# ============================================================================
# Rats Search v2 — C++/Qt6 Docker Build
# ============================================================================

# === Build Stage ===
FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG UBUNTU_APT_MIRROR

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    if [ -n "$UBUNTU_APT_MIRROR" ]; then \
        sed -i "s|^URIs: .*|URIs: $UBUNTU_APT_MIRROR|" /etc/apt/sources.list.d/ubuntu.sources; \
    fi && \
    rm -f /etc/apt/apt.conf.d/docker-clean && \
    apt-get -o Acquire::Retries=3 -o APT::Update::Error-Mode=any update && \
    apt-get -o Acquire::Retries=5 -o Acquire::http::Pipeline-Depth=0 \
        install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    qt6-base-dev \
    qt6-websockets-dev \
    qt6-tools-dev \
    libmaxminddb-dev \
    libgl1-mesa-dev \
    libxkbcommon-dev

WORKDIR /src
COPY . .

# Build the existing application target for console runtime (no tests).
RUN cmake -B build -G "Ninja" \
    -DCMAKE_BUILD_TYPE=Release \
    -DRATS_SEARCH_USE_SYSTEM_LIBRATS=OFF \
    -DRATS_SEARCH_BUILD_TESTS=OFF

RUN cmake --build build --config Release --parallel

FROM node:22-bookworm-slim AS webui
ARG NPM_REGISTRY=https://registry.npmjs.org
WORKDIR /src/webui-react
COPY webui-react/package.json webui-react/package-lock.json ./
RUN --mount=type=cache,target=/root/.npm \
    npm ci --registry="$NPM_REGISTRY" --audit=false --maxsockets=2 \
        --fetch-retries=3 --fetch-timeout=30000
COPY webui-react/ ./
RUN npm run build

# Select the Manticore binaries for the image's target architecture.
FROM ubuntu:24.04 AS manticore
ARG TARGETARCH
COPY imports/linux/ /tmp/manticore/
RUN case "$TARGETARCH" in \
        amd64) arch=x64 ;; \
        arm64) arch=arm64 ;; \
        *) echo "Unsupported Linux architecture: $TARGETARCH" >&2; exit 1 ;; \
    esac && \
    mkdir /out && \
    cp "/tmp/manticore/$arch/searchd" "/tmp/manticore/$arch/indexer" \
       "/tmp/manticore/$arch/indextool" /out/

# === Runtime Stage ===
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG UBUNTU_APT_MIRROR

# Install only the runtime libraries needed
RUN if [ -n "$UBUNTU_APT_MIRROR" ]; then \
        sed -i "s|^URIs: .*|URIs: $UBUNTU_APT_MIRROR|" /etc/apt/sources.list.d/ubuntu.sources; \
    fi && \
    apt-get -o Acquire::Retries=3 -o APT::Update::Error-Mode=any update && \
    apt-get -o Acquire::Retries=5 -o Acquire::http::Pipeline-Depth=0 \
        install -y --no-install-recommends \
    libqt6core6t64 \
    libqt6gui6t64 \
    libqt6widgets6t64 \
    libqt6network6t64 \
    libqt6sql6t64 \
    libqt6sql6-mysql \
    libqt6sql6-sqlite \
    libqt6concurrent6t64 \
    libqt6websockets6 \
    libmaxminddb0 \
    libgl1 \
    libxkbcommon0 \
    ca-certificates \
    curl \
    jq \
    util-linux \
    && rm -rf /var/lib/apt/lists/*

# Remove default ubuntu user (may conflict with host UID 1000)
RUN userdel -f ubuntu 2>/dev/null; rm -rf /home/ubuntu || true

# Create non-root user
RUN groupadd -r rats && useradd -r -g rats -d /app -s /sbin/nologin rats

WORKDIR /app

# Copy built binary
COPY --from=builder /src/build/bin/RatsSearch /app/

# Copy Manticore search engine binaries for the selected architecture.
COPY --from=manticore /out/ /app/

# Copy the React production build.
COPY --from=webui /src/webui-react/dist/ /app/webui/
COPY docker/nas-first-run.json /app/nas-first-run.json

RUN chmod +x /app/RatsSearch /app/searchd /app/indexer /app/indextool

# Copy and set up entrypoint (runs as root to fix /data permissions)
COPY docker-entrypoint.sh /app/docker-entrypoint.sh
COPY docker/rats-import /app/rats-import
RUN chmod +x /app/docker-entrypoint.sh /app/rats-import

# Persistent data directory for database, config, and logs
VOLUME /data

# HTTP API and authenticated WebSocket ports.
EXPOSE 8095/tcp 8096/tcp

# Health check for Kubernetes
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -f http://localhost:8095/healthz || exit 1

# Entrypoint runs as root to fix /data ownership, then drops to rats user
ENTRYPOINT ["/app/docker-entrypoint.sh"]

# CMD runs inside entrypoint (switched to rats user there)
CMD ["/app/RatsSearch", "--console", "--max-peers", "30", "--data-dir", "/data", "--webui-dir", "/app/webui", "--api-bind", "0.0.0.0", "--api-file-root", "/data/imports"]
