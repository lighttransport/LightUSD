const privatePath = (value) => /^(?:[A-Za-z]:[\\/]|[\\/]home[\\/]|[\\/]Users[\\/]|\\\\|(?:\.\.[\\/]))/.test(String(value || '')) || String(value || '').split(/[\\/]/).some((part) => part === '..');
const containsPrivatePath = (value) => typeof value === 'string' ? privatePath(value) : Array.isArray(value) ? value.some(containsPrivatePath) : value && typeof value === 'object' ? Object.values(value).some(containsPrivatePath) : false;

export function createProcessingRecipe(input = {}) {
  const { profile = 'web-viewer', operations = [] } = input && typeof input === 'object' && !Array.isArray(input) ? input : {};
  const safeOperations = (Array.isArray(operations) ? operations : []).filter((operation) => operation && typeof operation === 'object' && !Array.isArray(operation)).map((operation) => ({
    id: String(operation.id || ''), fix: String(operation.fix || ''), path: String(operation.path || ''),
    dependsOn: [...new Set(Array.isArray(operation.dependsOn) ? operation.dependsOn.filter((dependency) => typeof dependency === 'string' && dependency) : [])].sort(), options: operation.options && typeof operation.options === 'object' && !Array.isArray(operation.options) ? { ...operation.options } : {},
    ...(Number.isInteger(operation.priority) ? { priority: operation.priority } : {}),
    ...(Number.isFinite(operation.estimatedWork) && operation.estimatedWork >= 0 ? { estimatedWork: operation.estimatedWork } : {}),
    ...(Array.isArray(operation.issueIds) && operation.issueIds.length ? { issueIds: [...new Set(operation.issueIds.filter((issueId) => typeof issueId === 'string' && issueId))].sort() } : {}),
  })).filter((operation) => operation.id && operation.fix && operation.path && !privatePath(operation.path) && !containsPrivatePath(operation.options)).sort((a, b) => (a.priority ?? Infinity) - (b.priority ?? Infinity) || a.id.localeCompare(b.id));
  return { schemaVersion: 1, profile: String(profile), operations: safeOperations };
}

export function serializeProcessingRecipe(recipe) {
  return `${JSON.stringify(createProcessingRecipe(recipe), null, 2)}\n`;
}

export function parseProcessingRecipe(serialized) {
  let recipe;
  try { recipe = JSON.parse(String(serialized)); } catch { throw new Error('Processing recipe is not valid JSON.'); }
  if (!recipe || recipe.schemaVersion !== 1 || typeof recipe.profile !== 'string' || !Array.isArray(recipe.operations)) throw new Error('Unsupported processing recipe schema.');
  if (recipe.operations.some((operation) => !operation || typeof operation !== 'object' || Array.isArray(operation) || operation.dependsOn != null && (!Array.isArray(operation.dependsOn) || operation.dependsOn.some((dependency) => typeof dependency !== 'string' || !dependency)) || operation.issueIds != null && (!Array.isArray(operation.issueIds) || operation.issueIds.some((issueId) => typeof issueId !== 'string' || !issueId)) || operation.options != null && (typeof operation.options !== 'object' || Array.isArray(operation.options)))) throw new Error('Processing recipe contains malformed operation collections.');
  const normalized = createProcessingRecipe(recipe);
  if (normalized.operations.length !== recipe.operations.length) throw new Error('Processing recipe contains unsafe or incomplete operation paths.');
  const ids = new Set(normalized.operations.map((operation) => operation.id));
  if (ids.size !== normalized.operations.length || normalized.operations.some((operation) => operation.dependsOn.some((dependency) => !ids.has(dependency)))) throw new Error('Processing recipe contains an unknown operation dependency.');
  orderProcessingRecipe(normalized);
  return normalized;
}

export function orderProcessingRecipe(recipe) {
  const operations = new Map((Array.isArray(recipe?.operations) ? recipe.operations : []).filter((operation) => operation && typeof operation === 'object').map((operation) => [operation.id, operation])), state = new Map(), ordered = [];
  const visit = (id) => {
    if (state.get(id) === 1) throw new Error('Processing recipe contains a dependency cycle.');
    if (state.get(id) === 2) return;
    const operation = operations.get(id); if (!operation) throw new Error(`Processing recipe dependency is missing: ${id}`);
    state.set(id, 1);
    for (const dependency of [...(operation.dependsOn || [])].sort()) visit(dependency);
    state.set(id, 2); ordered.push(operation);
  };
  for (const operation of [...operations.values()].sort((a, b) => (a.priority ?? Infinity) - (b.priority ?? Infinity) || a.id.localeCompare(b.id))) visit(operation.id);
  return ordered;
}

export function editProcessingRecipe(recipe, input = {}) {
  const { disabledIds = [], orderedIds = null } = input && typeof input === 'object' && !Array.isArray(input) ? input : {};
  const normalized = parseProcessingRecipe(serializeProcessingRecipe(recipe)), disabled = new Set((Array.isArray(disabledIds) ? disabledIds : []).map(String)), ids = new Set(normalized.operations.map((operation) => operation.id));
  for (const id of disabled) if (!ids.has(id)) throw new Error(`Cannot disable unknown recipe operation: ${id}`);
  for (const operation of normalized.operations) if (!disabled.has(operation.id)) for (const dependency of operation.dependsOn) if (disabled.has(dependency)) throw new Error(`Cannot disable ${dependency}; ${operation.id} depends on it.`);
  const enabled = normalized.operations.filter((operation) => !disabled.has(operation.id));
  if (orderedIds != null) {
    const requested = Array.isArray(orderedIds) ? orderedIds.map(String) : []; if (requested.length !== enabled.length || new Set(requested).size !== requested.length || requested.some((id) => !enabled.some((operation) => operation.id === id))) throw new Error('Edited recipe order must contain each enabled operation exactly once.');
    const positions = new Map(requested.map((id, index) => [id, index]));
    for (const operation of enabled) if (operation.dependsOn.some((dependency) => positions.get(dependency) >= positions.get(operation.id))) throw new Error(`Edited recipe order places a dependency after ${operation.id}.`);
    enabled.sort((left, right) => requested.indexOf(left.id) - requested.indexOf(right.id));
  }
  const priority = new Map(enabled.map((operation, index) => [operation.id, index]));
  return createProcessingRecipe({ ...normalized, operations: enabled.map((operation) => ({ ...operation, priority: priority.get(operation.id) })) });
}
