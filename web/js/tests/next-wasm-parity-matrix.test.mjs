// Keep the cross-product WASM parity matrix aligned with runtime inventories.
import assert from 'node:assert/strict';
import fs from 'node:fs';

const read = name => JSON.parse(fs.readFileSync(new URL(`./${name}`, import.meta.url)));
const legacy = read('lightusd-loader-api-inventory.json');
const next = new Set(read('renderstream-api-inventory.json'));
const nextModule = new Set(read('next-module-function-api-inventory.json'));
const nextConverter = new Set(read('next-usdz-converter-api-inventory.json'));
const nextAssets = new Set(read('next-asset-store-api-inventory.json'));
const nextComposition = new Set(read('next-composition-parity-map.json').map(row => row.legacyMethod));
const nextLayerDocument = new Set(read('next-layer-document-api-inventory.json'));
const nextFlatten = new Set(read('next-flatten-session-api-inventory.json'));
const nextLayerExport = new Set(read('next-layer-export-parity-map.json').map(row => row.legacyMethod));
const classification = new Map(read('lightusd-loader-api-classification.json')
  .map(row => [row.name, row.family]));
const matrix = read('next-wasm-parity-gaps.json');

assert.equal(matrix.length, legacy.length, 'matrix must cover every combined-loader method');
assert.deepEqual(matrix.map(row => row.legacyMethod), [...legacy].sort(),
  'matrix methods must be sorted and match the combined runtime inventory');
for (const row of matrix) {
  assert.equal(row.family, classification.get(row.legacyMethod),
    `classification mismatch for ${row.legacyMethod}`);
  assert.equal(row.nextNameMatch, next.has(row.legacyMethod),
    `next runtime name mismatch for ${row.legacyMethod}`);
  if (row.behaviorTest) {
    assert.ok(fs.existsSync(new URL(`./${row.behaviorTest}`, import.meta.url)),
      `${row.legacyMethod}: behavior test ${row.behaviorTest} is missing`);
  }
  if (row.parityStatus === 'behavior_verified') {
    // The named paired test pins each method against legacy and cross-checks
    // this row, including any documented edge delta.
    assert.ok(row.behaviorTest, `${row.legacyMethod}: verified rows must name their paired test`);
    assert.equal(row.knownDifference, undefined);
    if (row.nextSurface === 'NextFlattenSession' && !row.nextEquivalent) {
      // Workflow rows: every step must exist on the flatten session.
      assert.ok(row.workflow?.length, `${row.legacyMethod}: verified workflow row needs steps`);
      for (const step of row.workflow) assert.ok(nextFlatten.has(step), `${row.legacyMethod}: ${step}`);
      continue;
    }
    const inventory = row.nextSurface === 'NextAssetStore' ? nextAssets
      : row.nextSurface === 'RenderStream' ? next
      : row.nextSurface === 'NextUSDZConverterNative' ? nextConverter
      : row.nextSurface === 'NextLayerDocument' ? nextLayerDocument
      : row.nextSurface === 'NextFlattenSession' ? nextFlatten : null;
    assert.ok(inventory?.has(row.nextEquivalent),
      `verified method missing: ${row.nextSurface}.${row.nextEquivalent}`);
  } else if (row.parityStatus === 'product_decision') {
    assert.equal(row.family, 'mcp');
    assert.equal(row.nextNameMatch, false);
    assert.equal(row.nextSurface, undefined);
    assert.match(row.productDecision || '', /dedicated MCP server product/);
  } else if (row.parityStatus === 'test_only_excluded') {
    assert.equal(row.family, 'test_only');
    assert.ok(row.knownDifference, 'test-only exclusion must document its scope');
  } else if (row.parityStatus === 'known_behavior_gap') {
    assert.ok(row.knownDifference, 'known behavior gap must document the observed delta');
    if (row.nextSurface === 'NextModule') {
      assert.equal(row.family, 'loading_and_diagnostics');
      assert.ok(nextModule.has(row.nextEquivalent),
        `mapped NextModule function missing: ${row.nextEquivalent}`);
    } else if (row.nextSurface === 'RenderStream') {
      assert.ok(next.has(row.nextEquivalent),
        `mapped RenderStream method missing: ${row.nextEquivalent}`);
    } else if (row.nextSurface === 'NextAssetStore') {
      assert.ok(nextAssets.has(row.nextEquivalent),
        `mapped NextAssetStore method missing: ${row.nextEquivalent}`);
    } else if (row.nextSurface === 'NextUSDZConverterNative') {
      assert.ok(nextConverter.has(row.nextEquivalent),
        `mapped NextUSDZConverterNative method missing: ${row.nextEquivalent}`);
    } else {
      assert.fail(`unsupported known-gap surface: ${row.nextSurface}`);
    }
  } else if (row.nextSurface === 'NextAssetStore') {
    assert.equal(row.family, 'asset_resolution_and_cache');
    assert.ok(row.nextEquivalent && nextAssets.has(row.nextEquivalent),
      `mapped NextAssetStore method missing: ${row.nextEquivalent}`);
    assert.equal(row.parityStatus, 'mapped_behavior_review_required');
  } else if (row.nextSurface === 'NextUSDZConverterNative') {
    assert.equal(row.family === 'schema_and_image_utilities' ||
      row.family === 'layer_export_and_validation' ||
      row.family === 'loader_configuration', true);
    assert.equal(row.nextEquivalent, row.legacyMethod,
      'converter mappings must retain exact method names');
    assert.ok(nextConverter.has(row.nextEquivalent),
      `mapped NextUSDZConverterNative method missing: ${row.nextEquivalent}`);
    assert.equal(row.parityStatus, 'mapped_behavior_review_required');
  } else if (row.nextSurface === 'NextFlattenSession') {
    assert.ok(row.family === 'next_flatten' ||
      (row.family === 'composition_and_variants' && nextComposition.has(row.legacyMethod)) ||
      (row.family === 'layer_export_and_validation' &&
        (nextLayerExport.has(row.legacyMethod) || nextFlatten.has(row.nextEquivalent))));
    if (row.workflow) {
      assert.ok(row.workflow.length > 0);
      for (const method of row.workflow) assert.ok(nextFlatten.has(method));
    }
    assert.equal(row.parityStatus, 'workflow_covered_behavior_review_required');
  } else if (row.nextEquivalent && row.parityStatus === 'workflow_covered_behavior_review_required') {
    const target = row.nextSurface === 'RenderStream' ? next
      : row.nextSurface === 'NextLayerDocument' ? nextLayerDocument : null;
    assert.ok(target?.has(row.nextEquivalent),
      `mapped workflow method missing: ${row.nextSurface}.${row.nextEquivalent}`);
    if (row.workflow) {
      for (const step of row.workflow) {
        if (typeof step === 'string') assert.ok(target.has(step));
        else {
          const inventory = step.surface === 'NextAssetStore' ? nextAssets
            : step.surface === 'NextLayerDocument' ? nextLayerDocument : null;
          assert.ok(inventory?.has(step.method));
        }
      }
    }
  } else if (row.nextEquivalent) {
    assert.equal(row.nextSurface, 'RenderStream');
    assert.ok(next.has(row.nextEquivalent), `mapped RenderStream method missing: ${row.nextEquivalent}`);
    assert.equal(row.parityStatus, 'mapped_behavior_review_required');
  } else {
    assert.equal(row.parityStatus, row.nextNameMatch
      ? 'name_match_behavior_review_required'
      : 'missing_next_renderstream_method');
  }
}
// Every product method is reviewed by a paired legacy/next behavior test:
// it is either behavior_verified or a pinned known_behavior_gap.
const unreviewed = matrix.filter(row => !['product_decision', 'test_only_excluded']
  .includes(row.parityStatus) && !(row.behaviorTest &&
  ['behavior_verified', 'known_behavior_gap'].includes(row.parityStatus)));
