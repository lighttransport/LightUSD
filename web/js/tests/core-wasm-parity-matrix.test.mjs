// Keep the cross-product WASM parity matrix aligned with runtime inventories.
import assert from 'node:assert/strict';
import fs from 'node:fs';

const read = name => JSON.parse(fs.readFileSync(new URL(`./${name}`, import.meta.url)));
const legacy = read('lightusd-loader-api-inventory.json');
const core = new Set(read('renderstream-api-inventory.json'));
const coreModule = new Set(read('core-module-function-api-inventory.json'));
const coreConverter = new Set(read('core-usdz-converter-api-inventory.json'));
const coreAssets = new Set(read('core-asset-store-api-inventory.json'));
const coreComposition = new Set(read('core-composition-parity-map.json').map(row => row.legacyMethod));
const coreLayerDocument = new Set(read('core-layer-document-api-inventory.json'));
const flatten = new Set(read('core-flatten-session-api-inventory.json'));
const coreLayerExport = new Set(read('core-layer-export-parity-map.json').map(row => row.legacyMethod));
const classification = new Map(read('lightusd-loader-api-classification.json')
  .map(row => [row.name, row.family]));
const matrix = read('core-wasm-parity-gaps.json');

assert.equal(matrix.length, legacy.length, 'matrix must cover every combined-loader method');
assert.deepEqual(matrix.map(row => row.legacyMethod), [...legacy].sort(),
  'matrix methods must be sorted and match the combined runtime inventory');
for (const row of matrix) {
  assert.equal(row.family, classification.get(row.legacyMethod),
    `classification mismatch for ${row.legacyMethod}`);
  assert.equal(row.coreNameMatch, core.has(row.legacyMethod),
    `core runtime name mismatch for ${row.legacyMethod}`);
  if (row.behaviorTest) {
    assert.ok(fs.existsSync(new URL(`./${row.behaviorTest}`, import.meta.url)),
      `${row.legacyMethod}: behavior test ${row.behaviorTest} is missing`);
  }
  if (row.parityStatus === 'behavior_verified') {
    // The named paired test pins each method against legacy and cross-checks
    // this row, including any documented edge delta.
    assert.ok(row.behaviorTest, `${row.legacyMethod}: verified rows must name their paired test`);
    assert.equal(row.knownDifference, undefined);
    if (row.coreSurface === 'FlattenSession' && !row.coreEquivalent) {
      // Workflow rows: every step must exist on the flatten session.
      assert.ok(row.workflow?.length, `${row.legacyMethod}: verified workflow row needs steps`);
      for (const step of row.workflow) assert.ok(flatten.has(step), `${row.legacyMethod}: ${step}`);
      continue;
    }
    const inventory = row.coreSurface === 'AssetStore' ? coreAssets
      : row.coreSurface === 'RenderStream' ? core
      : row.coreSurface === 'USDZConverterNative' ? coreConverter
      : row.coreSurface === 'LayerDocument' ? coreLayerDocument
      : row.coreSurface === 'FlattenSession' ? flatten : null;
    assert.ok(inventory?.has(row.coreEquivalent),
      `verified method missing: ${row.coreSurface}.${row.coreEquivalent}`);
  } else if (row.parityStatus === 'product_decision') {
    assert.equal(row.family, 'mcp');
    assert.equal(row.coreNameMatch, false);
    assert.equal(row.coreSurface, undefined);
    assert.match(row.productDecision || '', /dedicated MCP server product/);
  } else if (row.parityStatus === 'test_only_excluded') {
    assert.equal(row.family, 'test_only');
    assert.ok(row.knownDifference, 'test-only exclusion must document its scope');
  } else if (row.parityStatus === 'known_behavior_gap') {
    assert.ok(row.knownDifference, 'known behavior gap must document the observed delta');
    if (row.coreSurface === 'ValidatorFeature') {
      assert.equal(row.coreEquivalent, 'validateFromBinary');
      assert.ok(coreModule.has('loadFeature'), 'validator must be available through loadFeature');
    } else if (row.coreSurface === 'CoreModule') {
      assert.equal(row.family, 'loading_and_diagnostics');
      assert.ok(coreModule.has(row.coreEquivalent),
        `mapped CoreModule function missing: ${row.coreEquivalent}`);
    } else if (row.coreSurface === 'RenderStream') {
      assert.ok(core.has(row.coreEquivalent),
        `mapped RenderStream method missing: ${row.coreEquivalent}`);
    } else if (row.coreSurface === 'AssetStore') {
      assert.ok(coreAssets.has(row.coreEquivalent),
        `mapped AssetStore method missing: ${row.coreEquivalent}`);
    } else if (row.coreSurface === 'USDZConverterNative') {
      assert.ok(coreConverter.has(row.coreEquivalent),
        `mapped USDZConverterNative method missing: ${row.coreEquivalent}`);
    } else {
      assert.fail(`unsupported known-gap surface: ${row.coreSurface}`);
    }
  } else if (row.coreSurface === 'AssetStore') {
    assert.equal(row.family, 'asset_resolution_and_cache');
    assert.ok(row.coreEquivalent && coreAssets.has(row.coreEquivalent),
      `mapped AssetStore method missing: ${row.coreEquivalent}`);
    assert.equal(row.parityStatus, 'mapped_behavior_review_required');
  } else if (row.coreSurface === 'USDZConverterNative') {
    assert.equal(row.family === 'schema_and_image_utilities' ||
      row.family === 'layer_export_and_validation' ||
      row.family === 'loader_configuration', true);
    assert.equal(row.coreEquivalent, row.legacyMethod,
      'converter mappings must retain exact method names');
    assert.ok(coreConverter.has(row.coreEquivalent),
      `mapped USDZConverterNative method missing: ${row.coreEquivalent}`);
    assert.equal(row.parityStatus, 'mapped_behavior_review_required');
  } else if (row.coreSurface === 'FlattenSession') {
    assert.ok(row.family === 'flatten' ||
      (row.family === 'composition_and_variants' && coreComposition.has(row.legacyMethod)) ||
      (row.family === 'layer_export_and_validation' &&
        (coreLayerExport.has(row.legacyMethod) || flatten.has(row.coreEquivalent))));
    if (row.workflow) {
      assert.ok(row.workflow.length > 0);
      for (const method of row.workflow) assert.ok(flatten.has(method));
    }
    assert.equal(row.parityStatus, 'workflow_covered_behavior_review_required');
  } else if (row.coreEquivalent && row.parityStatus === 'workflow_covered_behavior_review_required') {
    const target = row.coreSurface === 'RenderStream' ? core
      : row.coreSurface === 'LayerDocument' ? coreLayerDocument : null;
    assert.ok(target?.has(row.coreEquivalent),
      `mapped workflow method missing: ${row.coreSurface}.${row.coreEquivalent}`);
    if (row.workflow) {
      for (const step of row.workflow) {
        if (typeof step === 'string') assert.ok(target.has(step));
        else {
          const inventory = step.surface === 'AssetStore' ? coreAssets
            : step.surface === 'LayerDocument' ? coreLayerDocument : null;
          assert.ok(inventory?.has(step.method));
        }
      }
    }
  } else if (row.coreEquivalent) {
    assert.equal(row.coreSurface, 'RenderStream');
    assert.ok(core.has(row.coreEquivalent), `mapped RenderStream method missing: ${row.coreEquivalent}`);
    assert.equal(row.parityStatus, 'mapped_behavior_review_required');
  } else {
    assert.equal(row.parityStatus, row.coreNameMatch
      ? 'name_match_behavior_review_required'
      : 'missing_next_renderstream_method');
  }
}
// Every product method is reviewed by a paired legacy/core behavior test:
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
const missing = matrix.filter(row => !row.coreNameMatch && !row.coreSurface &&
  row.parityStatus !== 'test_only_excluded' && row.parityStatus !== 'product_decision');
