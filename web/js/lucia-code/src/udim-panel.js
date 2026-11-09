import { UDIM_DEFAULTS, stageAssetFiles, safeAssetPath } from './udim-workflow.js';
import { downloadBlob, basename, LuciaError } from './utils.js';
const esc = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
export function renderUDIMPanel(app) {
  app.udimPanelCleanup?.();
  app.udimOptions ||= {...UDIM_DEFAULTS};
  const state = app.udim, options = app.udimOptions, urls = [];
  const inventory = app.udimInventory || [];
  const detected = app.udimInventory != null;
  app.udimSelection ||= new Set();
  const ready = inventory.filter(item => !item.error);
  const selected = ready.filter(item => app.udimSelection.has(item.path));
  const number = (id,label,value,min,max) => `<label>${label}<input id="${id}" type="number" required min="${min}" max="${max}" step="1" value="${value}"></label>`;
  const preview = state.preview;
  const atlases = preview?.assets || [...app.project.assets].filter(([,a])=>a.udimLayout).map(([name,a])=>({name,data:a.bytes,layout:a.udimLayout}));
  app.$('#inspector').insertAdjacentHTML('afterbegin', `<section class="section" id="udim-panel"><h3>Stitch UDIM textures</h3>
    <p class="empty">Select texture shaders to stitch. Every shared consumer and animated file opinion of each selected shader is included. Tile limits reject excess tiles; source tiles are retained.</p>
    <label>Loose-file directory prefix <input id="udim-asset-prefix" placeholder="textures" value="${esc(app.udimAssetPrefix || '')}"></label>
    <label class="file-button">Add tile files<input id="udim-files" class="sr-only" type="file" multiple accept=".png,.jpg,.jpeg,.exr,.hdr"></label>
    <label class="file-button">Add asset folder<input id="udim-folder" class="sr-only" type="file" webkitdirectory multiple></label>
    <p class="empty">${app.project.assets.size} assets loaded · ${([...app.project.assets.values()].reduce((n,a)=>n+a.bytes.byteLength,0)/1048576).toFixed(1)} MiB</p>
    <button id="udim-detect">Detect UDIM sets</button>
    ${ready.length?`<div class="udim-actions"><button id="udim-select-all">Select all</button><button id="udim-select-none">Clear selection</button><small>${selected.length} of ${ready.length} ready sets selected</small></div>`:''}
    <div id="udim-sets">${inventory.map(item=>`<label class="udim-set"><input type="checkbox" data-udim-path="${esc(item.path)}" ${!item.error && app.udimSelection.has(item.path)?'checked':''} ${item.error?'disabled':''}><span><code>${esc(item.path)}</code><small>${esc(item.patterns.join(', '))} · ${item.ids.length} tiles: ${item.ids.join(', ')}</small>${item.error?`<small class="udim-error">${esc(item.error)}</small>`:''}<small>Consumers: ${esc(item.consumers?.join(', ') || 'No bound mesh found')}</small></span></label>`).join('') || `<p class="empty">${detected?'No UDIM texture shaders found in this scene.':'Detect sets after loading the scene and its tiles.'}</p>`}</div>
    <label>Layout<select id="udim-layout"><option value="grid" ${options.udimBake==='grid'?'selected':''}>Grid (UV transform)</option><option value="dense" ${options.udimBake==='dense'?'selected':''}>Dense (rewrites geometry UVs)</option></select></label>
    ${number('udim-max-tiles','Maximum tiles per layout',options.udimMaxTiles,1,8999)}
    ${number('udim-max-edge','Maximum atlas edge',options.udimMaxAtlasSize,1,32768)}
    ${number('udim-memory','Working budget (MiB)',options.udimMemoryBudgetBytes/1048576,1,2047)}
    <div id="udim-dense-controls" ${options.udimBake==='dense'?'':'hidden'}><label>Crossing faces<select id="udim-cross"><option value="reject" ${options.udimCrossTile==='reject'?'selected':''}>Reject</option><option value="split" ${options.udimCrossTile==='split'?'selected':''}>Split</option></select></label>${number('udim-padding','Dense gutter (pixels)',options.udimDensePadding,0,1024)}${number('udim-subdivision','Subdivision level',options.udimSubdivisionLevel,1,8)}</div>
    <button id="udim-preview" ${selected.length?'':'disabled'}>Preview stitching</button><button id="udim-discard" ${preview?'':'disabled'}>Discard preview</button>
    <button id="udim-apply" ${preview?'':'disabled'}>Apply preview</button>
    <div id="udim-summary" role="status">${preview?`${preview.paths.length} selected shaders · ${preview.tiles} tiles · ${preview.assets.length} atlases. ${options.udimBake==='dense'?'Dense changes geometry, UV primvars and readers.':'Grid changes texture paths and UV transforms.'} Review the viewport before Apply.`:''}</div>
    <div id="udim-atlases">${atlases.map((asset,i)=>{
      let image='';
      const bytes=asset.thumbnail || (/\.png$/i.test(asset.name) && asset.data.length<4*1048576?asset.data:null);
      if(bytes) { const url=URL.createObjectURL(new Blob([bytes],{type:'image/png'})); urls.push(url); image=`<img src="${url}" alt="Atlas ${esc(asset.name)}" width="160">`; }
      return `<div class="udim-atlas">${image}<code>${esc(asset.name)}</code><small>${asset.layout.width} × ${asset.layout.height} · ${asset.data.length.toLocaleString()} bytes</small><button data-udim-download="${i}">Download atlas</button></div>`;
    }).join('')}</div></section>`);
  const atlasImages = [...app.$$('#udim-atlases img')];
  app.udimPanelCleanup = () => {
    // Cancel queued image loads before revoking their URLs on rapid rerenders.
    atlasImages.forEach(img=>img.removeAttribute('src'));
    urls.forEach(url=>URL.revokeObjectURL(url));
  };
  const error = e => { app.setBusy(false); if(e.code!=='LUCIA_CANCELLED') app.showError(e); };
  const progress = p => app.setBusy(true,p.message,p.percentage);
  app.$('#udim-panel').inert = !!app.udimPanelBusy;
  let lease;
  const begin = (exclusive = false) => {
    if (app.udimPanelBusy) return false;
    if (exclusive) {
      try { lease = app.activity.acquire(); }
      catch(e) { app.showError(e); return false; }
    }
    app.udimPanelBusy = true;
    app.$('#udim-panel').inert = true;
    return true;
  };
  const end = () => {
    lease?.release(); lease = null;
    app.udimPanelBusy = false;
    // Rebuild controls from authoritative state even after cancellation.
    if (app.$('#udim-panel')) app.renderInspector();
  };
  const read = () => ({udimBake:app.$('#udim-layout').value,udimMaxTiles:Number(app.$('#udim-max-tiles').value),
    udimMaxAtlasSize:Number(app.$('#udim-max-edge').value),udimMemoryBudgetBytes:Number(app.$('#udim-memory').value)*1048576,
    udimCrossTile:app.$('#udim-cross').value,udimDensePadding:Number(app.$('#udim-padding').value),udimSubdivisionLevel:Number(app.$('#udim-subdivision').value)});
  const valid = () => [...app.$$('#udim-panel input[type=number]')].every(el => el.reportValidity());
  const invalidate = async () => {
    if (!valid() || !begin(true)) return;
    try { app.udimOptions=read(); await app.hideUDIMPreview(); state.discard(); }
    catch(e) { error(e); } finally { end(); }
  };
  app.$$('#udim-panel input[type=number], #udim-panel select').forEach(el=>el.onchange=()=>invalidate().catch(error));
  const changeSelection = async update => {
    if (!begin(true)) return;
    try { await app.hideUDIMPreview(); state.discard(); update(); }
    catch(e) { error(e); } finally { end(); }
  };
  app.$$('[data-udim-path]').forEach(el=>el.onchange=()=>changeSelection(()=>{
    if(el.checked) app.udimSelection.add(el.dataset.udimPath); else app.udimSelection.delete(el.dataset.udimPath);
  }));
  if (ready.length) {
    app.$('#udim-select-all').onclick=()=>changeSelection(()=>{app.udimSelection=new Set(ready.map(item=>item.path));});
    app.$('#udim-select-none').onclick=()=>changeSelection(()=>{app.udimSelection.clear();});
  }
  app.$('#udim-asset-prefix').onchange=e=>{app.udimAssetPrefix=e.target.value;};
  const addFiles = async event => {
    if (!begin()) return;
    try {
      const stamp=state.stamp(UDIM_DEFAULTS);
      const files=[...event.target.files]; let total=[...app.project.assets.values()].reduce((n,a)=>n+a.bytes.length,0);
      for(const file of files) { total+=file.size; if(total>256*1048576) throw new LuciaError('LUCIA_ASSET_MEMORY','Selected assets exceed the 256 MiB browser limit.'); }
      const prefix=app.udimAssetPrefix?`${safeAssetPath(app.udimAssetPrefix)}/`:'';
      const entries=await Promise.all(files.map(async file=>({name:prefix+(file.webkitRelativePath || file.name),bytes:new Uint8Array(await file.arrayBuffer())})));
      if (!state.matches(stamp,UDIM_DEFAULTS)) throw new LuciaError('LUCIA_UDIM_STALE','Scene or assets changed while reading files. Add the tiles again.');
      const assets=stageAssetFiles(app.project.assets,entries);
      await app.runMutation('Load UDIM tile assets',[],async()=>{app.project.assets=assets;await app.session.rebuildRender();},['assets']);
      app.udimInventory=null;
    }catch(e){error(e);}finally{event.target.value='';end();}
  };
  app.$('#udim-files').onchange=addFiles; app.$('#udim-folder').onchange=addFiles;
  app.$('#udim-detect').onclick=async()=>{
    if (!begin(true)) return;
    try {await app.hideUDIMPreview(); state.discard();app.setBusy(true,'Detecting UDIM sets…',0);
      if (lease.cancelled) throw new LuciaError('LUCIA_CANCELLED','Scene replacement cancelled detection.');
      app.udimInventory=(await state.inspect(progress)).inventory;app.udimSelection=new Set(app.udimInventory.filter(item=>!item.error).map(item=>item.path));
      app.setBusy(false);
    }catch(e){error(e);}finally{end();}
  };
  app.$('#udim-preview').onclick=async()=>{
    if (!valid()) return;
    if (!begin(true)) return;
    try {await app.hideUDIMPreview();app.udimOptions=read();app.setBusy(true,'Stitching UDIM preview…',0);
      if (lease.cancelled) throw new LuciaError('LUCIA_CANCELLED','Scene replacement cancelled preview.');
      const result=await state.bake({...app.udimOptions,udimShaderPaths:[...app.udimSelection].sort()},progress);
      const render=app.session.createRender(result.usda,state.stagedAssets(result));
      try {app.udimViewportPreview=true;await app.bridge.rebuild(render);} finally {render.delete();}
      app.setBusy(false);
    }catch(e){await app.hideUDIMPreview();state.discard();error(e);}finally{end();}
  };
  app.$('#udim-discard').onclick=()=>changeSelection(()=>{});
  app.$('#udim-apply').onclick=async()=>{
    if (!valid() || !begin()) return;
    try {
      const opts={...app.udimOptions,udimShaderPaths:[...app.udimSelection].sort()};
      const paths=state.preview?.paths || [];
      if(!await app.confirmAction('Apply UDIM stitching?',`Update ${paths.length} texture shaders and their consumers using ${opts.udimBake} layout? USD edits and atlas assets are one undoable command.`)) return;
      await app.runMutation('Stitch UDIM texture sets',paths,()=>state.apply(opts));
    } catch(e) { error(e); } finally { end(); }
  };
  app.$$('[data-udim-download]').forEach(button=>button.onclick=()=>{const asset=atlases[Number(button.dataset.udimDownload)];downloadBlob(new Blob([asset.data]),basename(asset.name));});
}
