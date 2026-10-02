import assert from 'node:assert/strict';
export async function runUDIMBrowserChecks(page) {
  const results = await page.evaluate(async () => {
    const {app} = await import('/lucia-code/src/main.js');
    const {buildUSDZWithNewRoot,parseUSDZEntries} = await import('/src/usdzconvert.js');
    const {validateUSDZArchive} = await import('/lucia-code/src/usd-doctor.js');
    const check=(condition,message)=>{if(!condition)throw new Error(message);};
    const source=`#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {
 def Mesh "Panel" {
  point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
  int[] faceVertexCounts = [3,3]
  int[] faceVertexIndices = [0,1,2,0,2,3]
  uniform token subdivisionScheme = "none"
  bool doubleSided = true
  texCoord2f[] primvars:st = [(0.1,0.1),(0.8,0.1),(0.8,0.8),(0.1,0.8)] (interpolation = "vertex")
  rel material:binding = </World/Mat>
 }
 def Material "Mat" {
  token outputs:surface.connect = </World/Mat/Surface.outputs:surface>
  def Shader "Surface" { uniform token info:id = "UsdPreviewSurface"
   color3f inputs:diffuseColor.connect = </World/Mat/Texture.outputs:rgb>
   token outputs:surface
  }
  def Shader "Texture" { uniform token info:id = "UsdUVTexture"
   asset inputs:file = @textures/tile.<UDIM>.png@
   token inputs:sourceColorSpace = "raw"
   float2 inputs:st.connect = </World/Mat/Reader.outputs:result>
   float3 outputs:rgb
  }
  def Shader "Reader" { uniform token info:id = "UsdPrimvarReader_float2"
   token inputs:varname = "st"
   float2 outputs:result
  }
 }
}`;
    const png=async color=>{
      const canvas=document.createElement('canvas');canvas.width=canvas.height=2;
      const ctx=canvas.getContext('2d');ctx.fillStyle=color;ctx.fillRect(0,0,2,2);
      const blob=await new Promise(resolve=>canvas.toBlob(resolve,'image/png'));
      return new Uint8Array(await blob.arrayBuffer());
    };
    const entries=[{name:'textures/tile.1001.png',data:await png('#ff0000')},{name:'textures/tile.1002.png',data:await png('#00ff00')}];
    // Let the existing package writer compute CRCs through the normal exporter.
    app.project.reset('UDIM');app.project.assets=new Map(entries.map(e=>[e.name,{bytes:e.data}]));
    await app.session.loadUSDA(source,'root.usda');
    const originalArchive=app.session.exportUSDZ(app.project.assets);
    validateUSDZArchive(originalArchive);
    await app.openFile(new File([originalArchive],'fixture.usdz'));
    check(app.project.assets.has('textures/tile.1002.png'),'USDZ tiles were not retained');
    const original=app.session.exportUSDA(), originalKeys=[...app.project.assets.keys()];
    app.currentInspectorTab='textures';app.renderInspector();
    check(app.$('#udim-max-tiles').value==='100','Tile cap default missing');
    check(getComputedStyle(app.$('#udim-dense-controls')).display==='none','Dense-only controls visible in grid mode');
    check(app.$('#udim-preview').disabled,'Preview enabled before detecting sets');
    await app.$('#udim-detect').onclick();
    const detected={inventory:app.udimInventory};
    check(detected.inventory[0].ids.length===2,'Worker did not discover tiles');
    check(detected.inventory[0].consumers.includes('/World/Panel'),'Consumer mesh not reported');
    await app.$('#udim-select-none').onclick();
    check(app.$('#udim-preview').disabled && app.udimSelection.size===0,'Empty selection can bake');
    await app.$('#udim-select-all').onclick();
    check(!app.$('#udim-preview').disabled,'Select all did not enable preview');
    app.$('#udim-max-tiles').value='';
    await app.$('#udim-preview').onclick();
    check(!app.operations.worker && !app.udim.preview,'Empty numeric option started a worker');
    app.$('#udim-max-tiles').value='100';
    app.udimInventory=detected.inventory;app.udimSelection=new Set(['/World/Mat/Texture']);app.renderInspector();
    const base={...app.udimOptions,udimShaderPaths:[...app.udimSelection]};
    let rejected=false;try{await app.udim.bake({...base,udimMaxTiles:1});}catch(e){rejected=/tile count/.test(e.message);}
    check(rejected && app.session.exportUSDA()===original && !app.commands.canUndo,'Tile limit was not atomic');
    await app.$('#udim-preview').onclick();
    const preview=app.udim.preview;
    check(preview,'Preview button failed');
    check(app.session.exportUSDA()===original && app.project.assets.size===originalKeys.length,'Preview mutated project');
    check(preview.assets.length===1 && preview.assets[0].thumbnail?.length,'Atlas or thumbnail missing');
    const cancelledDetection=app.$('#udim-detect').onclick();
    await Promise.resolve();app.operations.cancel();await cancelledDetection;
    check(!app.udim.preview && app.$('#udim-apply').disabled && !app.$('#udim-panel').inert,'Cancelled detection left stale or locked controls');
    await app.$('#udim-preview').onclick();
    check(!app.$('#udim-apply').disabled && app.$('#udim-atlases img'),'Preview UI missing');
    const inspector=app.$('#inspector');
    check(inspector.scrollWidth<=inspector.clientWidth+1,'UDIM controls overflow the narrow inspector');
    const mesh=app.bridge.content.getObjectByProperty('isMesh',true);
    const meshMaterials=Array.isArray(mesh?.material)?mesh.material:[mesh?.material];
    check(meshMaterials.some(material=>material?.map?.image),'Staged atlas did not reach viewport material');
    app.$('#udim-max-tiles').value='';
    await app.$('#udim-apply').onclick();
    check(!app.$('#confirm-dialog').open && !app.commands.canUndo && app.session.exportUSDA()===original,'Invalid edited limits allowed Apply');
    app.$('#udim-max-tiles').value='100';
    const applying=app.$('#udim-apply').onclick();
    check(app.$('#confirm-dialog').open,'Apply did not request confirmation');
    app.$('#confirm-dialog [value="confirm"]').click();
    await applying;
    check(app.commands.undoItems.length===1 && app.project.assets.size===originalKeys.length+1,'Apply is not one command');
    check(app.udimInventory===null && !app.$('[data-udim-path]'),'Applied scene retained stale detection rows');
    const baked=app.session.exportUSDA();check(!baked.includes('tile.<UDIM>'),'USD texture references were not changed');
    const exported=app.session.exportUSDZ(app.project.assets);validateUSDZArchive(exported);
    const packaged=parseUSDZEntries(exported);
    check(packaged.some(entry=>entry.name===preview.assets[0].name),'Atlas absent from exported USDZ');
    const bakedPNG=packaged.find(entry=>entry.name===preview.assets[0].name);
    const bitmap=await createImageBitmap(new Blob([bakedPNG.data],{type:'image/png'}));
    const canvas=document.createElement('canvas');canvas.width=bitmap.width;canvas.height=bitmap.height;
    const ctx=canvas.getContext('2d');ctx.drawImage(bitmap,0,0);bitmap.close();
    const left=ctx.getImageData(0,0,1,1).data,right=ctx.getImageData(canvas.width-1,0,1,1).data;
    check(left[0]===255 && right[1]===255,'Stitched atlas pixels do not preserve red/green tiles');
    await app.commands.undo();await app.refreshAll(false);
    check(app.session.exportUSDA()===original && app.project.assets.size===originalKeys.length,'Undo did not restore USD and assets');
    await app.commands.redo();await app.refreshAll(false);
    check(app.session.exportUSDA()===baked && app.project.assets.size===originalKeys.length+1,'Redo did not restore atlas');
    await app.commands.undo();await app.refreshAll(false);
    const denseOptions={...base,udimBake:'dense'};
    const dense=await app.udim.bake(denseOptions);
    check(dense.usda.includes('_udimAtlas'),'Dense UV primvar missing');
    check(await app.runMutation('Dense UDIM',dense.paths,()=>app.udim.apply(denseOptions)),'Dense apply failed');
    validateUSDZArchive(app.session.exportUSDZ(app.project.assets));
    await app.commands.undo();await app.refreshAll(false);
    await app.udim.bake(base);app.project.changed('Intervening change',[],['usd']);
    let stale=false;try{await app.udim.apply(base);}catch(e){stale=e.code==='LUCIA_UDIM_STALE';}check(stale,'Stale preview was accepted');
    app.udim.discard();
    const pending=app.udim.bake(base);app.operations.cancel();
    let cancelled=false;try{await pending;}catch(e){cancelled=e.code==='LUCIA_CANCELLED';}check(cancelled,'Cancellation left a running operation');
    check(app.session.exportUSDA()===original,'Cancelled operation changed USD');
    // Exercise the real next-only worker with HDR input and EXR atlas output.
    const hdr=new Uint8Array([...new TextEncoder().encode('#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n'),128,0,0,129,0,128,0,130]);
    app.project.assets=new Map(['1001','1002'].map(id=>[`textures/tile.${id}.hdr`,{bytes:hdr.slice()}]));
    await app.session.loadUSDA(source.replace('tile.<UDIM>.png','tile.<UDIM>.hdr'),'root.usda');
    const hdrPreview=await app.udim.bake(base),hdrAtlas=hdrPreview.assets[0];
    check(hdrAtlas.name.endsWith('.exr') && hdrAtlas.thumbnail?.length,'HDR worker did not preserve EXR or generate a thumbnail');
    const hdrBitmap=await createImageBitmap(new Blob([hdrAtlas.thumbnail],{type:'image/png'}));
    check(hdrBitmap.width<=256 && hdrBitmap.height<=256,'HDR thumbnail exceeds its bounded dimensions');hdrBitmap.close();
    app.udim.discard();
    // A nested package root resolves atlas names relative to its layer directory.
    const nested=buildUSDZWithNewRoot('scenes/root.usda',new TextEncoder().encode(source),parseUSDZEntries(originalArchive).filter(e=>!/\.usd[ac]?$/.test(e.name)).map(e=>({...e,name:`scenes/${e.name}`})));
    await app.openFile(new File([nested],'nested.usdz'));
    const nestedPreview=await app.udim.bake(base);
    check(nestedPreview.assets[0].name.startsWith('scenes/'),'Nested atlas lost its root directory');
    app.udim.discard();await app.refreshAll(false);
    const imported=app.session.exportUSDA(), importedAssets=app.project.assets, historySize=app.commands.undoItems.length;
    await app.openFile(new File(['this is not a USDZ archive'],'invalid.usdz'));
    check(app.session.exportUSDA()===imported && app.project.assets===importedAssets && app.commands.undoItems.length===historySize,'Failed import discarded the working project');
    return {tiles:detected.inventory[0].ids,grid:true,dense:true,undoRedo:true,cancellation:true,nested:true};
  });
  assert.deepEqual(results.tiles,[1001,1002]);
  console.log('Lucia UDIM browser: grid/dense previews, tile limits, package pixels, undo/redo, stale results, cancellation and nested roots passed');
}
