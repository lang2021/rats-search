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
  remote?: boolean;
  peer?: string;
  isFileMatch?: boolean;
};

type RatsEnvelope<T> = {
  success: boolean;
  data?: T;
  error?: string;
};

const tokenKey = "rats-api-token";
let tokenRequest: Promise<string> | undefined;

export function ratsAuthToken(): string {
  return sessionStorage.getItem(tokenKey) ?? "";
}

export function requestRatsToken(): Promise<string> {
  const saved = ratsAuthToken();
  if (saved) return Promise.resolve(saved);
  if (tokenRequest) return tokenRequest;
  tokenRequest = new Promise<string>((resolve) => {
    const dialog = document.createElement("dialog");
    dialog.setAttribute("aria-label", "API 令牌");
    dialog.style.cssText = "background:#10120d;color:#e6e9df;border:1px solid #b5ff00;padding:24px;max-width:min(420px,90vw)";
    const form = document.createElement("form");
    form.method = "dialog";
    const label = document.createElement("label");
    label.textContent = "输入本机数据目录 api-token 文件中的 API 令牌";
    const input = document.createElement("input");
    input.type = "password";
    input.autocomplete = "off";
    input.required = true;
    input.style.cssText = "display:block;box-sizing:border-box;width:100%;margin:16px 0;padding:10px;background:#080908;color:#fff;border:1px solid #7b8c45";
    const submit = document.createElement("button");
    submit.type = "submit";
    submit.textContent = "连接 API";
    submit.style.cssText = "padding:9px 16px;background:#b5ff00;color:#080908;border:0;cursor:pointer";
    label.appendChild(input);
    form.append(label, submit);
    dialog.appendChild(form);
    document.body.appendChild(dialog);
    let submitted = false;
    form.addEventListener("submit", () => { submitted = true; });
    dialog.addEventListener("close", () => {
      const token = submitted ? input.value.trim() : "";
      if (token) sessionStorage.setItem(tokenKey, token);
      dialog.remove();
      tokenRequest = undefined;
      resolve(token);
    }, { once: true });
    dialog.showModal();
    input.focus();
  });
  return tokenRequest;
}

async function authorizationHeaders(): Promise<Record<string, string>> {
  const token = await requestRatsToken();
  return token ? { Authorization: `Bearer ${token}` } : {};
}

export function summarizeRatsEvent(frame: unknown): { name: string; detail: string } | null {
  if (!frame || typeof frame !== "object") return null;
  const message = frame as { event?: unknown; data?: unknown };
  if (typeof message.event !== "string") return null;
  const data = message.data && typeof message.data === "object" ? message.data as Record<string, unknown> : {};
  if (Array.isArray(data["changedKeys"])) return { name: message.event, detail: `变更：${data["changedKeys"].join(", ")}` };
  if (Array.isArray(data["torrents"])) return { name: message.event, detail: `结果：${data["torrents"].length} 条` };
  const fields = ["hash", "searchId", "processed", "total", "percent", "cancelled", "good", "bad", "status"];
  const detail = fields.filter(key => data[key] != null).map(key => `${key}: ${String(data[key])}`).join(" · ");
  return { name: message.event, detail: detail || "事件已接收" };
}

/** The existing Rats Search HTTP API uses method names under /api/. */
export async function ratsCall<T>(
  method: string,
  params: Record<string, unknown> = {},
  signal?: AbortSignal,
): Promise<T> {
  const response = await fetch(`${ratsApiBaseUrl}/api/${method}`, {
    method: "POST",
    headers: { "Content-Type": "application/json", ...await authorizationHeaders() },
    body: JSON.stringify(params),
    signal,
  });
  const envelope = (await response.json()) as RatsEnvelope<T>;
  if (response.status === 401) {
    sessionStorage.removeItem(tokenKey);
    throw new Error("API 令牌无效；刷新页面后重新输入。");
  }
  if (!response.ok || !envelope.success) {
    throw new Error(envelope.error ?? `Rats Search API: ${response.status}`);
  }
  return envelope.data as T;
}

export async function ratsSafeSearch(signal?: AbortSignal): Promise<boolean> {
  const config = await ratsCall<{ safeSearch: boolean }>("config.get", {}, signal);
  return config.safeSearch === true;
}

export async function ratsReady(signal?: AbortSignal): Promise<boolean> {
  const response = await fetch(`${ratsApiBaseUrl}/readyz`, { signal, headers: await authorizationHeaders() });
  return response.ok;
}
import { ratsApiBaseUrl } from "../../environments/environment";
