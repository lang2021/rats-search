export type RatsTorrent = {
  hash: string;
  name: string;
  size: number;
  files: number;
  files_list?: Array<{ path: string; size: number }>;
  added: number;
  seeders: number;
  leechers: number;
  contentType: string;
  contentCategory: string;
};

type RatsEnvelope<T> = {
  success: boolean;
  data?: T;
  error?: string;
};

/** The existing Rats Search HTTP API uses method names under /api/. */
export async function ratsCall<T>(
  method: string,
  params: Record<string, unknown> = {},
  signal?: AbortSignal,
): Promise<T> {
  const response = await fetch(`/api/${method}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(params),
    signal,
  });
  const envelope = (await response.json()) as RatsEnvelope<T>;
  if (!response.ok || !envelope.success) {
    throw new Error(envelope.error ?? `Rats Search API: ${response.status}`);
  }
  return envelope.data as T;
}

export async function ratsReady(signal?: AbortSignal): Promise<boolean> {
  const response = await fetch("/readyz", { signal });
  return response.ok;
}
