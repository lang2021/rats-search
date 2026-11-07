import type { ReactNode } from "react";

const suffixes = ["K", "M", "B"];
const fullNumber = new Intl.NumberFormat("zh-CN", { maximumFractionDigits: 20 });

export function formatFullCount(value: number): string {
  return fullNumber.format(value);
}

export function formatCompactCount(value: number): string {
  if (!Number.isFinite(value)) return "—";
  const absolute = Math.abs(value);
  if (absolute < 1_000) return formatFullCount(value);

  const unitIndex = Math.min(Math.floor(Math.log10(absolute) / 3) - 1, suffixes.length - 1);
  const divisor = 1_000 ** (unitIndex + 1);
  const scaled = Math.floor((absolute / divisor) * 10) / 10;
  return `${value < 0 ? "−" : ""}${scaled.toFixed(1)}${suffixes[unitIndex]}`;
}

export function CompactNumber({ value }: { value?: number | null }): ReactNode {
  if (value == null || !Number.isFinite(value)) return <span>—</span>;
  const full = formatFullCount(value);
  return <span className="compact-number" title={full} aria-label={full}>{formatCompactCount(value)}</span>;
}
