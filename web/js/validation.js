import body from './validation-body.html?raw';
import './validation.css';
document.body.insertAdjacentHTML('afterbegin', body);

const samples = [
  {
    name: 'Clean Xform',
    filename: 'clean.usda',
    text: `#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
)

def Xform "World"
{
}
`
  },
  {
    name: 'Mesh topology issues',
    filename: 'mesh-bad-topology.usda',
    text: `#usda 1.0

def Mesh "badMesh"
{
    point3f[] points = [(0, 0, 0), (1, 0, 0)]
    int[] faceVertexCounts = [3, 0]
    int[] faceVertexIndices = [0, 1, 9, 0]
}
`
  },
  {
    name: 'UsdLux light issues',
    filename: 'lux-light-issues.usda',
    text: `#usda 1.0

def SphereLight "sphere"
{
    float inputs:intensity = -1
    float inputs:radius = 0
    float inputs:shaping:cone:softness = 2
}

def DomeLight_1 "dome"
{
    token inputs:texture:format = "cubeMap"
    token poleAxis = "X"
}
`
  },
  {
    name: 'UsdPhysics issues',
    filename: 'physics-issues.usda',
    text: `#usda 1.0

def PhysicsScene "scene"
{
    vector3f physics:gravityDirection = (0, 0, 0)
    float physics:gravityMagnitude = -1
}

def Mesh "body" (
    prepend apiSchemas = ["PhysicsRigidBodyAPI", "PhysicsCollisionAPI", "PhysicsMeshCollisionAPI"]
)
{
    float physics:mass = -1
    token physics:approximation = "triangleSoup"
    token physics:simulationOwner = "notARelationship"
}

def PhysicsRevoluteJoint "hinge"
{
    token physics:axis = "W"
    token physics:body0 = "notARelationship"
    float physics:lowerLimit = 10
    float physics:upperLimit = 0
}
`
  },
  {
    name: 'UsdPreviewSurface warnings',
    filename: 'preview-surface-warning.usda',
    text: `#usda 1.0

def Material "mat"
{
    def Shader "surface"
    {
        uniform token info:id = "UsdPreviewSurface"
        float inputs:diffuseColor = 0.5
        float inputs:rooughness = 0.5
        token outputs:surface
    }
}
`
  },
  {
    name: 'Shader/material issues',
    filename: 'shader-material-issues.usda',
    text: `#usda 1.0

def Material "mat"
{
    token outputs:surface = "notAConnection"
    token inputs:stPrimvarName = "st"

    def Mesh "notShader"
    {
        token outputs:surface
    }

    token outputs:volume.connect = </mat/notShader.outputs:surface>

    def Shader "tex"
    {
        uniform token info:id = "UsdUVTexture"
        asset inputs:file = @@
        token inputs:wrapS = "tileForever"
        token inputs:sourceColorSpace = "ACEScg"
        float3 outputs:rgb
    }

    def Shader "reader"
    {
        uniform token info:id = "UsdPrimvarReader_float2"
        string inputs:varname.connect = </mat.inputs:stPrimvarName>
        token outputs:result
    }
}
`
  },
  {
    name: 'Skel, MaterialX, composition',
    filename: 'advanced-usd-issues.usda',
    text: `#usda 1.0

def Material "mat" (
    references = @./look.mtlx@
)
{
    string config:mtlx:version = "2.0"
    string config:mtlx:sourceUri = "look.usda"

    def Shader "mtlxShader"
    {
        uniform token info:id = "ND_open_pbr_surface_surfaceshader"
    }
}

def SkelRoot "Rig"
{
    def Skeleton "Skel"
    {
        uniform token[] joints = ["Root/Spine", "Root"]
        uniform matrix4d[] bindTransforms = [
            ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1))
        ]
    }

    def Mesh "Mesh"
    {
        rel skel:skeleton = </Rig/Skel>
        int[] primvars:skel:jointIndices = [0, -1, 1] (
            interpolation = "constant"
            elementSize = 2
        )
        float[] primvars:skel:jointWeights = [1, 0] (
            interpolation = "vertex"
            elementSize = 1
        )
    }
}

def Xform "Composed" (
    references = @./ref.usda@</Ref> (scale = 0)
    payload = @./payload.usda@</Payload> (scale = 0)
    inherits = </Composed>
    variantSets = ["bad-name"]
)
{
}
`
  },
  {
    name: 'Animation, BlendShape, clips',
    filename: 'animation-clips-issues.usda',
    text: `#usda 1.0

over "Animated" (
    clips = {
        dictionary bad = {
            asset[] assetPaths = [@clip_0.usdc@]
            double2[] active = [(0, 0), (1, 2)]
            string primPath = "NotAbsolute"
            double2[] times = [(10, 10), (5, 5)]
        }
        dictionary templ = {
            asset templateAssetPath = @clips/anim.###.usd@
            double templateStartTime = 10
            double templateEndTime = 1
            double templateStride = 0
        }
    }
)
{
}

def SkelRoot "Rig"
{
    def Skeleton "Skel"
    {
        uniform token[] joints = ["Root", "Root/Spine"]
    }

    def SkelAnimation "Anim"
    {
        uniform token[] joints = ["Root", "Root/Spine"]
        quatf[] rotations.timeSamples = {
            1: [(1, 0, 0, 0), (1, 0, 0, 0)]
            2: [(1, 0, 0, 0)]
        }
        uniform token[] blendShapes = ["Smile", "Smile"]
        float[] blendShapeWeights.timeSamples = {
            1: [0, 1]
            2: [0]
        }
    }

    def BlendShape "Smile"
    {
        uniform vector3f[] offsets = [(0, 0, 0), (1, 0, 0)]
        uniform vector3f[] normalOffsets = [(0, 0, 1)]
        uniform int[] pointIndices = [0, -1]
        uniform vector3f[] inbetweens:low = [(0, 0, 0)]
    }

    def Mesh "Mesh"
    {
        point3f[] points = [(0, 0, 0)]
        int[] faceVertexCounts = [1]
        int[] faceVertexIndices = [0]
        rel skel:skeleton = </Rig/Skel>
        int[] primvars:skel:jointIndices = [0, 3] (
            interpolation = "vertex"
            elementSize = 2
        )
        float[] primvars:skel:jointWeights = [0.25, 0.25] (
            interpolation = "vertex"
            elementSize = 2
        )
    }
}
`
  },
  {
    name: 'Metadata and API schemas',
    filename: 'metadata-issues.usda',
    text: `#usda 1.0
(
    defaultPrim = "1Bad"
    kilogramsPerUnit = 0
    colorConfiguration = @@
    colorManagementSystem = ""
    owner = ""
)

def Xform "Root" (
    kind = ""
    instanceable = true
    assetInfo = {
        asset identifier = @@
        string name = ""
        asset[] payloadAssetDependencies = [@@]
    }
    prepend apiSchemas = [
        "CollectionAPI",
        "MaterialBindingAPI:bad",
        "CollectionAPI:look",
        "CollectionAPI:look",
        "UnknownAPI:bad:name"
    ]
)
{
    uniform token side = "middle" (
        allowedTokens = ["left", "right", "left"]
        interpolation = "bogus"
        elementSize = 0
        connectability = "sometimes"
        renderType = ""
        outputName = ""
    )
    varying rel material:binding = </Root>
}
`
  },
  {
    name: 'UsdGeom schema issues',
    filename: 'usdgeom-issues.usda',
    text: `#usda 1.0
def Camera "cam"
{
    float2 clippingRange = (10, 1)
    float focalLength = 0
    float horizontalAperture = -1
    float verticalAperture = 0
    float fStop = -1
    token projection = "fisheye"
    token stereoRole = "center"
    double shutter:open = 1
    double shutter:close = 0
}

def PointInstancer "pi"
{
    point3f[] positions = [(0, 0, 0), (1, 0, 0)]
    int[] protoIndices = [0, -1]
    quath[] orientations = [(1, 0, 0, 0)]
}

def BasisCurves "curves"
{
    uniform token type = "cubic"
    uniform token basis = "bogus"
    uniform token wrap = "loop"
    point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0)]
    int[] curveVertexCounts = [2, 2]
    float[] widths = [0.1, 0.2, 0.3, 0.4]
}
`
  }
];

