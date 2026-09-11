import { validIdentifier, validPrimPath, LuciaError } from './utils.js';
import { normalizePBRFormat } from './material-translation.js';

const BAKE_MAX_RESOLUTION = 4096;
const BAKE_MAX_DILATION = 32;
const BAKE_MAX_SAMPLES = 16;
const BAKE_MAX_RADIUS = 1e6;
const TEXTURE_MAX_DIMENSION = 8192;

function invalidBakeNumber(value, { integer = false, minimum = 0, maximum } = {}) {
  return typeof value !== 'number' || !Number.isFinite(value) || integer && !Number.isSafeInteger(value) || value < minimum || value > maximum;
}

export const LUCIA_TOOLS = [
  ['scene.inspect', false], ['scene.create_prim', false], ['scene.rename_prim', false], ['scene.rename_component_parts', true], ['scene.create_instance', true], ['scene.generate_convex_hull', true], ['scene.generate_triangle_collider', true], ['scene.split_components', true], ['scene.correct_components', true], ['scene.propose_component_names', false], ['scene.inspect_component_segmentation', false], ['scene.inspect_semantic_suggestions', false],
  ['scene.delete_prim', true], ['scene.set_transform', false], ['scene.set_attribute', false], ['scene.inspect_material_graph', false], ['scene.inspect_material_parameterization', false], ['scene.parameterize_materials', true], ['scene.optimize_material_graph', true],
  ['scene.rename_texture', false], ['scene.inspect_material_merges', false], ['scene.merge_material_bindings', true], ['scene.rewrite_collection_material_bindings', true], ['scene.repair_inherited_material_bindings', true], ['scene.merge_material_definitions', true], ['scene.translate_material', true], ['scene.transfer_skin_weights', true], ['scene.generate_template', false], ['scene.retopo', true], ['scene.generate_lods', true], ['scene.unwrap_uv', true], ['scene.extract_reference', true], ['scene.flatten_reference', true], ['scene.health_report', false],
  ['scene.bake_shading', true], ['scene.bake_projection', true], ['scene.transfer_uvs', true], ['scene.pack_channels', true], ['scene.resize_texture', true], ['scene.cleanup_mesh', true], ['scene.merge_cracks', true], ['scene.recompute_normals', true], ['scene.recompute_tangents', true], ['scene.create_collision_group', true], ['scene.validate', false], ['scene.usd_doctor', false], ['scene.repair_usd_metadata', true], ['scene.localize_dependencies', true], ['scene.export', true],
  ['scene.undo', false], ['scene.redo', false],
].map(([name, confirm]) => ({ name, confirm }));

export class LuciaLLMClient {
  constructor(endpoint = 'http://127.0.0.1:8788/api/lucia/chat') { this.endpoint = endpoint; }
  async complete(messages, sceneSummary) {
    const response = await fetch(this.endpoint, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({
        messages,
        sceneSummary,
        tools: LUCIA_TOOLS.map(({ name }) => ({
          type: 'function',
          function: {
            name: name.replace('.', '__'),
            parameters: { type: 'object', additionalProperties: true },
          },
        })),
      }),
    });
    if (!response.ok) throw new LuciaError('LUCIA_LLM_CONNECTION', `Assistant proxy returned HTTP ${response.status}.`);
    return response.json();
  }
}

