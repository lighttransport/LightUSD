// SPDX-License-Identifier: Apache-2.0
import { loadMaterialXResources } from './resources.js';
import { shaderSource } from './shaders.js';
import { encodeEXR } from './capture.js';

export async function validateResourceLoading() {
  const files=new Map([
    ['https://fixtures.test/root.mtlx',`<materialx version="1.39" xmlns:xi="http://www.w3.org/2001/XInclude">
      <xi:include href="layers/library.mtlx"/>
      <surfacematerial name="material" type="material"><input name="surfaceshader" type="surfaceshader" nodename="surface"/></surfacematerial>
      <surface name="surface" type="surfaceshader"><input name="bsdf" type="BSDF" nodename="diffuse"/></surface>
      <oren_nayar_diffuse_bsdf name="diffuse" type="BSDF"><input name="color" type="color3" nodegraph="colorGraph" output="out"/></oren_nayar_diffuse_bsdf>
    </materialx>`],
    ['https://fixtures.test/layers/library.mtlx',`<materialx version="1.39"><nodegraph name="colorGraph" fileprefix="../maps/" colorspace="acescg">
      <image name="image" type="color3"><input name="file" type="filename" value="color.exr"/></image>
      <output name="out" type="color3" nodename="image"/>
    </nodegraph></materialx>`],
    ['https://fixtures.test/maps/color.exr',encodeEXR(1,1,new Float32Array([.5,.5,.5,1]))],
  ]);
  const fetched=[];
  const fetcher=async url=>{fetched.push(url);return new Response(files.get(url)||'',{status:files.has(url)?200:404});};
  const doc=await loadMaterialXResources('https://fixtures.test/root.mtlx',{fetcher});
  const resources={};shaderSource([doc],resources);
  if(doc.output.nodename!=='material'||Object.keys(doc.images).length!==1||!fetched.includes('https://fixtures.test/maps/color.exr'))throw new Error('Included material resource resolution failed');
  if(Math.abs(resources.imageData[0]-.5)>1e-6)throw new Error('Inherited image colorspace failed');
  files.set('https://fixtures.test/layers/library.mtlx','<materialx xmlns:xi="http://www.w3.org/2001/XInclude"><xi:include href="../root.mtlx"/></materialx>');
  let rejected=false;try{await loadMaterialXResources('https://fixtures.test/root.mtlx',{fetcher});}catch(e){rejected=/cycle/.test(e.message);}if(!rejected)throw new Error('Include cycle was not rejected');
  files.set('https://fixtures.test/layers/library.mtlx','<materialx xmlns:xi="http://www.w3.org/2001/XInclude"><xi:include href="https://outside.test/root.mtlx"/></materialx>');
  rejected=false;try{await loadMaterialXResources('https://fixtures.test/root.mtlx',{fetcher});}catch(e){rejected=/origin/.test(e.message);}if(!rejected||fetched.some(url=>url.startsWith('https://outside.test')))throw new Error('Cross-origin include was fetched');
  return {includedDocuments:2,textures:1,cycleRejected:true,crossOriginRejected:true};
}
