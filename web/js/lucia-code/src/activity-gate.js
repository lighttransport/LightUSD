import { LuciaError } from './utils.js';

// Native authoring and viewport conversion share one session. Replacements
// cancel workers, wait for rollback, and supersede older asynchronous reads.
export class LuciaActivityGate {
  constructor(cancel = () => {}) {
    this.cancel = cancel;
    this.active = null;
    this.epoch = 0;
    this.pending = 0;
  }
  acquire() {
    if (this.active || this.pending)
      throw new LuciaError('LUCIA_BUSY', 'Wait for the current scene operation to finish.');
    return this.claim();
  }
  claim() {
    let release;
    const lease = { done: new Promise(resolve => { release = resolve; }) };
    lease.release = () => {
      if (this.active === lease) this.active = null;
      release();
    };
    this.active = lease;
    return lease;
  }
  beginReplacement() {
    this.pending = ++this.epoch;
    if (this.active) this.active.cancelled = true;
    this.cancel();
    return this.epoch;
  }
  current(epoch) { return this.epoch === epoch; }
  async replacement(epoch) {
    while (this.active) await this.active.done;
    return this.current(epoch) ? this.claim() : null;
  }
  finishReplacement(epoch) {
    if (this.pending === epoch) this.pending = 0;
  }
}