assert.deepEqual(unreviewed.map(row => row.legacyMethod), [],
  'every product method must carry a paired behavior test and a reviewed status');
for (const row of matrix) {
  if (row.behaviorTest) {
    assert.ok(fs.existsSync(new URL(`./${row.behaviorTest}`, import.meta.url)), row.behaviorTest);
  }
}
const missing = matrix.filter(row => !row.nextNameMatch && !row.nextSurface &&
  row.parityStatus !== 'test_only_excluded' && row.parityStatus !== 'product_decision');
const testOnly = matrix.filter(row => row.parityStatus === 'test_only_excluded');
const productDecisions = matrix.filter(row => row.parityStatus === 'product_decision');
const knownBehaviorGaps = matrix.filter(row => row.parityStatus === 'known_behavior_gap');
const workflowMapped = matrix.filter(row => row.nextSurface === 'NextFlattenSession');
assert.equal(workflowMapped.length, 25,
  'flatten, composition, layer flatten, and asset-remap workflows must remain mapped');
const renderMapped = matrix.filter(row => row.nextSurface === 'RenderStream' && row.parityStatus === 'mapped_behavior_review_required');
const assetMapped = matrix.filter(row => row.nextSurface === 'NextAssetStore');
const behaviorVerified = matrix.filter(row => row.parityStatus === 'behavior_verified');
const converterMapped = matrix.filter(row => row.nextSurface === 'NextUSDZConverterNative');
const loadingMapped = matrix.filter(row => row.nextSurface === 'RenderStream' && row.parityStatus === 'workflow_covered_behavior_review_required').length + matrix.filter(row => row.nextSurface === 'NextLayerDocument' && row.family === 'loading_and_diagnostics').length;
const layerExportMapped = matrix.filter(row => row.workflow && row.family === 'layer_export_and_validation').length;
console.log(`ok - next-only WASM parity matrix covers ${matrix.length} combined methods; ${workflowMapped.length} map to NextFlattenSession workflows, ${loadingMapped} to loading workflows, ${layerExportMapped} to layer-export workflows, ${renderMapped.length} to other RenderStream methods, ${assetMapped.length} to NextAssetStore methods, ${converterMapped.length} to NextUSDZConverterNative methods, ${behaviorVerified.length} are behavior-verified against legacy by paired tests, ${knownBehaviorGaps.length} have a next implementation with a verified behavior difference, ${testOnly.length} are test-only exclusions, ${productDecisions.length} are explicit product-boundary decisions, and ${missing.length} product methods remain unmapped`);
