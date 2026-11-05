// A one-shot search history selection stays in memory, never in the URL.
let queuedLocalSearch: string | undefined;

export function queueLocalSearch(query: string): void {
  queuedLocalSearch = query;
}

export function takeQueuedLocalSearch(): string | undefined {
  const query = queuedLocalSearch;
  queuedLocalSearch = undefined;
  return query;
}
