#!/bin/sh
set -eu
umask 077
# Fix /data permissions when mounted from host (runs as root).
if [ -d /data ]; then
    HOST_UID=$(stat -c %u /data 2>/dev/null || echo "0")
    HOST_GID=$(stat -c %g /data 2>/dev/null || echo "0")
    RATS_UID=$(id -u rats 2>/dev/null || echo "0")

    if [ "$HOST_UID" = "0" ]; then
        # /data owned by root (fresh volume) — chown to rats
        chown rats:rats /data || { echo "Cannot assign /data to rats" >&2; exit 1; }
    elif [ "$HOST_UID" != "$RATS_UID" ]; then
        # /data owned by host user — sync rats uid/gid to match host
        # macOS and NAS mounts can use a GID already assigned in the image.
        # Matching the owning UID is sufficient for a user-owned data folder.
        groupmod -g "$HOST_GID" rats 2>/dev/null || true
        usermod -u "$HOST_UID" rats || exit 1
    fi

    # Seed only a genuinely empty volume. Never replace a user's existing
    # configuration or silently enable networking for an older data directory.
    existing_entries=$(find /data -mindepth 1 -maxdepth 1 ! -name lost+found -print -quit) || {
        echo "Cannot inspect /data; refusing first-run initialization" >&2
        exit 1
    }
    if [ ! -e /data/rats.json ] && [ -z "$existing_entries" ]; then
        cp /app/nas-first-run.json /data/rats.json || exit 1
        chown rats:rats /data/rats.json || exit 1
    fi
fi

# Replace this shell with RatsSearch so Docker's TERM reaches the application.
exec setpriv --reuid=rats --regid=rats --init-groups -- "$@"