const state = {
  ready: false,
  busy: false,
  assets: new Map(),
  currentBytes: null,
  currentName: '',
  lastResult: null
};

const statusEl = document.getElementById('status');
const dropzone = document.getElementById('dropzone');
const fileInput = document.getElementById('fileInput');
const chooseFileBtn = document.getElementById('chooseFile');
const validateBtn = document.getElementById('validate');
const fileInfoEl = document.getElementById('fileInfo');
const sampleSelect = document.getElementById('sampleSelect');
const loadSampleBtn = document.getElementById('loadSample');
const groupCore = document.getElementById('groupCore');
const groupGeom = document.getElementById('groupGeom');
const groupShade = document.getElementById('groupShade');
const groupLux = document.getElementById('groupLux');
const groupPhysics = document.getElementById('groupPhysics');
const groupCrate = document.getElementById('groupCrate');
const summaryStatus = document.getElementById('summaryStatus');
const summaryErrors = document.getElementById('summaryErrors');
const summaryWarnings = document.getElementById('summaryWarnings');
const summaryGroups = document.getElementById('summaryGroups');
const issueRows = document.getElementById('issueRows');
const reportEl = document.getElementById('report');
const copyReportBtn = document.getElementById('copyReport');
const downloadReportBtn = document.getElementById('downloadReport');

