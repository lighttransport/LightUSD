import { LuciaError } from './utils.js';

export function validationSummary(result) {
  if (!result || typeof result !== 'object' || Array.isArray(result) ||
      !['ok', 'valid', 'parse_ok', 'issues', 'errors', 'error'].some(key => key in result))
    throw new LuciaError('LUCIA_VALIDATION', 'USD validation returned an invalid result.');
  for (const key of ['issues', 'errors']) {
    if (result[key] !== undefined && !Array.isArray(result[key]))
      throw new LuciaError('LUCIA_VALIDATION', 'USD validation returned invalid issues.');
  }
  for (const key of ['ok', 'valid', 'parse_ok']) {
    if (result[key] !== undefined && typeof result[key] !== 'boolean')
      throw new LuciaError('LUCIA_VALIDATION', 'USD validation returned an invalid status.');
  }
  const issues = [...(result.issues || []), ...(result.errors || [])];
  if (issues.some(issue => typeof issue !== 'string' &&
      (!issue || typeof issue !== 'object' || Array.isArray(issue))))
    throw new LuciaError('LUCIA_VALIDATION', 'USD validation returned an invalid issue.');
  if (result.error) issues.push({ severity: 'error', message: String(result.error) });
  const hasErrors = result.ok === false || result.valid === false || result.parse_ok === false ||
    result.error_count > 0 || (result.errors?.length || 0) > 0 || !!result.error ||
    issues.some(issue => typeof issue === 'string' ? /\berror\b/i.test(issue) :
      /^(error|fatal)$/i.test(issue?.severity || ''));
  return { issues, hasErrors };
}
