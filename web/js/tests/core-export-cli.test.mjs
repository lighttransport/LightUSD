// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import createCore from '../src/lightusd/lightusd.js';
import {parseUSDZEntries} from '../src/usdzconvert.js';
const temp=fs.mkdtempSync(path.join(os.tmpdir(),'lightusd-export-cli-'));
const native=await createCore();
const cli=new URL('../cli/usd-export-cli.js',import.meta.url).pathname;
const run=args=>spawnSync(process.execPath,[cli,...args],{encoding:'utf8',timeout:60000});
try {
 const sample=path.join(temp,'sample');
 const created=run(['--sample','--format','all','-o',sample]);
 assert.equal(created.status,0,created.stderr);assert.match(created.stdout,/Wrote/);
 const reloaded=run([sample+'.usdz','--format','all','-o',path.join(temp,'reload')]);
 assert.equal(reloaded.status,0,reloaded.stderr);assert.match(reloaded.stdout,/Wrote/);
 for(const base of ['sample','reload'])for(const ext of ['usda','usdc','usdz']) {
  let bytes=new Uint8Array(fs.readFileSync(path.join(temp,base+'.'+ext)));
  if(ext==='usdz')bytes=parseUSDZEntries(bytes)[0].data;
  const document=new native.LayerDocument();
  try{assert.equal(document.load(bytes).success,true);assert.ok(document.exportUSDA().text.includes('Mesh'));}
  finally{document.delete();}
 }
 assert.notEqual(run([path.join(temp,'missing.usda')]).status,0);
 assert.notEqual(run(['--sample','--format','bad']).status,0);
 console.log('PASS core export CLI sample, USDA/USDC/USDZ roundtrip and errors');
}finally{fs.rmSync(temp,{recursive:true,force:true});}