function setStatus(text) {
  statusEl.textContent = text;
}

function selectedGroups() {
  const groups = [];
  if (groupCore.checked) groups.push('core');
  if (groupGeom.checked) groups.push('geom');
  if (groupShade.checked) groups.push('shade');
  if (groupLux.checked) groups.push('lux');
  if (groupPhysics.checked) groups.push('physics');
  if (groupCrate.checked) groups.push('crate');
  for (const [id, name] of [['groupRender', 'render'], ['groupPackage', 'package'], ['groupArkit', 'arkit']])
    if (document.getElementById(id).checked) groups.push(name);
  return groups.length ? groups : ['core'];
}

function updateGroupsSummary() {
  summaryGroups.textContent = selectedGroups().join(', ');
}

function invalidateResult() {
  state.lastResult = null;
  issueRows.innerHTML = '<tr><td colspan="4">No validation run for the current input and options.</td></tr>';
  reportEl.textContent = '{}';
  summaryErrors.textContent = summaryWarnings.textContent = '0';
  document.getElementById('coverage').textContent = 'Coverage has not been checked.';
  summaryStatus.textContent = 'Not checked';
  summaryStatus.className = 'value';
  for (const id of ['copyReport', 'downloadReport', 'downloadSarif']) document.getElementById(id).disabled = true;
}

function setCurrentInput(bytes, filename) {
  invalidateResult();
  state.currentBytes = bytes;
  state.currentName = filename;
  validateBtn.disabled = !state.ready;
  fileInfoEl.textContent = `${filename} - ${bytes.byteLength.toLocaleString()} bytes`;
}

function formatResult(result) {
  return JSON.stringify(result, null, 2);
}

