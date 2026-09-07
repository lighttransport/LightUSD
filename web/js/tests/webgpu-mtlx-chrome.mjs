// SPDX-License-Identifier: Apache-2.0
import puppeteer from 'puppeteer';
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const hardware = process.argv.includes('--hardware');
const shaderball = process.argv.includes('--shaderball');
const executablePath = process.env.CHROME_PATH || (process.platform === 'win32' ? 'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe' : undefined);
const port = Number(process.env.WEBGPU_MTLX_TEST_PORT || 5198);
const server = spawn(process.execPath, ['node_modules/vite/bin/vite.js', '--config', 'vite.webgpu-mtlx.config.mjs', '--port', String(port)], { cwd: root, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
let log = ''; server.stdout.on('data', d => { log += d; }); server.stderr.on('data', d => { log += d; });
let browser, page; const browserLog=[];
try {
  let started = false;
  for (let i = 0; i < 100; i++) { try { if ((await fetch(`http://127.0.0.1:${port}/webgpu-mtlx.html`)).ok) { started = true; break; } } catch {} await new Promise(r => setTimeout(r, 100)); }
  if (!started) throw new Error(`Vite failed: ${log}`);
  browser = await puppeteer.launch({ executablePath, headless: true, protocolTimeout: 600000, args: hardware ? [] : ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
  page = await browser.newPage(); await page.setViewport({ width: 1100, height: 700 });
  page.on('console',msg=>{browserLog.push(msg.text());if(browserLog.length>30)browserLog.shift();});
  const errors = []; page.on('pageerror', e => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}/webgpu-mtlx.html?manual&width=96&height=64`);
  await page.waitForFunction(() => window.__webgpuMtlx?.ready || window.__webgpuMtlx?.errors.length, { timeout: 60000 });
  const initial = await page.evaluate(() => ({ ready: window.__webgpuMtlx.ready, errors: window.__webgpuMtlx.errors }));
  assert.deepEqual(initial.errors, [], 'initialization errors'); assert.ok(initial.ready);
  const results = await page.evaluate(async () => {
    const state = window.__webgpuMtlx, r = state.renderer;
    const {validateResourceLoading}=await import('/src/webgpu-mtlx/resource-validation.js');
    const resourceLoading=await validateResourceLoading();
    const {validateLibraryGraphs}=await import('/src/webgpu-mtlx/library-validation.js');const libraryGraphs=await validateLibraryGraphs(r.device);
    const {validateReferenceScenes}=await import('/src/webgpu-mtlx/reference-validation.js');const referenceScenes=await validateReferenceScenes(r);await r.loadScene((await import('/src/webgpu-mtlx/scene.js')).syntheticScene());r.setMode('path-preview');
    const { validateValueKernels } = await import('/src/webgpu-mtlx/gpu-validation.js');
    const numeric = await validateValueKernels(r.device);
    const { validateImageKernels } = await import('/src/webgpu-mtlx/image-validation.js');
    const imageNumeric = await validateImageKernels(r.device);
    const { validateTransportKernels } = await import('/src/webgpu-mtlx/transport-validation.js');
    const transport = await validateTransportKernels(r.device);
    const { validateSpectrumKernels } = await import('/src/webgpu-mtlx/spectrum-validation.js');
    const spectrum = await validateSpectrumKernels(r.device);
    for (let i = 0; i < 4; i++) await r.renderStep();
    const capture = await r.capture({ format: 'float32' });
    const luminance = []; for (let i = 0; i < capture.pixels.length; i += 4) luminance.push(capture.pixels[i] + capture.pixels[i + 1] + capture.pixels[i + 2]);
    const samples = r.samples; r.setOptions({ exposure: 1 }); const exposureSamples = r.samples;
    r.setCamera(r.camera); const reset = r.samples; await r.renderStep();
    r.setMode('realtime'); await r.renderStep(); const raster = { ...r.stats };
    r.setMode('path-preview'); r.setOptions({ resolutionScale: 0.5 }); await r.renderStep();
    const resized = await r.capture({ format: 'float32' });
    const { syntheticScene } = await import('/src/webgpu-mtlx/scene.js'); await r.loadScene(syntheticScene('graph')); await r.renderStep();
    const textured = syntheticScene(); const doc = textured.materials[1];
    doc.images = { checker: { width: 2, height: 2, colorspace: 'srgb_texture', data: [1,0,0,1, 0,1,0,1, 0,0,1,1, 1,1,1,1] } };
    doc.nodes[0].inputs.base_color = { nodename: 'image' };
    doc.nodes.unshift({ name: 'image', category: 'image', type: 'color3', inputs: { file: { type: 'filename', value: 'checker' } } });
    await r.loadScene(textured); await r.renderStep();
    const imageCapture = await r.capture({ format: 'float32' });
    if (!imageCapture.pixels.every(Number.isFinite)) throw new Error('Non-finite textured render');
    const { decodeImage } = await import('/src/webgpu-mtlx/resources.js');
    const oversizedCanvas = document.createElement('canvas'); oversizedCanvas.width = 512; oversizedCanvas.height = 512;
    const oversizedContext = oversizedCanvas.getContext('2d'); oversizedContext.fillStyle = '#c84'; oversizedContext.fillRect(0, 0, 512, 512);
    const oversizedBlob = await new Promise(resolve => oversizedCanvas.toBlob(resolve, 'image/png'));
    const oversizedBytes = new Uint8Array(await oversizedBlob.arrayBuffer());
    const reducedImage = await decodeImage(oversizedBytes, { filename: 'oversized.png', maxPixels: 64 * 64, allowDownsample: true });
    if (reducedImage.width * reducedImage.height > 64 * 64 || !reducedImage.resizedFrom || !reducedImage.data.every(Number.isFinite)) throw new Error('Regular image downsample budget failed');
    let rejectedOversized = false; try { await decodeImage(oversizedBytes, { filename: 'oversized.png', maxPixels: 64 * 64 }); } catch { rejectedOversized = true; }
    if (!rejectedOversized) throw new Error('Regular image budget rejection failed');
    r.setMode('realtime'); await r.renderStep();
    await r.setMaterialDocument(doc); await r.renderStep(); r.setMode('path-preview');
    await r.loadScene(syntheticScene('rough-glass')); r.setMode('path-physical');
    let dispatches=0; while(r.samples<4 && dispatches<200) { await r.renderStep(); dispatches++; }
    if(r.samples!==4) throw new Error('Resumable transport did not finish four samples');
    const physicalCapture=await r.capture({format:'float32'});
    if(!physicalCapture.pixels.every(Number.isFinite)) throw new Error('Physical capture is non-finite');
    const physical={dispatches,samples:r.samples};
    const spectralScene=syntheticScene('glass'); spectralScene.materials[1].spectra={ior:[[360,1.6],[830,1.4]]};
    await r.loadScene(spectralScene); r.setMode('path-spectral');
    let spectralDispatches=0; while(r.samples<4 && spectralDispatches<200) {await r.renderStep();spectralDispatches++;}
    if(r.samples!==4)throw new Error('Spectral transport did not finish');
    const spectralCapture=await r.capture({format:'float32'});
    if(spectralCapture.metadata.colorSpace!=='CIE XYZ' || !spectralCapture.pixels.every(Number.isFinite))throw new Error('Invalid spectral capture');
    await r.loadScene(syntheticScene('sss'));let volumeDispatches=0;
    while(r.samples<2 && volumeDispatches<400){await r.renderStep();volumeDispatches++;}
    if(r.samples!==2)throw new Error('Random-walk volume did not finish');
    const volumeCapture=await r.capture({format:'float32'});if(!volumeCapture.pixels.every(Number.isFinite))throw new Error('Invalid volume radiance');
    const heterogeneous=syntheticScene('sss');heterogeneous.materials[1].mediumMajorant=5;await r.loadScene(heterogeneous);let heterogeneousDispatches=0;
    while(r.samples<1&&heterogeneousDispatches<400){await r.renderStep();heterogeneousDispatches++;}if(r.samples!==1)throw new Error('Delta-tracking did not finish');
    const {bakeDisplacement}=await import('/src/webgpu-mtlx/displacement.js');
    const flat={positions:[0,0,0,1,0,0,0,1,0],indices:[0,1,2],materials:[{nodes:[{name:'height',category:'constant',type:'float',inputs:{value:{type:'float',value:.25}}}],displacementOutput:{nodename:'height'}}],displacementRefinement:1};
    const baked=await bakeDisplacement(flat,r.device);for(let i=2;i<baked.positions.length;i+=3)if(Math.abs(baked.positions[i]-.25)>1e-6)throw new Error('Displacement mismatch');
    await r.loadScene(syntheticScene('displacement'));r.setMode('realtime');await r.renderStep();
    if(!r.scene.provenance.displacement?.bakedBeforeBVH)throw new Error('Displacement did not rebuild scene geometry');
    const emitterMaterial={nodes:[
      {name:'black',category:'oren_nayar_diffuse_bsdf',type:'BSDF',inputs:{weight:{type:'float',value:0}}},
      {name:'emission',category:'uniform_edf',type:'EDF',inputs:{color:{type:'color3',value:[2,3,4]}}},
      {name:'surface',category:'surface',type:'surfaceshader',inputs:{bsdf:{nodename:'black'},edf:{nodename:'emission'}}}
    ]};
    const emitterScene={positions:[-10,-10,0,10,-10,0,10,10,0,-10,10,0],indices:[0,1,2,0,2,3],materials:[emitterMaterial],camera:{origin:[0,0,1],target:[0,0,0],fov:45},lighting:{environment:[0,0,0],directional:{radiance:[0,0,0]}}};
    await r.loadScene(emitterScene);r.setMode('path-physical');while(r.samples<3)await r.renderStep();
    const emissionCapture=await r.capture({format:'float32'});
    for(let i=0;i<emissionCapture.pixels.length;i+=4)for(let k=0;k<3;k++)if(Math.abs(emissionCapture.pixels[i+k]-(k+2))>1e-5||emissionCapture.variance[i+k]>1e-10)throw new Error('Analytic emitter radiance/variance failed');
    r.setCamera({origin:[0,0,-1],target:[0,0,0],fov:45});while(r.samples<1)await r.renderStep();
    const backEmitter=await r.capture({format:'float32'});for(let i=0;i<backEmitter.pixels.length;i+=4)if(backEmitter.pixels[i]!==0||backEmitter.pixels[i+1]!==0||backEmitter.pixels[i+2]!==0)throw new Error('One-sided emitter leaked through back face');
    emitterMaterial.spectra={emission_color:[[360,2],[830,2]]};await r.loadScene(emitterScene);r.setMode('path-spectral');while(r.samples<16)await r.renderStep();
    const spectralEmitter=await r.capture({format:'float32'});let meanY=0;for(let i=1;i<spectralEmitter.pixels.length;i+=4)meanY+=spectralEmitter.pixels[i]/spectralEmitter.sampleCounts.length;
    if(Math.abs(meanY-2)>.06)throw new Error(`Equal-energy spectral emitter Y=${meanY}`);
    emitterMaterial.nodes[0].inputs.weight.value=-1;await r.loadScene(emitterScene);r.setMode('path-physical');
    let rejected=false;try{await r.renderStep();}catch(e){rejected=/Physical transport invalid/.test(e.message);}if(!rejected)throw new Error('Negative closure weight was not rejected');
    rejected=false;try{await r.capture({format:'float32'});}catch(e){rejected=/Physical transport invalid/.test(e.message);}if(!rejected)throw new Error('Invalid transport capture was not blocked');
    await r.loadScene(syntheticScene('image')); r.setMode('path-preview');
    return { referenceScenes,libraryGraphs,resourceLoading,numeric, imageNumeric, transport, spectrum, physical, spectralDispatches, volumeDispatches,analyticEmitter:{rgb:[2,3,4],spectralMeanY:meanY}, samples, exposureSamples, reset, raster, resized: resized.metadata, min: Math.min(...luminance), max: Math.max(...luminance), finite: capture.pixels.every(Number.isFinite), adapter: r.stats.adapter, errors: [...state.errors, ...r.errors] };
  });
  assert.equal(results.samples, 4); assert.equal(results.exposureSamples, 4); assert.equal(results.reset, 0);
  assert.ok(results.finite); assert.ok(results.max - results.min > 0.1, 'nonblank radiance');
  assert.equal(results.resized.width, 48); assert.equal(results.resized.height, 32);
  assert.deepEqual(results.errors, []); assert.deepEqual(errors, []);
  await page.click('#inventory');
  await page.waitForFunction(() => window.__webgpuMtlx.inventory || window.__webgpuMtlx.errors.length, { timeout: 60000 });
  const inventory = await page.evaluate(() => window.__webgpuMtlx.inventory);
  assert.ok(inventory?.length > 100, 'pinned library inventory');
  if (hardware) assert.ok(!results.adapter.isFallbackAdapter && !/swiftshader|software/i.test(JSON.stringify(results.adapter)), 'hardware gate must not use software');
  const out = path.join(root, '.regression/webgpu-mtlx'); fs.mkdirSync(out, { recursive: true });
  await page.evaluate(async () => {
    const r = window.__webgpuMtlx.renderer;
    r.canvas.width = 640; r.canvas.height = 360; r.setOptions({ resolutionScale: 1, exposure: 0 });
    for (let i = 0; i < 32; i++) await r.renderStep();
  });
  await page.screenshot({ path: path.join(out, 'chrome.png') });
  const referenceImages=[];
  if(process.argv.includes('--reference-images')) {
    const referencePresets=['native-copper','native-glass','sss','hair','thin-film','subsurface','normalmap','bump','coat','sheen','thin-walled','transmission-depth','generalized-schlick','open-pbr-weight','open-pbr-film','open-pbr-normal','opacity','displacement','normalmap-image'];
    const onlyPreset=process.argv.find(arg=>arg.startsWith('--only-preset='))?.slice('--only-preset='.length);
    const requestedReferenceSamples=Number(process.argv.find(arg=>arg.startsWith('--reference-samples='))?.slice('--reference-samples='.length)||32);
    const requestedReferenceMode=process.argv.find(arg=>arg.startsWith('--reference-mode='))?.slice('--reference-mode='.length)||'path-spectral';
    assert.ok(['path-spectral','path-physical','realtime'].includes(requestedReferenceMode),'reference mode must be path-spectral, path-physical, or realtime');
    assert.ok(Number.isInteger(requestedReferenceSamples)&&requestedReferenceSamples>0&&requestedReferenceSamples<=32,'reference samples must be 1..32');
    for(const preset of referencePresets.filter(name=>!onlyPreset||name===onlyPreset).filter(name=>name!=='normalmap-image'||requestedReferenceMode==='realtime')) {
      console.log(`reference-start preset=${preset} mode=${requestedReferenceMode} samples=${requestedReferenceSamples}`);
      const stats=await page.evaluate(async (preset,requestedReferenceSamples,requestedReferenceMode)=>{
        const r=window.__webgpuMtlx.renderer;const {syntheticScene}=await import('/src/webgpu-mtlx/scene.js');
        r.canvas.width=192;r.canvas.height=128;await r.loadScene(syntheticScene(preset));r.setMode(requestedReferenceMode);
        if(requestedReferenceMode==='realtime'){await r.renderStep();return {preset,samples:r.samples,dispatches:1};}
        let dispatches=0;while(r.samples<requestedReferenceSamples&&dispatches<4000){await r.renderStep();dispatches++;}
        if(r.samples!==requestedReferenceSamples)throw new Error(`${preset} did not converge to ${requestedReferenceSamples} spp`);
        const capture=await r.capture({format:'float32'});if(!capture.pixels.every(Number.isFinite))throw new Error(`${preset} has invalid radiance`);
        return {preset,samples:r.samples,dispatches};
      },preset,requestedReferenceSamples,requestedReferenceMode);
      referenceImages.push(stats);console.log(JSON.stringify(stats));await page.screenshot({path:path.join(out,`${preset}-${requestedReferenceMode}.png`)});
    }
  }
    let shaderballResult;
    if (shaderball) {
      await page.evaluate(async () => {
        const { validateUSDGraphSnapshot, validateUSDMaterialTranslation } = await import('/src/webgpu-mtlx/usd-graph-validation.js');
        await validateUSDGraphSnapshot();
        return validateUSDMaterialTranslation(window.__webgpuMtlx.renderer);
      });
    if(process.argv.includes('--authored-lights'))await page.click('#authored-lights');
    if(process.argv.includes('--authored-materials'))await page.click('#authored-materials');
    await page.select('#scene', 'shaderball');
    await page.waitForFunction(() => window.__webgpuMtlx.ready || window.__webgpuMtlx.errors.length, { timeout: 120000 });
    shaderballResult = await page.evaluate(async () => {
      const state = window.__webgpuMtlx;
      if (state.errors.length) return { errors: state.errors };
      if(state.renderer.mode==='path-physical') {
        const r=state.renderer;r.canvas.width=192;r.canvas.height=128;let dispatches=0;
        do{await r.renderStep();dispatches++;}while(r.samples<8&&dispatches<800);
        if(r.samples<8)throw new Error('Authored-light ShaderBall paths did not complete');
        const capture=await r.capture({format:'float32'});if(!capture.pixels.every(Number.isFinite))throw new Error('Invalid authored-light radiance');
      }
      await state.renderer.renderStep();
      const {fetchResource,decodeImage}=await import('/src/webgpu-mtlx/resources.js');
      const filename='/__assets/full_assets/StandardShaderBall/maps/neutral.ACEScg.exr';
      const image=await decodeImage(await fetchResource(filename),{filename,colorspace:'acescg'});
      if(!image.data.every(Number.isFinite))throw new Error('ShaderBall EXR contains non-finite values');
      return { stats: state.renderer.stats, provenance: state.renderer.scene.provenance, authored:state.renderer.sourceScene.authored, texture:{width:image.width,height:image.height},errors: state.errors };
    });
    assert.deepEqual(shaderballResult.errors, []); assert.ok(shaderballResult.stats.triangles > 1000);
    if(process.argv.includes('--authored-materials')) assert.ok(shaderballResult.provenance.authoredMaterialCount >= 1);
    assert.ok(Object.keys(shaderballResult.authored.materialPaths).length >= 2);
    assert.ok(shaderballResult.authored.bindings.every(binding => Array.isArray(binding.submeshes) && Number.isInteger(binding.materialId)));
    const sourceKeys = new Set(shaderballResult.authored.textureSources.map(entry => entry.authored));
    for (const prim of shaderballResult.authored.shadingGraph.prims) {
      for (const property of Object.values(prim.properties)) {
        if (property.type === 'asset' && property.value) assert.ok(sourceKeys.has(property.value), `Missing texture source: ${property.value}`);
      }
    }
    if(process.argv.includes('--authored-lights')){assert.equal(shaderballResult.provenance.lightingOverride,false);assert.equal(shaderballResult.provenance.rectLights.length,5);}
    if(process.argv.includes('--authored-materials')) {
      // The selected ShaderBall scene was already loaded with authoredMaterials
      // above. Re-loading it here doubles large texture fetch/decode cost and
      // can make the inspection gate appear hung; inspect the rendered source
      // scene instead.
      const translation = { translated: Object.keys(shaderballResult.authored.translatedMaterials || {}).length, compiled: Object.keys(shaderballResult.authored.compiledMaterials || {}).length, diagnostics: shaderballResult.authored.translationDiagnostics || [], textureDiagnostics: shaderballResult.authored.textureDiagnostics || [] };
      assert.ok(translation.translated + translation.diagnostics.length >= 1);
      assert.ok(translation.compiled <= translation.translated);
      shaderballResult.authoredTranslation = translation;
    }
    await page.screenshot({ path: path.join(out, 'shaderball.png') });
  }
  let performanceResult;
  if (process.argv.includes('--performance')) {
    performanceResult = await page.evaluate(async () => {
      const r = window.__webgpuMtlx.renderer; r.canvas.width = 1280; r.canvas.height = 720; r.setMode('realtime'); r.setOptions({ resolutionScale: 1, autoResolution: false });
      for (let i = 0; i < 30; i++) await r.renderStep();
      const times = [], start = performance.now();
      while (performance.now() - start < 30000) { await r.renderStep(); times.push(r.stats.milliseconds); }
      times.sort((a, b) => a - b);
      const png = await r.capture({ format: 'png' });
      return { width: 1280, height: 720, seconds: (performance.now() - start) / 1000, frames: times.length, median: times[Math.floor(times.length * 0.5)], p95: times[Math.floor(times.length * 0.95)], pngBytes: png.bytes.length };
    });
    assert.ok(performanceResult.pngBytes > 100); assert.ok(performanceResult.p95 <= 33.3, `720p frame p95 ${performanceResult.p95} exceeds target`);
  }
  const finalErrors=await page.evaluate(()=>[...window.__webgpuMtlx.errors,...window.__webgpuMtlx.renderer.errors]);assert.deepEqual(finalErrors,[]);assert.deepEqual(errors,[]);
  const report = { browser: await browser.version(), requestedHardware: hardware, inventoriedNodeDefs: inventory.length, shaderball: shaderballResult, performance: performanceResult,referenceImages, ...results };
  fs.writeFileSync(path.join(out, 'chrome.json'), JSON.stringify(report, null, 2));
  console.log(JSON.stringify({ browser: report.browser, adapter: results.adapter, inventoriedNodeDefs: inventory.length, numericPassed: results.numeric.length, samples: results.samples, shaderball: shaderballResult?{stats:shaderballResult.stats,provenance:shaderballResult.provenance,texture:shaderballResult.texture}:undefined, performance: performanceResult, errors: results.errors, report: path.relative(root, path.join(out, 'chrome.json')) }, null, 2));
} catch(e) {
  const state=await page?.evaluate(()=>({url:location.href,status:document.getElementById('status')?.textContent,ready:window.__webgpuMtlx?.ready,errors:window.__webgpuMtlx?.errors})).catch(()=>null);
  console.error(JSON.stringify({failure:e.message,state,browserLog,serverLog:log.slice(-3000)},null,2));throw e;
} finally { await browser?.close(); server.kill(); }
