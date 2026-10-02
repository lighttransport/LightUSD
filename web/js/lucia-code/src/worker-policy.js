import { LuciaError } from './utils.js';
export const WORKER_MAX_BYTES = 256 * 1024 * 1024;
export function workerPayloadBytes(value, limit = WORKER_MAX_BYTES) {
  if (!Number.isSafeInteger(limit) || limit < 1 || limit > WORKER_MAX_BYTES) throw new LuciaError('LUCIA_WORKER_MEMORY', 'Invalid worker memory budget.');
  const seen = new Set(), pending = [value]; let bytes = 0, count = 0;
  while (pending.length) {
    const item = pending.pop();
    if (typeof item === 'string') { bytes += item.length * 2; }
    else if (item && typeof item === 'object' && !seen.has(item)) {
      seen.add(item);
      if (++count > 100000) throw new LuciaError('LUCIA_WORKER_MEMORY', 'Worker input has too many objects.');
      if (ArrayBuffer.isView(item)) pending.push(item.buffer);
      else if (item instanceof ArrayBuffer) bytes += item.byteLength;
      else {
        if (Array.isArray(item)) bytes += item.length * 8;
        const keys = Object.keys(item);
        if (keys.length > 100000) throw new LuciaError('LUCIA_WORKER_MEMORY', 'Worker input has too many entries.');
        for (const key of keys) pending.push(item[key]);
      }
    }
    if (bytes > limit) throw new LuciaError('LUCIA_WORKER_MEMORY', 'Worker input exceeds its memory budget.');
  }
  return bytes;
}
export function monitorWorker(owner, worker, reject, { timeoutMs = 600000, signal = null } = {}) {
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 600000) throw new LuciaError('LUCIA_WORKER_TIMEOUT', 'Invalid worker timeout.');
  const terminate = worker.terminate.bind(worker);
  const stop = error => {
    if (owner.worker !== worker) return;
    owner.worker = null; owner.workerReject = null; worker.terminate(); reject(error);
  };
  const abort = () => stop(new LuciaError('LUCIA_CANCELLED', 'The worker operation was cancelled.'));
  const timer = setTimeout(() => stop(new LuciaError('LUCIA_WORKER_TIMEOUT', 'The worker operation timed out.')), timeoutMs);
  timer.unref?.();
  worker.terminate = () => { clearTimeout(timer); signal?.removeEventListener('abort', abort); return terminate(); };
  signal?.addEventListener('abort', abort, { once: true });
  if (signal?.aborted) abort();
  return owner.worker === worker;
}