function renderResult(result) {
  state.lastResult = result;
  const full = result.tool === 'lusdchecker';
  const parseOk = full ? result.executionSuccessful !== false : result.parse_ok !== false;
  const ok = parseOk && (full ? result.valid : result.ok) === true;
  const warningCount = Number(result.warningCount ?? result.warning_count ?? 0);
  summaryStatus.textContent = !parseOk ? 'Execution failed'
    : full && result.complete === false ? 'Incomplete'
    : ok ? (warningCount > 0 ? 'Passed with warnings' : 'Passed') : 'Failed';
  summaryStatus.className = `value ${ok ? (warningCount > 0 ? 'warning' : 'ok') : 'error'}`;
  summaryErrors.textContent = String(result.errorCount ?? result.error_count ?? 0);
  summaryWarnings.textContent = String(warningCount);
  summaryGroups.textContent = (result.checkedGroups || result.checked_groups || selectedGroups()).join(', ');
  document.getElementById('coverage').textContent = full
    ? `Coverage: ${result.complete ? 'complete' : 'incomplete'} · Conformance: ${result.conformance} · Variant passes: ${result.variantPasses ?? 0}. ${result.conformanceScope || ''}`
    : 'Legacy validation: strict coverage and conformance profiles are unavailable.';

  const issues = Array.isArray(result.issues) ? result.issues : [];
  issueRows.innerHTML = '';
  if (!parseOk && !issues.length) {
    const row = document.createElement('tr');
    row.innerHTML = `<td><span class="severity error">error</span></td><td>parse</td><td>${escapeHTML(state.currentName)}</td><td>${escapeHTML(result.error || 'Failed to parse input')}</td>`;
    issueRows.appendChild(row);
  } else if (issues.length === 0) {
    const row = document.createElement('tr');
    row.innerHTML = '<td colspan="4">No issues found.</td>';
    issueRows.appendChild(row);
  } else {
    for (const issue of issues) {
      const severity = issue.severity === 'error' ? 'error' : 'warning';
      const row = document.createElement('tr');
      row.innerHTML = `
        <td><span class="severity ${severity}">${severity}</span></td>
        <td>${escapeHTML(issue.ruleId || issue.rule_id || '')}</td>
        <td>${escapeHTML(issue.location || '')}<div class="issue-context">${escapeHTML([issue.sourceAsset, issue.variants, issue.time !== undefined ? `Time: ${issue.time}` : '', issue.category].filter(Boolean).join('\n'))}</div></td>
        <td>${escapeHTML(issue.message || '')}</td>
      `;
      issueRows.appendChild(row);
    }
  }

  reportEl.textContent = formatResult(result);
  copyReportBtn.disabled = false;
  downloadReportBtn.disabled = false;
  document.getElementById('downloadSarif').disabled = !full || state.busy;
}

function escapeHTML(text) {
  return String(text)
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#039;');
}

const backend = new URLSearchParams(location.search).get('backend') === 'legacy' ? 'legacy' : 'next';
const backendSelect = document.getElementById('backend');
backendSelect.value = backend;
backendSelect.addEventListener('change', () => {
  const url = new URL(location.href); url.searchParams.set('backend', backendSelect.value); location.href = url.href;
});
let worker, requestId = 0;
const pending = new Map();
function request(type, payload = {}) {
  const id = ++requestId;
  return new Promise((resolve, reject) => {
    pending.set(id, {resolve, reject});
    worker.postMessage({id, type, ...payload});
  });
}
function stopWorker(message) {
  worker?.terminate();
  for (const task of pending.values()) task.reject(new Error(message));
  pending.clear(); state.ready = false;
}
async function initWasm() {
  worker = new Worker(new URL('./validation-worker.js', import.meta.url), {type: 'module'});
  worker.onmessage = ({data}) => {
    const task = pending.get(data.id); if (!task) return;
    pending.delete(data.id);
    if (data.error) task.reject(new Error(data.error)); else task.resolve(data.result);
  };
  worker.onerror = event => { stopWorker(event.message || 'Validation worker failed'); setStatus('Validation worker failed; reload to retry.'); };
  try {
    await request('init', {backend});
    state.ready = true;
    setStatus('Ready'); validateBtn.disabled = !state.currentBytes;
  } catch (error) { setStatus(`WASM load failed: ${error.message}`); }
}