const testOnly = matrix.filter(row => row.parityStatus === 'test_only_excluded');
const productDecisions = matrix.filter(row => row.parityStatus === 'product_decision');
const knownBehaviorGaps = matrix.filter(row => row.parityStatus === 'known_behavior_gap');
const workflowMapped = matrix.filter(row => row.coreSurface === 'FlattenSession');
assert.equal(workflowMapped.length, 25,
  'flatten, composition, layer flatten, and asset-remap workflows must remain mapped');
const renderMapped = matrix.filter(row => row.coreSurface === 'RenderStream' && row.parityStatus === 'mapped_behavior_review_required');
const assetMapped = matrix.filter(row => row.coreSurface === 'AssetStore');
const behaviorVerified = matrix.filter(row => row.parityStatus === 'behavior_verified');
const converterMapped = matrix.filter(row => row.coreSurface === 'USDZConverterNative');
const loadingMapped = matrix.filter(row => row.coreSurface === 'RenderStream' && row.parityStatus === 'workflow_covered_behavior_review_required').length + matrix.filter(row => row.coreSurface === 'LayerDocument' && row.family === 'loading_and_diagnostics').length;
const layerExportMapped = matrix.filter(row => row.workflow && row.family === 'layer_export_and_validation').length;
console.log(`ok - core WASM parity matrix covers ${matrix.length} combined methods; ${workflowMapped.length} map to FlattenSession workflows, ${loadingMapped} to loading workflows, ${layerExportMapped} to layer-export workflows, ${renderMapped.length} to other RenderStream methods, ${assetMapped.length} to AssetStore methods, ${converterMapped.length} to USDZConverterNative methods, ${behaviorVerified.length} are behavior-verified against legacy by paired tests, ${knownBehaviorGaps.length} have a core implementation with a verified behavior difference, ${testOnly.length} are test-only exclusions, ${productDecisions.length} are explicit product-boundary decisions, and ${missing.length} product methods remain unmapped`);
