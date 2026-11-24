// SPDX-License-Identifier: Apache-2.0
// Feature factories are imported by name at runtime, including inside Workers.
// Keep complete JS/WASM distributions beside the generated application chunks.
import path from 'node:path';
import {access,copyFile,mkdir} from 'node:fs/promises';
async function present(filename) {
  try {await access(filename);return true;}
  catch(error){if(error.code==='ENOENT')return false;throw error;}
}
export function copyLocalWasmProducts(sourceDir,{includeMCP=false}={}) {
  return {name:'copy-local-wasm-products',apply:'build',async writeBundle(output) {
    const assetsDir=path.join(output.dir || 'dist','assets');
    await mkdir(assetsDir,{recursive:true});
    const names=['lightusd','lightusd_geometry','lightusd_raytrace','lightusd_validator','lightusd_exr'];
    const copyPair=async name=>{
      for(const extension of ['js','wasm']) {
        const filename=`${name}.${extension}`;
        await copyFile(path.join(sourceDir,filename),path.join(assetsDir,filename));
      }
    };
    for(const name of names)await copyPair(name);
    // A built memory64 core requires its matching optional feature factories.
    // Missing halves fail the build instead of producing runtime 404s.
    if(await present(path.join(sourceDir,'lightusd_64.js')) ||
       await present(path.join(sourceDir,'lightusd_64.wasm')))
      for(const name of names)await copyPair(name+'_64');
    if(includeMCP) {
      await copyPair('lightusd_mcp');
      if(await present(path.join(sourceDir,'lightusd_mcp_64.js')) ||
         await present(path.join(sourceDir,'lightusd_mcp_64.wasm')))
        await copyPair('lightusd_mcp_64');
    }
  }};
}