async function loadFile(file) {
  if (state.busy) return;
  if (!file) return;
  if (file.size > 512 * 1024 * 1024) { setStatus('Input exceeds the 512 MiB demo limit'); return; }
  const bytes = new Uint8Array(await file.arrayBuffer());
  setCurrentInput(bytes, file.name);
  setStatus('File loaded');
}

function loadSample(index) {
  const sample = samples[index] || samples[0];
  const bytes = new TextEncoder().encode(sample.text);
  setCurrentInput(bytes, sample.filename);
  setStatus('Sample loaded');
}

const profile = document.getElementById('profile');
function updateProfile() {
  const fullProfile = profile.value !== 'default';
  document.getElementById('ruleGroups').disabled = fullProfile;
  for (const id of ['strictWarnings', 'composed', 'allSamples']) {
    const input = document.getElementById(id);
    input.disabled = fullProfile || backend === 'legacy';
    if (fullProfile) input.checked = id !== 'strictWarnings' || profile.value === 'strict';
  }
}
profile.addEventListener('change', () => { updateProfile(); invalidateResult(); });
if (backend === 'legacy') {
  profile.disabled = true;
  for (const id of ['definitions', 'advancedChecks']) document.getElementById(id).hidden = true;
  for (const id of ['groupRender', 'groupPackage', 'groupArkit']) document.getElementById(id).disabled = true;
  document.getElementById('profileHelp').textContent = 'Legacy checks only. Select the next backend for lusdchecker profiles, dependencies, and conformance reports.';
}
updateProfile();
const manifestOptions = async id => Promise.all(Array.from(document.getElementById(id).files)
  .map(async file => {
    if (file.size > 16 * 1024 * 1024) throw new Error('Definition manifest exceeds 16 MiB');
    return JSON.parse(await file.text());
  }));
async function validateCurrentInput(format = 'json') {
  if (!state.ready || !state.currentBytes || state.busy) return;
  const activeWorker = worker;
  state.busy = true; validateBtn.disabled = true;
  const controls = Array.from(document.querySelectorAll('input, select, #chooseFile, #loadSample, #clearAssets'));
  const disabledBefore = controls.map(control => control.disabled);
  controls.forEach(control => { control.disabled = true; });
  document.getElementById('cancel').disabled = false;
  document.getElementById('downloadSarif').disabled = true;
  try {
    setStatus('Validating...');
    let options = {groups: selectedGroups()};
    if (backend === 'next') {
      options = {profile: profile.value, format,
        maxSamples: Number(document.getElementById('maxSamples').value),
        maxMemoryMB: Number(document.getElementById('maxMemory').value),
        schemaDefinitions: await manifestOptions('schemaDefinitions'),
        shaderDefinitions: await manifestOptions('shaderDefinitions')};
      if (profile.value === 'default') Object.assign(options, {
        groups: selectedGroups(), strict: document.getElementById('strictWarnings').checked,
        composed: document.getElementById('composed').checked,
        allTimeSamples: document.getElementById('allSamples').checked});
    }
    if (worker !== activeWorker) throw new Error('Validation cancelled');
    const result = await request('check', {bytes: state.currentBytes, filename: state.currentName, options,
      assets: Array.from(state.assets, ([identifier, bytes]) => ({identifier, bytes}))});
    if (format === 'sarif') downloadJSON(result, 'sarif'); else renderResult(result);
    setStatus('Validation complete');
  } catch (error) {
    invalidateResult();
    setStatus(`Validation failed: ${error.message}`);
    summaryStatus.textContent = 'Execution failed'; summaryStatus.className = 'value error';
  } finally {
    controls.forEach((control, i) => { control.disabled = disabledBefore[i]; });
    state.busy = false; validateBtn.disabled = !state.ready;
    document.getElementById('cancel').disabled = true;
    document.getElementById('downloadSarif').disabled = backend !== 'next' || !state.lastResult;
  }
}
document.getElementById('cancel').addEventListener('click', async () => {
  stopWorker('Validation cancelled');
  await initWasm();
});
document.getElementById('downloadSarif').addEventListener('click', () => validateCurrentInput('sarif'));
function downloadJSON(result, extension) {
  const blob = new Blob([formatResult(result)], {type: 'application/json'});
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a'); link.href = url;
  link.download = `${state.currentName || 'validation'}.${extension}`; link.click();
  URL.revokeObjectURL(url);
}
const rootFile = document.getElementById('rootFile');
function updateAssets() {
  rootFile.replaceChildren();
  for (const name of state.assets.keys()) if (/\.usd[acz]?$/i.test(name)) {
    const option = document.createElement('option'); option.value = name; option.textContent = name;
    option.selected = name === state.currentName; rootFile.appendChild(option);
  }
  document.getElementById('assetInfo').textContent = `${state.assets.size} supplied files`;
}
async function addFiles(files, folder = false) {
  try {
    const entries = Array.from(files);
    let total = Array.from(state.assets.values()).reduce((n, bytes) => n + bytes.byteLength, 0);
    for (const file of entries) {
      total += file.size;
      if (total > 512 * 1024 * 1024) throw new Error('Supplied files exceed the 512 MiB demo limit');
    }
    for (const file of entries) state.assets.set(folder ? file.webkitRelativePath : file.name, new Uint8Array(await file.arrayBuffer()));
    updateAssets(); invalidateResult();
    if (folder && rootFile.value) setCurrentInput(state.assets.get(rootFile.value), rootFile.value);
    setStatus('Dependencies loaded');
  } catch (error) { setStatus(error.message); }
}
document.getElementById('dependencies').addEventListener('change', event => addFiles(event.target.files));
document.getElementById('folder').addEventListener('change', event => addFiles(event.target.files, true));
rootFile.addEventListener('change', () => setCurrentInput(state.assets.get(rootFile.value), rootFile.value));
document.getElementById('clearAssets').addEventListener('click', () => { state.assets.clear(); updateAssets(); invalidateResult(); });