export class LuciaAssistant extends EventTarget {
  constructor({ executeTool, getSelection, getSceneSummary, confirm, recordDecision }) {
    super(); this.executeTool = executeTool; this.getSelection = getSelection; this.getSceneSummary = getSceneSummary; this.confirm = confirm; this.recordDecision = recordDecision;
    this.mode = 'mock'; this.client = new LuciaLLMClient(); this.messages = [{ role: 'assistant', text: 'Lucia is ready. Try “Create a product turntable with a red metallic sphere.”' }];
  }
  emit() { this.dispatchEvent(new CustomEvent('change')); }
  add(role, text, extra = {}) { this.messages.push({ role, text, ...extra }); this.emit(); }
  validate(call) {
    const definition = LUCIA_TOOLS.find((tool) => tool.name === call.name);
    if (!definition) throw new LuciaError('LUCIA_TOOL_UNKNOWN', `Unknown Lucia tool: ${call.name}`);
    const path = call.arguments?.path;
    if (path && !validPrimPath(path)) throw new LuciaError('LUCIA_TOOL_PATH', `Invalid USD path: ${path}`);
    for (const field of ['targetPath', 'sourcePath', 'transformSourcePath']) if (call.arguments?.[field] && !validPrimPath(call.arguments[field])) throw new LuciaError('LUCIA_TOOL_PATH', `Invalid USD path: ${call.arguments[field]}`);
    if (call.name === 'scene.create_collision_group') for (const field of ['colliderPaths', 'filteredGroupPaths']) if (call.arguments?.[field] != null && (!Array.isArray(call.arguments[field]) || call.arguments[field].some((value) => !validPrimPath(value)))) throw new LuciaError('LUCIA_TOOL_COLLISION_GROUP', `${field} must contain valid USD prim paths.`);
    if (call.name === 'scene.create_collision_group' && call.arguments?.mergeGroup != null && !validIdentifier(call.arguments.mergeGroup)) throw new LuciaError('LUCIA_TOOL_COLLISION_GROUP', 'mergeGroup must be a valid USD identifier.');
    if (call.name === 'scene.create_collision_group' && call.arguments?.invertFilteredGroups != null && typeof call.arguments.invertFilteredGroups !== 'boolean') throw new LuciaError('LUCIA_TOOL_COLLISION_GROUP', 'invertFilteredGroups must be boolean.');
    if (call.name === 'scene.correct_components') {
      const mergeGroups = call.arguments?.mergeGroups, discard = call.arguments?.discard;
      if (!Array.isArray(mergeGroups) || mergeGroups.some((group) => !Array.isArray(group) || group.length < 2 || group.some((id) => !Number.isSafeInteger(id) || id < 0))) throw new LuciaError('LUCIA_TOOL_COMPONENT_CORRECTION', 'mergeGroups must contain groups of at least two non-negative integer component IDs.');
      if (discard != null && (!Array.isArray(discard) || discard.some((id) => !Number.isSafeInteger(id) || id < 0))) throw new LuciaError('LUCIA_TOOL_COMPONENT_CORRECTION', 'discard must contain non-negative integer component IDs.');
      if (call.arguments?.minFaces != null && (!Number.isSafeInteger(call.arguments.minFaces) || call.arguments.minFaces < 1)) throw new LuciaError('LUCIA_TOOL_COMPONENT_CORRECTION', 'minFaces must be a positive integer.');
    }
    if (['scene.split_components', 'scene.correct_components'].includes(call.name) && call.arguments?.asChildren != null && typeof call.arguments.asChildren !== 'boolean') throw new LuciaError('LUCIA_TOOL_COMPONENT_HIERARCHY', 'asChildren must be boolean.');
    if (['scene.split_components', 'scene.correct_components', 'scene.propose_component_names'].includes(call.name) && call.arguments?.splitAngle != null && (!Number.isFinite(call.arguments.splitAngle) || call.arguments.splitAngle < 0 || call.arguments.splitAngle > 180)) throw new LuciaError('LUCIA_TOOL_COMPONENT_ANGLE', 'splitAngle must be between 0 and 180 degrees.');
    if (['scene.split_components', 'scene.correct_components', 'scene.propose_component_names'].includes(call.name) && call.arguments?.splitConcavity != null && typeof call.arguments.splitConcavity !== 'boolean') throw new LuciaError('LUCIA_TOOL_COMPONENT_CONCAVITY', 'splitConcavity must be boolean.');
    if (call.name === 'scene.propose_component_names' && call.arguments?.minFaces != null && (!Number.isSafeInteger(call.arguments.minFaces) || call.arguments.minFaces < 1)) throw new LuciaError('LUCIA_TOOL_COMPONENT_NAMES', 'minFaces must be a positive integer.');
    if (call.name === 'scene.inspect_semantic_suggestions' && call.arguments?.approvedInference != null && typeof call.arguments.approvedInference !== 'boolean') throw new LuciaError('LUCIA_TOOL_SEMANTIC_INFERENCE', 'approvedInference must be boolean.');
    if (call.name === 'scene.inspect_semantic_suggestions' && call.arguments?.inference != null && (!Array.isArray(call.arguments.inference) || call.arguments.inference.length > 256)) throw new LuciaError('LUCIA_TOOL_SEMANTIC_INFERENCE', 'inference must be an array containing at most 256 suggestions.');
    if (call.name === 'scene.rename_component_parts') {
      const names = call.arguments?.names;
      if (!names || Array.isArray(names) || typeof names !== 'object' || !Object.keys(names).length) throw new LuciaError('LUCIA_TOOL_COMPONENT_NAMES', 'names must be a non-empty component-ID to USD-name map.');
      const values = Object.entries(names);
      if (values.some(([id, name]) => !/^\d+$/.test(id) || !validIdentifier(name)) || new Set(values.map(([id]) => Number(id))).size !== values.length || new Set(values.map(([, name]) => name)).size !== values.length) throw new LuciaError('LUCIA_TOOL_COMPONENT_NAMES', 'Component IDs must be non-negative integers with unique valid USD names.');
      if (call.arguments?.asChildren != null && typeof call.arguments.asChildren !== 'boolean') throw new LuciaError('LUCIA_TOOL_COMPONENT_HIERARCHY', 'asChildren must be boolean.');
    }
    if (call.name === 'scene.create_instance' && !validIdentifier(call.arguments?.name)) throw new LuciaError('LUCIA_INSTANCE_NAME', 'Instance name must be a valid USD identifier.');
    const assetPath = call.arguments?.assetPath;
    if (assetPath && (assetPath.startsWith('/') || assetPath.includes('..') || assetPath.includes('\\'))) throw new LuciaError('LUCIA_TOOL_ASSET_PATH', `Invalid asset path: ${assetPath}`);
    if (call.name === 'scene.rename_texture' && call.arguments?.newPath != null && (typeof call.arguments.newPath !== 'string' || !/^[A-Za-z0-9_./-]+$/.test(call.arguments.newPath) || call.arguments.newPath.startsWith('/') || call.arguments.newPath.includes('..') || call.arguments.newPath.includes('\\') || call.arguments.newPath.includes('//'))) throw new LuciaError('LUCIA_TOOL_ASSET_PATH', `Invalid texture path: ${call.arguments.newPath}`);
    if (call.name === 'scene.merge_material_bindings' || call.name === 'scene.merge_material_definitions' || call.name === 'scene.rewrite_collection_material_bindings') {
      const mapping = call.arguments?.mapping;
      if (call.name === 'scene.merge_material_definitions' && mapping == null) return definition;
      if (!mapping || Array.isArray(mapping) || typeof mapping !== 'object' || Object.entries(mapping).some(([from, to]) => !validPrimPath(from) || !validPrimPath(to))) throw new LuciaError('LUCIA_TOOL_MATERIAL_MAPPING', 'Material mappings must contain absolute USD prim paths.');
    }
    if (call.name === 'scene.repair_inherited_material_bindings') {
      const repairs = call.arguments?.repairs;
      if (!Array.isArray(repairs) || !repairs.length || repairs.some((repair) => !repair || !validPrimPath(repair.path) || !validPrimPath(repair.materialPath)) || new Set(repairs.map((repair) => repair.path)).size !== repairs.length) throw new LuciaError('LUCIA_TOOL_INHERITED_BINDING', 'Inherited material repairs require unique absolute prim and material paths.');
    }
    if (call.name === 'scene.translate_material') {
      try { normalizePBRFormat(call.arguments?.from); normalizePBRFormat(call.arguments?.to); } catch { throw new LuciaError('LUCIA_TOOL_MATERIAL_FORMAT', 'Material translation requires supported from and to formats.'); }
    }
    if (call.name === 'scene.inspect_material_graph' && call.arguments?.output != null && (typeof call.arguments.output !== 'string' || !/^[A-Za-z_][\w:]*$/.test(call.arguments.output))) throw new LuciaError('LUCIA_TOOL_MATERIAL_OUTPUT', 'Material graph output must be a valid channel name.');
    if (call.name === 'scene.inspect_material_parameterization' && call.arguments?.mode != null && !['auto', 'primvar', 'variant'].includes(call.arguments.mode)) throw new LuciaError('LUCIA_TOOL_MATERIAL_PARAMETERIZATION', 'Material parameterization mode must be auto, primvar, or variant.');
    if (call.name === 'scene.parameterize_materials' && call.arguments?.mode != null && !['auto', 'primvar', 'variant'].includes(call.arguments.mode)) throw new LuciaError('LUCIA_TOOL_MATERIAL_PARAMETERIZATION', 'Material authoring mode must be auto, primvar, or variant.');
    if (call.name === 'scene.localize_dependencies') {
      const mapping = call.arguments?.mapping;
      if (!mapping || Array.isArray(mapping) || typeof mapping !== 'object' || Object.entries(mapping).some(([from, to]) => !String(from) || !String(to))) throw new LuciaError('LUCIA_TOOL_DEPENDENCY_MAPPING', 'Dependency localization requires a non-empty old-to-new path map.');
    }
    if (call.name === 'scene.extract_reference' && call.arguments?.assetPath != null && (typeof call.arguments.assetPath !== 'string' || !/^(?!\/)(?!.*(?:^|\/)\.\.(?:\/|$))[A-Za-z0-9_./-]+\.usda$/i.test(call.arguments.assetPath) || call.arguments.assetPath.includes('\\'))) throw new LuciaError('LUCIA_TOOL_ASSET_PATH', 'Extracted layer path must be a safe package-relative .usda path.');
    const name = call.arguments?.name || call.arguments?.newName;
    if (name && call.name !== 'scene.pack_channels' && !validIdentifier(name)) throw new LuciaError('LUCIA_TOOL_IDENTIFIER', `Invalid USD identifier: ${name}`);
    if (call.arguments?.normalSpace != null && !['object', 'tangent'].includes(call.arguments.normalSpace)) throw new LuciaError('LUCIA_TOOL_NORMAL_SPACE', 'normalSpace must be object or tangent.');
    if (call.name === 'scene.recompute_normals' && call.arguments?.interpolation != null && !['vertex', 'faceVarying'].includes(call.arguments.interpolation)) throw new LuciaError('LUCIA_TOOL_NORMAL_INTERPOLATION', 'Normal interpolation must be vertex or faceVarying.');
    if (['scene.recompute_normals', 'scene.retopo'].includes(call.name) && call.arguments?.sharpChains != null && (call.arguments.sharpEdges != null || !Array.isArray(call.arguments.sharpChains) || call.arguments.sharpChains.some((chain) => !Array.isArray(chain) || chain.length < 2 || chain.some((index, offset) => !Number.isSafeInteger(index) || index < 0 || offset > 0 && index === chain[offset - 1])) || call.arguments.sharpChainSharpness != null && (!Array.isArray(call.arguments.sharpChainSharpness) || call.arguments.sharpChainSharpness.length !== call.arguments.sharpChains.length || call.arguments.sharpChainSharpness.some((value) => !Number.isFinite(value) || value <= 0)))) throw new LuciaError('LUCIA_TOOL_SHARP_CHAINS', 'sharpChains must contain distinct-in-use non-negative integer chains with aligned positive sharpness values.');
    if (call.name === 'scene.bake_shading' && call.arguments?.channel != null && !['baseColor', 'normal', 'roughness', 'metallic', 'opacity', 'emissive', 'objectID', 'materialID', 'occlusion'].includes(call.arguments.channel)) throw new LuciaError('LUCIA_TOOL_BAKE_CHANNEL', 'Unsupported shading-bake channel.');
    if (call.name === 'scene.bake_projection' && call.arguments?.channel != null && !['baseColor', 'normal', 'roughness', 'metallic', 'opacity', 'emissive'].includes(call.arguments.channel)) throw new LuciaError('LUCIA_TOOL_PROJECTION_CHANNEL', 'Unsupported projected-bake channel.');
    if (call.name === 'scene.bake_shading' && call.arguments?.normalY != null && !['opengl', 'directx'].includes(call.arguments.normalY)) throw new LuciaError('LUCIA_TOOL_NORMAL_Y', 'normalY must be opengl or directx.');
    if (call.name === 'scene.bake_shading') {
      const args = call.arguments || {};
      if (args.resolution != null && invalidBakeNumber(args.resolution, { integer: true, minimum: 64, maximum: BAKE_MAX_RESOLUTION })) throw new LuciaError('LUCIA_TOOL_BAKE_RESOLUTION', 'Bake resolution must be a safe integer from 64 through 4096.');
      if (args.maxResolution != null && invalidBakeNumber(args.maxResolution, { integer: true, minimum: 64, maximum: BAKE_MAX_RESOLUTION })) throw new LuciaError('LUCIA_TOOL_BAKE_MAX_RESOLUTION', 'Bake maximum dimension must be a safe integer from 64 through 4096.');
      if (args.dilation != null && invalidBakeNumber(args.dilation, { integer: true, minimum: 0, maximum: BAKE_MAX_DILATION })) throw new LuciaError('LUCIA_TOOL_BAKE_DILATION', 'Bake dilation must be an integer from 0 through 32.');
      if (args.samples != null && invalidBakeNumber(args.samples, { integer: true, minimum: 1, maximum: BAKE_MAX_SAMPLES })) throw new LuciaError('LUCIA_TOOL_BAKE_SAMPLES', 'Bake samples must be an integer from 1 through 16.');
      if (args.radius != null && invalidBakeNumber(args.radius, { minimum: 1e-5, maximum: BAKE_MAX_RADIUS })) throw new LuciaError('LUCIA_TOOL_BAKE_RADIUS', 'Bake ray radius must be finite and between 0.00001 and 1000000.');
    }
    if (call.name === 'scene.bake_projection') {
      const args = call.arguments || {};
      if (args.resolution != null && invalidBakeNumber(args.resolution, { integer: true, minimum: 64, maximum: BAKE_MAX_RESOLUTION })) throw new LuciaError('LUCIA_TOOL_PROJECTION_RESOLUTION', 'Projected-bake resolution must be a safe integer from 64 through 4096.');
      if (args.maxResolution != null && invalidBakeNumber(args.maxResolution, { integer: true, minimum: 64, maximum: BAKE_MAX_RESOLUTION })) throw new LuciaError('LUCIA_TOOL_PROJECTION_MAX_RESOLUTION', 'Projected-bake maximum dimension must be a safe integer from 64 through 4096.');
      if (args.dilation != null && invalidBakeNumber(args.dilation, { integer: true, minimum: 0, maximum: BAKE_MAX_DILATION })) throw new LuciaError('LUCIA_TOOL_PROJECTION_DILATION', 'Projected-bake dilation must be an integer from 0 through 32.');
      if (args.rayDistance != null && invalidBakeNumber(args.rayDistance, { minimum: 1e-6, maximum: BAKE_MAX_RADIUS })) throw new LuciaError('LUCIA_TOOL_PROJECTION_DISTANCE', 'Projected-bake ray distance must be finite and between 0.000001 and 1000000.');
      if (args.cageOffset != null && invalidBakeNumber(args.cageOffset, { minimum: 0, maximum: BAKE_MAX_RADIUS })) throw new LuciaError('LUCIA_TOOL_PROJECTION_CAGE', 'Projected-bake cage offset must be finite and between 0 and 1000000.');
    }
    if (call.name === 'scene.resize_texture' && call.arguments?.maxDimension != null && invalidBakeNumber(call.arguments.maxDimension, { integer: true, minimum: 1, maximum: TEXTURE_MAX_DIMENSION })) throw new LuciaError('LUCIA_TOOL_TEXTURE_DIMENSION', 'Texture maximum dimension must be a safe integer from 1 through 8192.');
    if (call.name === 'scene.pack_channels') {
      const args = call.arguments || {}, channels = args.channels;
      if (args.name != null && (typeof args.name !== 'string' || !/^(?!\/)(?!.*(?:^|\/)\.\.(?:\/|$))[A-Za-z0-9_./-]+\.png$/i.test(args.name) || args.name.includes('\\') || args.name.includes('//'))) throw new LuciaError('LUCIA_TOOL_PACK_PATH', 'Packed texture output must be a safe package-relative .png path.');
      if (args.outputChannels != null && invalidBakeNumber(args.outputChannels, { integer: true, minimum: 1, maximum: 4 })) throw new LuciaError('LUCIA_TOOL_PACK_CHANNELS', 'Packed output channels must be a safe integer from 1 through 4.');
      if (args.colorSpace != null && !['linear', 'srgb', 'raw'].includes(args.colorSpace)) throw new LuciaError('LUCIA_TOOL_PACK_COLOR_SPACE', 'Packed texture color space must be linear, srgb, or raw.');
      if (channels != null && (!channels || Array.isArray(channels) || typeof channels !== 'object' || Object.keys(channels).some((slot) => !['r', 'g', 'b', 'a'].includes(slot)) || Object.values(channels).some((value) => { const descriptor = typeof value === 'string' ? null : value; return value != null && (!descriptor || typeof descriptor.path !== 'string' || descriptor.channel != null && invalidBakeNumber(descriptor.channel, { integer: true, minimum: 0, maximum: 3 })); }))) throw new LuciaError('LUCIA_TOOL_PACK_CHANNELS', 'Packed channels must use r/g/b/a slots with safe integer source channels from 0 through 3.');
      if (channels != null && Object.entries(channels).some(([slot, value]) => value != null && ['r', 'g', 'b', 'a'].indexOf(slot) >= (args.outputChannels ?? 4))) throw new LuciaError('LUCIA_TOOL_PACK_CHANNELS', 'Packed channels cannot specify sources beyond the requested output channel count.');
    }
    if (call.name === 'scene.merge_cracks' && call.arguments?.tolerance != null && (!Number.isFinite(call.arguments.tolerance) || call.arguments.tolerance <= 0)) throw new LuciaError('LUCIA_TOOL_TOLERANCE', 'Crack-merge tolerance must be a positive finite number.');
    if (['scene.unwrap_uv', 'scene.project_uv', 'scene.transfer_uvs'].includes(call.name) && call.arguments?.uvSet != null && !['default', 'lightmap'].includes(call.arguments.uvSet)) throw new LuciaError('LUCIA_TOOL_UV_SET', 'uvSet must be default or lightmap.');
    if (call.arguments?.sharpEdges != null && (!Array.isArray(call.arguments.sharpEdges) || call.arguments.sharpEdges.some((edge) => !Array.isArray(edge) || edge.length !== 2 || edge[0] === edge[1] || edge.some((index) => !Number.isSafeInteger(index) || index < 0)))) throw new LuciaError('LUCIA_TOOL_SHARP_EDGES', 'sharpEdges must contain distinct non-negative integer vertex pairs.');
    if (call.name === 'scene.retopo' && call.arguments?.lockedVertices != null && (!Array.isArray(call.arguments.lockedVertices) || call.arguments.lockedVertices.some((value) => value !== 0 && value !== 1))) throw new LuciaError('LUCIA_TOOL_RETOPO_LOCKS', 'lockedVertices must be a zero/one vertex lock mask.');
    if (['scene.retopo', 'scene.generate_lods'].includes(call.name) && call.arguments?.lockBorder != null && typeof call.arguments.lockBorder !== 'boolean') throw new LuciaError('LUCIA_TOOL_RETOPO_BORDER', 'lockBorder must be boolean.');
    if (['scene.retopo', 'scene.generate_lods'].includes(call.name) && call.arguments?.lockUVSeams != null && typeof call.arguments.lockUVSeams !== 'boolean') throw new LuciaError('LUCIA_TOOL_RETOPO_SEAMS', 'lockUVSeams must be boolean.');
    return definition;
  }
  mock(prompt) {
    const lower = prompt.toLowerCase(); const selected = this.getSelection();
    if (lower.includes('product turntable')) return { name: 'scene.generate_template', arguments: { template: 'product' } };
    if (/\bpack(?:\s+texture)?\s+channels?\b/i.test(prompt)) { const output = prompt.match(/\b(?:into|as|to)\s+([A-Za-z0-9_./-]+\.png)\b/i)?.[1] || 'textures/packed.png', channelCount = prompt.match(/\b([1-4])\s+(?:output\s+)?channels?\b/i)?.[1], channels = {}; for (const match of prompt.matchAll(/\b([RGBA])(?:\s+channel\s+(\d+))?\s+(?:from\s+)?([A-Za-z0-9_./-]+\.(?:png|jpe?g|webp|exr|hdr))\b/gi)) channels[match[1].toLowerCase()] = { path: match[3], ...(match[2] == null ? {} : { channel: Number(match[2]) }) }; if (Object.keys(channels).length) return { name: 'scene.pack_channels', arguments: { name: output, ...(channelCount == null ? {} : { outputChannels: Number(channelCount) }), channels } }; }
    if (/(?:remove|delete|clean).*(?:unreachable|dead).*(?:material|shader).*(?:graph|node)|(?:material|shader).*(?:graph|node).*(?:remove|delete|clean).*(?:unreachable|dead)/i.test(prompt)) return { name: 'scene.optimize_material_graph', arguments: { path: selected } };
    if (/(?:inspect|evaluate|analy[sz]e|preview).*(?:material|shader).*(?:graph|network)|(?:material|shader).*(?:graph|network).*(?:inspect|evaluate|analy[sz]e|preview)/i.test(prompt)) { const output = prompt.match(/(?:output|channel)\s+([A-Za-z_][\w:]*)/i); return { name: 'scene.inspect_material_graph', arguments: { path: selected, ...(output ? { output: output[1] } : {}) } }; }
    if (/(?:apply|author|write|convert).*(?:parameteri[sz]e|material.*(?:primvar|variant)|(?:primvar|variant).*material)/i.test(prompt)) return { name: 'scene.parameterize_materials', arguments: { path: selected, mode: /variant/i.test(prompt) ? 'variant' : 'primvar' } };
    if (/(?:parameteri[sz]e|material.*(?:primvar|variant)|primvar.*material|variant.*material)/i.test(prompt)) return { name: 'scene.inspect_material_parameterization', arguments: { path: selected } };
    if (/(?:inspect|analy[sz]e|suggest|find).*(?:semantic|role|relationship)|(?:semantic|role|relationship).*(?:inspect|analy[sz]e|suggest|find)/i.test(prompt)) return { name: 'scene.inspect_semantic_suggestions', arguments: { path: selected } };
    if (/(?:inspect|analy[sz]e|preview|show).*(?:component|part).*(?:segmentation|boundary|curvature|concavity|planarity)|(?:segmentation|boundary|curvature|concavity|planarity).*(?:component|part)/i.test(prompt)) return { name: 'scene.inspect_component_segmentation', arguments: { path: selected, minFaces: 1 } };
    if (/(?:inspect|analy[sz]e|preview|find|show|list).*(?:duplicate|equivalent).*(?:material|shader)|(?:material|shader).*(?:duplicate|equivalent).*(?:inspect|analy[sz]e|preview|find|show|list)/i.test(prompt)) return { name: 'scene.inspect_material_merges', arguments: {} };
    const materialTranslation = prompt.match(/translate(?: the)? material(?: shader)?\s+[`'\"]?([^`'\"\s]+)[`'\"]?\s+from\s+([\w-]+(?:\s+standard\s+surface)?)\s+to\s+([\w-]+(?:\s+standard\s+surface)?)/i);
    if (materialTranslation) { const normalizeFormat = (value) => { const format = value.toLowerCase().replaceAll('_', '-').replaceAll(' ', '-'); return format === 'materialx' || format === 'materialx-standard-surface' ? 'materialx-standard-surface' : format === 'usdpreviewsurface' || format === 'usd-preview-surface' ? 'usd-preview-surface' : format === 'metallic-roughness' || format === 'pbr' ? 'metallic-roughness' : format; }; return { name: 'scene.translate_material', arguments: { path: materialTranslation[1].startsWith('/') ? materialTranslation[1] : selected, from: normalizeFormat(materialTranslation[2]), to: normalizeFormat(materialTranslation[3]) } }; }
    if (/(?:merge|consolidate).*(?:duplicate|equivalent).*(?:material|shader)/i.test(prompt)) return { name: 'scene.merge_material_definitions', arguments: {} };
    const inheritedBinding = prompt.match(/(?:repair|author|override).*inherited(?:\s+or\s+specialized)?(?:\s+material)?\s+binding.*?([/][A-Za-z0-9_/-]+)\s+(?:to|as)\s+([/][A-Za-z0-9_/-]+)/i);
    if (inheritedBinding) return { name: 'scene.repair_inherited_material_bindings', arguments: { repairs: [{ path: inheritedBinding[1], materialPath: inheritedBinding[2] }] } };
    const collectionBinding = prompt.match(/(?:repair|rewrite|redirect).*collection(?:\s+material)?\s+binding.*?([/][A-Za-z0-9_/-]+)\s+(?:to|as)\s+([/][A-Za-z0-9_/-]+)/i);
    if (collectionBinding) return { name: 'scene.rewrite_collection_material_bindings', arguments: { mapping: { [collectionBinding[1]]: collectionBinding[2] } } };
    if (/(?:extract|externalize).*(?:reference|layer)/i.test(prompt)) return { name: 'scene.extract_reference', arguments: { path: selected } };
    if (/(?:flatten|inline).*(?:reference|layer)/i.test(prompt)) return { name: 'scene.flatten_reference', arguments: { path: selected } };
    const localization = prompt.match(/locali[sz]e(?:\s+(?:the\s+)?(?:dependency|reference))?\s+[`'@"]?([^`'@"\s]+)[`'@"]?\s+(?:to|as)\s+[`'@"]?([^`'@"\s]+)[`'@"]?/i);
    if (localization) return { name: 'scene.localize_dependencies', arguments: { mapping: { [localization[1]]: localization[2] } } };
    const instance = prompt.match(/(?:create|make|author)\s+(?:an?\s+)?(?:USD\s+)?instance(?:\s+(?:of|from)\s+[`'"]?([^`'"\s]+)[`'"]?)?(?:\s+(?:named|called|as)\s+[`'"]?([A-Za-z_][A-Za-z0-9_]*)[`'"]?)?/i);
    if (instance) { const sourcePath = instance[1]?.startsWith('/') ? instance[1] : selected, sourceName = sourcePath.split('/').at(-1) || 'Mesh', transformSourcePath = prompt.match(/(?:placement|transform)\s+from\s+([/][A-Za-z0-9_/-]*)/i)?.[1], explicitName = prompt.match(/(?:named|called|as)\s+[`'"]?([A-Za-z_][A-Za-z0-9_]*)/i)?.[1]; return { name: 'scene.create_instance', arguments: { sourcePath, name: explicitName || instance[2] || `${sourceName}_Instance`, ...(transformSourcePath ? { transformSourcePath } : {}) } }; }
    const rename = prompt.match(/rename every [`'"]?([^`'"\s]+)[`'"]? texture to [`'"]?([^`'"\s]+)[`'"]?/i);
    if (rename) return { name: 'scene.rename_texture', arguments: { oldPath: rename[1], newPath: rename[2] } };
    if (lower.includes('lod') || lower.includes('level of detail')) { const lockUVSeams = !/(?:without|unlock|allow|ignore)\s+(?:the\s+)?uv\s+seams?/i.test(prompt), lockBorder = !/(?:without|unlock|allow|ignore)\s+(?:the\s+)?(?:open\s+)?(?:borders?|boundaries?)/i.test(prompt); return { name: 'scene.generate_lods', arguments: { path: selected, ratios: [.5, .25, .125], targetError: 0, lockUVSeams, lockBorder } }; }
    if (lower.includes('split') && (lower.includes('component') || lower.includes('part'))) { const angle = prompt.match(/(?:angle|crease)\s+(?:of\s+)?([\d.]+)/i), splitConcavity = /concav(?:e|ity)/i.test(prompt); return { name: 'scene.split_components', arguments: { path: selected, minFaces: 1, ...(angle ? { splitAngle: Number(angle[1]) } : {}), ...(splitConcavity ? { splitConcavity: true } : {}) } }; }
    if (/(?:rename|call)\s+(?:component|part)/i.test(prompt)) { const names = {}; for (const match of prompt.matchAll(/(?:component|part)\s*(\d+)\s+(?:to|as|named)\s+([A-Za-z_][A-Za-z0-9_]*)/gi)) names[match[1]] = match[2]; if (Object.keys(names).length) return { name: 'scene.rename_component_parts', arguments: { path: selected, names } }; }
    if (/(?:name|label).*(?:component|part)|(?:component|part).*(?:name|label)/i.test(prompt)) return { name: 'scene.propose_component_names', arguments: { path: selected, minFaces: 1 } };
    const componentCorrection = prompt.match(/(?:merge|join) component(?:s)?\s+(.+?)(?:\s+and\s+discard\s+(.+))?$/i);
    if (componentCorrection) { const groups = componentCorrection[1].split(';').map((group) => group.trim().split(/\s*(?:\+|,|and)\s*/).filter(Boolean).map(Number)); const discard = componentCorrection[2] ? componentCorrection[2].split(/\s*(?:,|and)\s*/).filter(Boolean).map(Number) : []; return { name: 'scene.correct_components', arguments: { path: selected, mergeGroups: groups, discard } }; }
    const crackMerge = /(?:merge|weld).*(?:crack|seam)|(?:crack|seam).*(?:merge|weld)/i.test(prompt);
    if (crackMerge) { const tolerance = prompt.match(/(?:tolerance|within|at)\s+([\d.e+-]+)/i); return { name: 'scene.merge_cracks', arguments: { path: selected, tolerance: tolerance ? Number(tolerance[1]) : 0.0001 } }; }
    const skinTransfer = prompt.match(/(?:transfer|copy).*skin.*(?:from|source)\s+[`'\"]?([^`'\"\s]+)/i); if (skinTransfer) return { name: 'scene.transfer_skin_weights', arguments: { targetPath: selected, sourcePath: skinTransfer[1].startsWith('/') ? skinTransfer[1] : selected } };
    if (lower.includes('collision group')) { const named = prompt.match(/(?:named|called)\s+[`'\"]?([^`'\"\s]+)/i); return { name: 'scene.create_collision_group', arguments: { path: selected, ...(named ? { name: named[1] } : {}) } }; }
    if (lower.includes('triangle collider') || lower.includes('reduced collider')) return { name: 'scene.generate_triangle_collider', arguments: { path: selected, targetRatio: .25, targetError: .01 } };
    if ((lower.includes('convex') && lower.includes('hull')) || lower.includes('collider')) return { name: 'scene.generate_convex_hull', arguments: { path: selected } };
    const ratio = prompt.match(/(?:reduce|retopo).*?(\d+)%/i);
    if (ratio) { const sharpEdges = [...prompt.matchAll(/(?:sharp\s+edge|crease)\s*(\d+)\s*(?:[:\-|,]|to)\s*(\d+)/gi)].map((match) => [Number(match[1]), Number(match[2])]), chainMatches = [...prompt.matchAll(/(?:sharp\s+chain|crease\s+chain)\s*([\d\s>]+?)(?:\s*@\s*(\d+(?:\.\d+)?))?(?=\s*(?:;|$))/gi)], sharpChains = chainMatches.map((match) => match[1].split('>').map((value) => Number(value.trim()))), sharpChainSharpness = chainMatches.map((match) => match[2] == null ? 1 : Number(match[2])), lockUVSeams = !/(?:without|unlock|allow|ignore)\s+(?:the\s+)?uv\s+seams?/i.test(prompt), lockBorder = !/(?:without|unlock|allow|ignore)\s+(?:the\s+)?(?:open\s+)?(?:borders?|boundaries?)/i.test(prompt); return { name: 'scene.retopo', arguments: { path: selected, targetRatio: Number(ratio[1]) / 100, tolerance: .0001, lockUVSeams, lockBorder, ...(sharpChains.length ? { sharpChains, sharpChainSharpness } : sharpEdges.length ? { sharpEdges } : {}) } }; }
    if (lower.includes('clean') && lower.includes('mesh')) { const preset = lower.match(/\b(preserve|game[- ]ready|physics[- ]ready|aggressive)\b/), componentVolume = prompt.match(/(?:minimum|min)\s+(?:(?:isolated|closed)\s+)?component\s+volume\s+([\d.e+-]+)/i), componentArea = prompt.match(/(?:minimum|min)\s+(?:isolated\s+)?component\s+area\s+([\d.e+-]+)/i), area = prompt.match(/(?:minimum|min)\s+(?:face\s+)?area\s+([\d.e+-]+)/i); return { name: 'scene.cleanup_mesh', arguments: { path: selected, ...(preset ? { preset: preset[1].replace(' ', '-') } : {}), ...(area && !componentArea && !componentVolume ? { minArea: Number(area[1]) } : {}), ...(componentArea ? { minComponentArea: Number(componentArea[1]) } : {}), ...(componentVolume ? { minComponentVolume: Number(componentVolume[1]) } : {}) } }; }
    if (lower.includes('recompute') && lower.includes('normal')) { const angle = prompt.match(/(?:angle|smooth(?:ing)?)[^\d]*(\d+(?:\.\d+)?)/i), sharpEdges = [...prompt.matchAll(/(?:sharp\s+edge|crease)\s*(\d+)\s*(?:[:\-|,]|to)\s*(\d+)/gi)].map((match) => [Number(match[1]), Number(match[2])]), chainMatches = [...prompt.matchAll(/(?:sharp\s+chain|crease\s+chain)\s*([\d\s>]+?)(?:\s*@\s*(\d+(?:\.\d+)?))?(?=\s*(?:;|$))/gi)], sharpChains = chainMatches.map((match) => match[1].split('>').map((value) => Number(value.trim()))), sharpChainSharpness = chainMatches.map((match) => match[2] == null ? 1 : Number(match[2])); return { name: 'scene.recompute_normals', arguments: { path: selected, weighting: lower.includes('angle') ? 'angle' : 'area', smoothingAngle: angle ? Number(angle[1]) : 180, ...(lower.includes('face-varying') || lower.includes('face varying') ? { interpolation: 'faceVarying' } : {}), ...(sharpChains.length ? { sharpChains, sharpChainSharpness } : sharpEdges.length ? { sharpEdges } : {}) } }; }
    if (lower.includes('recompute') && lower.includes('tangent')) return { name: 'scene.recompute_tangents', arguments: { path: selected } };
    if (/(?:unwrap|generate|create).*(?:uv|atlas)|(?:uv|atlas).*(?:unwrap|generate|create)/i.test(prompt)) return { name: 'scene.unwrap_uv', arguments: { path: selected, resolution: 1024, padding: 2, ...(lower.includes('lightmap') || /\bst1\b/.test(lower) ? { uvSet: 'lightmap' } : {}) } };
    const projection = lower.match(/(?:project|projection).*?\b(planar|box|cylindrical|spherical)\b/); if (projection) return { name: 'scene.project_uv', arguments: { path: selected, mode: projection[1], ...(lower.includes('lightmap') || /\bst1\b/.test(lower) ? { uvSet: 'lightmap' } : {}) } };
    const transfer = prompt.match(/(?:transfer|copy).*?(?:uv|texture coordinates).*?(?:from|source)\s+[`'\"]?([^`'\"\s]+)[`'\"]?/i); if (transfer) return { name: 'scene.transfer_uvs', arguments: { targetPath: selected, sourcePath: transfer[1].startsWith('/') ? transfer[1] : selected, ...(lower.includes('lightmap') || /\bst1\b/.test(lower) ? { uvSet: 'lightmap' } : {}) } };
    if (lower.includes('health') || lower.includes('asset report') || lower.includes('diagnos')) return { name: 'scene.health_report', arguments: {} };
    const resize = prompt.match(/resize(?: the)? texture\s+[`'\"]?([^`'\"\s]+)[`'\"]?(?:\s+to\s+(\d+))?/i);
    if (resize) return { name: 'scene.resize_texture', arguments: { assetPath: resize[1], maxDimension: Number(resize[2] || 4096) } };
    const bake = prompt.match(/bake.*?(\d+)\s*(?:px)?/i);
    const projectionBake = prompt.match(/(?:project|bake).*?(?:from|source)\s+[`'\"]?([^`'\"\s]+)[`'\"]?.*?(?:to|target)\s+[`'\"]?([^`'\"\s]+)[`'\"]?/i);
    if (projectionBake) return { name: 'scene.bake_projection', arguments: { sourcePath: projectionBake[1].startsWith('/') ? projectionBake[1] : selected, targetPath: projectionBake[2].startsWith('/') ? projectionBake[2] : selected, resolution: Number(bake?.[1] || 1024), normalSpace: lower.includes('tangent') ? 'tangent' : 'object', channel: lower.includes('normal') ? 'normal' : lower.includes('roughness') ? 'roughness' : lower.includes('metallic') ? 'metallic' : lower.includes('opacity') ? 'opacity' : lower.includes('emissive') ? 'emissive' : 'baseColor' } };
    if (bake) return { name: 'scene.bake_shading', arguments: { path: selected, resolution: Number(bake[1]), maxResolution: Number(prompt.match(/max(?:imum)?\s+(?:dimension|resolution)\s+(\d+)/i)?.[1] || 2048), normalY: lower.includes('directx') || /\bdx\b/.test(lower) ? 'directx' : 'opengl', normalSpace: lower.includes('tangent') ? 'tangent' : 'object', channel: /object\s*id/.test(lower) ? 'objectID' : /material\s*id/.test(lower) ? 'materialID' : lower.includes('normal') ? 'normal' : lower.includes('roughness') ? 'roughness' : lower.includes('metallic') ? 'metallic' : lower.includes('opacity') ? 'opacity' : lower.includes('emissive') ? 'emissive' : lower.includes('occlusion') || lower.includes('ambient') ? 'occlusion' : 'baseColor' } };
    if (lower.includes('frame') && lower.includes('selected')) return { name: 'scene.inspect', arguments: { path: selected, frame: true } };
    if (lower === 'undo' || lower.includes('undo that')) return { name: 'scene.undo', arguments: {} };
    if (lower === 'redo') return { name: 'scene.redo', arguments: {} };
    if (lower.includes('validate')) return { name: 'scene.validate', arguments: {} };
    if (lower.includes('usd doctor') || lower.includes('usd portability') || lower.includes('package audit')) return { name: 'scene.usd_doctor', arguments: {} };
    if (lower.includes('repair') && lower.includes('usd')) return { name: 'scene.repair_usd_metadata', arguments: {} };
    return null;
  }
  async send(prompt) {
    const text = String(prompt).trim(); if (!text) return;
    this.add('user', text);
    let call;
    try {
      if (this.mode === 'mock') call = this.mock(text);
      else {
        const response = await this.client.complete(this.messages.map(({ role, text }) => ({ role, content: text })), this.getSceneSummary());
        const tc = response.choices?.[0]?.message?.tool_calls?.[0];
        if (!tc) { this.add('assistant', response.choices?.[0]?.message?.content || 'No action was proposed.'); return; }
        call = { name: tc.function.name.replace('__', '.'), arguments: JSON.parse(tc.function.arguments || '{}') };
      }
      if (!call) { this.add('assistant', 'I need a more specific request. Try a template, texture rename, selected-mesh reduction, bake, validation, undo, or redo.'); return; }
      const definition = this.validate(call);
      this.recordDecision?.(call, 'proposed');
      this.add('tool', `${call.name} ${JSON.stringify(call.arguments)}`, { call });
      if (definition.confirm && !(await this.confirm(call))) { this.recordDecision?.(call, 'declined'); this.add('assistant', 'Cancelled. No scene changes were made.'); return; }
      const result = await this.executeTool(call);
      this.recordDecision?.(call, 'executed');
      this.add('assistant', result?.message || 'Done. The change is available in Review Changes and can be undone.');
    } catch (error) { if (typeof call !== 'undefined') this.recordDecision?.(call, 'rejected'); this.add('assistant', `${error.code || 'LUCIA_ASSISTANT'}: ${error.message}`); }
  }
}
