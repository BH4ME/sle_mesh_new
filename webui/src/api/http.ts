const defaultRequestTimeoutMs = 1800;

function mergeJsonHeaders(init?: RequestInit): RequestInit | undefined {
  if (!init?.body) {
    return init;
  }
  return {
    headers: { "content-type": "application/json" },
    ...init,
  };
}

async function withTimeout<T>(path: string, run: (signal: AbortSignal) => Promise<T>): Promise<T> {
  const controller = new AbortController();
  const timeoutId = window.setTimeout(() => controller.abort(), defaultRequestTimeoutMs);
  try {
    return await run(controller.signal);
  } catch (error) {
    if (error instanceof DOMException && error.name === "AbortError") {
      throw new Error(`request timeout: ${path}`);
    }
    throw error;
  } finally {
    window.clearTimeout(timeoutId);
  }
}

export async function fetchJson<T>(path: string, init?: RequestInit): Promise<T> {
  const requestInit = mergeJsonHeaders(init);
  const response = await withTimeout(path, (signal) => fetch(path, { ...requestInit, signal }));
  if (!response.ok) {
    throw new Error(`${response.status} ${response.statusText}`);
  }
  return response.json() as Promise<T>;
}

export async function fetchAction(path: string): Promise<void> {
  const response = await withTimeout(path, (signal) => fetch(path, { redirect: "manual", signal }));
  if (response.ok || response.type === "opaqueredirect" || (response.status >= 300 && response.status < 400)) {
    return;
  }
  throw new Error(`${response.status} ${response.statusText}`);
}