function downloadReport() {
  if (!state.lastResult) return;
  const blob = new Blob([formatResult(state.lastResult)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  const base = state.currentName ? state.currentName.replace(/\.[^.]+$/, '') : 'validation';
  link.href = url;
  link.download = `${base}.validation.json`;
  link.click();
  URL.revokeObjectURL(url);
}

for (let i = 0; i < samples.length; i++) {
  const option = document.createElement('option');
  option.value = String(i);
  option.textContent = samples[i].name;
  sampleSelect.appendChild(option);
}

chooseFileBtn.addEventListener('click', () => fileInput.click());
fileInput.addEventListener('change', () => loadFile(fileInput.files[0]));
validateBtn.addEventListener('click', () => validateCurrentInput());
loadSampleBtn.addEventListener('click', () => loadSample(Number(sampleSelect.value)));
copyReportBtn.addEventListener('click', async () => {
  await navigator.clipboard.writeText(reportEl.textContent);
  setStatus('Report copied');
});
downloadReportBtn.addEventListener('click', downloadReport);

for (const checkbox of document.querySelectorAll('#ruleGroups input')) {
  checkbox.addEventListener('change', () => { updateGroupsSummary(); invalidateResult(); });
}

dropzone.addEventListener('dragover', (event) => {
  event.preventDefault();
  dropzone.classList.add('active');
});

dropzone.addEventListener('dragleave', () => {
  dropzone.classList.remove('active');
});

dropzone.addEventListener('drop', (event) => {
  event.preventDefault();
  dropzone.classList.remove('active');
  loadFile(event.dataTransfer.files[0]);
});

for (const input of document.querySelectorAll('#advancedChecks input, #definitions input')) input.addEventListener('change', invalidateResult);
updateGroupsSummary();
loadSample(0);
// Backend switch reloads the page (the WASM module is chosen at startup).

initWasm();
