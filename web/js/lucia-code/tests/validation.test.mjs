import test from 'node:test';
import assert from 'node:assert/strict';
import { validationSummary } from '../src/validation.js';

test('native validation failures block export even without issues', () => {
  for (const result of [{ ok: false }, { parse_ok: false }, { valid: false },
    { issues: [], error_count: 1 }, { error: 'No loaded layer' }, { errors: ['Invalid layer'] }])
    assert.equal(validationSummary(result).hasErrors, true);
});
test('warnings mentioning errors do not become errors', () => {
  assert.equal(validationSummary({ ok: true, issues: [{ severity: 'warning', message: '0 errors; missing optional metadata' }] }).hasErrors, false);
  assert.equal(validationSummary({ issues: [{ severity: 'error', message: 'Invalid path' }] }).hasErrors, true);
});
test('malformed validation results cannot certify a stage', () => {
  for (const result of [null, {}, [], { ok: 'false' }, { issues: 'errors' }, { issues: [null] }])
    assert.throws(() => validationSummary(result), { code: 'LUCIA_VALIDATION' });
});
