import { LuciaError, encoder } from './utils.js';

function interval(before, after, offset = 0) {
  let start = 0, endBefore = before.length, endAfter = after.length;
  while (start < endBefore && start < endAfter && before[start] === after[start]) start++;
  while (endBefore > start && endAfter > start && before[endBefore - 1] === after[endAfter - 1]) { endBefore--; endAfter--; }
  return { start: offset + start, removed: before.slice(start, endBefore), inserted: after.slice(start, endAfter) };
}

// Equal line counts let independent property edits use separate intervals.
// Changed line structure falls back to the general single-interval format.
function alignedIntervals(before, after, budget) {
  const edits = [];
  let a = 0, b = 0, bytes = 0;
  while (a < before.length || b < after.length) {
    const newlineA = before.indexOf('\n', a), newlineB = after.indexOf('\n', b);
    if ((newlineA < 0) !== (newlineB < 0)) return null;
    const endA = newlineA < 0 ? before.length : newlineA + 1;
    const endB = newlineB < 0 ? after.length : newlineB + 1;
    const left = before.slice(a, endA), right = after.slice(b, endB);
    if (left !== right) {
      const edit = interval(left, right, a);
      bytes += 2 * (edit.removed.length + edit.inserted.length) + 128;
      if (bytes >= budget || edits.length >= 256) return null;
      edits.push(edit);
    }
    a = endA; b = endB;
  }
  return edits.length > 1 ? edits : null;
}

async function digest(source) {
  const bytes = await crypto.subtle.digest('SHA-256', encoder.encode(source));
  return Array.from(new Uint8Array(bytes), value => value.toString(16).padStart(2, '0')).join('');
}

// Store only the changed interval. Hash both complete states to reject stale
// history without retaining either full stage in the command.
export async function createSourceDelta(before, after) {
  const single = interval(before, after);
  const budget = 2 * (single.removed.length + single.inserted.length);
  const edits = budget > 4096 ? alignedIntervals(before, after, budget) : null;
  // V8 substring views can retain the entire original stage. JSON copies own
  // only the changed text and preserve UTF-16 even at surrogate boundaries.
  const ownText = value => JSON.parse(JSON.stringify(value));
  const ownEdit = edit => ({ start: edit.start, removed: ownText(edit.removed), inserted: ownText(edit.inserted) });
  const [beforeHash, afterHash] = await Promise.all([digest(before), digest(after)]);
  return { ...(edits ? { edits: edits.map(ownEdit) } : ownEdit(single)),
    beforeLength: before.length, afterLength: after.length,
    beforeHash, afterHash };
}

export async function applySourceDelta(source, delta, direction) {
  const undo = direction === 'undo';
  if (!undo && direction !== 'redo') throw new LuciaError('LUCIA_HISTORY', 'Invalid history direction.');
  const length = undo ? delta.afterLength : delta.beforeLength;
  const hash = undo ? delta.afterHash : delta.beforeHash;
  if (source.length !== length || await digest(source) !== hash)
    throw new LuciaError('LUCIA_HISTORY_STALE', 'The working stage differs from this undo record.');
  const edits = delta.edits || [delta], parts = [];
  if (!Array.isArray(edits) || !edits.length || edits.length > 256) throw new LuciaError('LUCIA_HISTORY', 'The undo record is damaged.');
  let cursor = 0, shift = 0;
  for (const edit of edits) {
    if (!edit || typeof edit !== 'object') throw new LuciaError('LUCIA_HISTORY', 'The undo record is damaged.');
    const remove = undo ? edit.inserted : edit.removed, insert = undo ? edit.removed : edit.inserted;
    const start = edit.start + (undo ? shift : 0);
    if (!Number.isSafeInteger(edit.start) || edit.start < 0 || typeof remove !== 'string' || typeof insert !== 'string' || start < cursor || start > source.length - remove.length)
      throw new LuciaError('LUCIA_HISTORY', 'The undo record is damaged.');
    if (source.slice(start, start + remove.length) !== remove)
      throw new LuciaError('LUCIA_HISTORY_STALE', 'The working stage differs from this undo record.');
    parts.push(source.slice(cursor, start), insert);
    cursor = start + remove.length;
    shift += edit.inserted.length - edit.removed.length;
  }
  parts.push(source.slice(cursor));
  const result = parts.join('');
  if (await digest(result) !== (undo ? delta.beforeHash : delta.afterHash))
    throw new LuciaError('LUCIA_HISTORY', 'The undo record is damaged.');
  return result;
}
