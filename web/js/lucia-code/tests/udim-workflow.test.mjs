import test from 'node:test';
import assert from 'node:assert/strict';
import {LuciaUDIMWorkflow, UDIM_DEFAULTS, safeAssetPath, stageAssetFiles, udimInventory, validateUDIMPreview, selectUDIMSites} from '../src/udim-workflow.js';
import {LuciaCommandStack, sessionCommand, estimateCommandBytes, estimateHistoryBytes} from '../src/command-stack.js';
if (!globalThis.CustomEvent) globalThis.CustomEvent = class extends Event { constructor(type,options={}) { super(type); this.detail=options.detail; } };
const bytes = (...v) => Uint8Array.from(v);
const sites=[{path:'/Mat/A',pattern:'tex.<UDIM>.png',time:null},{path:'/Mat/A',pattern:'anim.%04d.png',time:1},{path:'/Mat/B',pattern:'b.%(UDIM)d.png',time:null}];
const options={...UDIM_DEFAULTS,udimShaderPaths:['/Mat/A']};
const result=()=>({usda:'#usda 1.0\ndef Xform "World" {}',paths:['/Mat/A'],tiles:2,assets:[{name:'atlas.png',data:bytes(1,2),layout:{width:4,height:2}}]});
function fixture() {
 const project={revision:0,domainRevisions:{usd:0,assets:0},assets:new Map([['tex.1001.png',{bytes:bytes(1)}]]),exportRemap:{},emit(){}};
 let source='#usda 1.0\ndef Xform "Before" {}';
 const observed=[];
 const session={filename:'root.usda',exportUSDA:()=>source,replaceUSDA:async value=>{source=value;observed.push([...project.assets.keys()]);},restore:async value=>{source=value;observed.push([...project.assets.keys()]);}};
 const owner={cancel(){},lastStats:null};
 const workflow=new LuciaUDIMWorkflow(session,project,owner);
 workflow.request=async()=>result();
 return {project,session,owner,workflow,observed};
}
test('selection includes every animated opinion and validates unknown/duplicate paths',()=>{
 assert.deepEqual(selectUDIMSites(sites,['/Mat/A']),sites.slice(0,2));
 assert.equal(selectUDIMSites(sites,undefined),sites);
 for(const selection of [[],['/Missing'],['/Mat/A','/Mat/A'],['/Mat/A',3]]) assert.throws(()=>selectUDIMSites(sites,selection),/selection/);
 const inventory=udimInventory(sites,['tex.1001.png','tex.1101.png','anim.1002.png']);
 assert.deepEqual(inventory[0].ids,[1001,1002,1101]);assert.match(inventory[1].error,/no tiles/);
});
test('asset loading validates paths, collisions, and remains atomic',()=>{
 const original=new Map([['tex.png',{bytes:bytes(1)}]]);
 assert.throws(()=>stageAssetFiles(original,[{name:'other.png',bytes:bytes(2)},{name:'tex.png',bytes:bytes(3)}]),/Different assets/);
 assert.deepEqual([...original.keys()],['tex.png']);
 for(const path of ['/a','../a','a/../b','C:/a','a\\b','a//b','']) assert.throws(()=>safeAssetPath(path));
 assert.equal(safeAssetPath('./textures/t.png'),'textures/t.png');
 assert.equal(stageAssetFiles(original,[{name:'tex.png',bytes:bytes(1)}]).size,1);
});
test('worker result rejects invalid atlas data and retention exceeding budget',()=>{
 assert.equal(validateUDIMPreview(result(),options).tiles,2);
 const invalid=result(); invalid.assets[0].layout.width=99999;
 assert.throws(()=>validateUDIMPreview(invalid,options),/invalid atlas/);
 const duplicate=result(); duplicate.assets.push({...duplicate.assets[0]});
 assert.throws(()=>validateUDIMPreview(duplicate,options),/invalid atlas/);
 assert.throws(()=>validateUDIMPreview(result(),{...options,udimMemoryBudgetBytes:1}),/memory budget/);
});
test('preview leaves source and project untouched, apply/undo/redo restore assets before render',async()=>{
 const {project,session,workflow,observed}=fixture(); const before=session.exportUSDA();
 await workflow.bake(options);assert.equal(session.exportUSDA(),before);assert.equal(project.assets.size,1);
 const stack=new LuciaCommandStack();
 assert.equal(await stack.execute(sessionCommand(session,'UDIM',['/Mat/A'],()=>workflow.apply(options),project)),true);
 assert.equal(project.assets.size,2);assert.equal(stack.undoItems.length,1);
 await stack.undo();assert.equal(session.exportUSDA(),before);assert.equal(project.assets.size,1);assert.deepEqual(observed.at(-1),['tex.1001.png']);
 await stack.redo();assert.equal(project.assets.size,2);assert.ok(observed.at(-1).includes('atlas.png'));
});
test('stale scene, asset and option previews cannot apply',async()=>{
 for(const mutate of [f=>f.project.revision++,f=>f.project.assets.set('new.png',{bytes:bytes(4)}),f=>f.session.replaceUSDA('#usda changed')]) {
  const f=fixture();await f.workflow.bake(options);await mutate(f);await assert.rejects(()=>f.workflow.apply(options),/stale/);assert.equal(f.project.assets.has('atlas.png'),false);
 }
 const f=fixture();await f.workflow.bake(options);await assert.rejects(()=>f.workflow.apply({...options,udimMaxTiles:1}),/stale/);
});
test('detection rejects results when the source or tile registry changed',async()=>{
 for(const mutate of [f=>f.project.revision++,f=>f.project.assets.set('late.png',{bytes:bytes(1)})]) {
  const f=fixture(); let resolve;
  f.workflow.request=()=>new Promise(done=>{resolve=done;});
  const pending=f.workflow.inspect();mutate(f);resolve({inventory:[]});
  await assert.rejects(pending,e=>e.code==='LUCIA_UDIM_STALE');
 }
});
test('failed/cancelled preview and failed apply restore all project state',async()=>{
 const f=fixture(), before=f.session.exportUSDA();f.workflow.request=async()=>{throw Object.assign(new Error('cancelled'),{code:'LUCIA_CANCELLED'});};
 await assert.rejects(()=>f.workflow.bake(options),/cancelled/);assert.equal(f.workflow.preview,null);assert.equal(f.session.exportUSDA(),before);
 f.workflow.request=async()=>result();await f.workflow.bake(options);f.session.replaceUSDA=async()=>{throw new Error('conversion failed');};
 const stack=new LuciaCommandStack();await assert.rejects(()=>stack.execute(sessionCommand(f.session,'UDIM',[],()=>f.workflow.apply(options),f.project)),/conversion failed/);
 assert.equal(f.project.assets.size,1);assert.equal(stack.canUndo,false);
});
test('history counts buffers once per backing store and rejects oversized undoable edits',async()=>{
 const buffer=new ArrayBuffer(64), command={projectBefore:{assets:new Map([['x',{bytes:new Uint8Array(buffer),other:new Uint16Array(buffer)}]])}};
 assert.ok(estimateCommandBytes(command)>=64);assert.ok(estimateCommandBytes(command)<80);
 const f=fixture(),before=f.session.exportUSDA();await f.workflow.bake(options);
 const stack=new LuciaCommandStack({maxBytes:2});await assert.rejects(()=>stack.execute(sessionCommand(f.session,'UDIM',[],()=>f.workflow.apply(options),f.project)),/memory budget/);
 assert.equal(f.session.exportUSDA(),before);assert.equal(f.project.assets.size,1);
});
test('undo invoked during an active command cannot remove history',async()=>{
 const stack=new LuciaCommandStack();stack.undoItems=[{before:'x'}];stack.busy=true;await stack.undo();assert.equal(stack.undoItems.length,1);
});
test('unchanged asset snapshots share storage and in-place changes preserve older undo states',async()=>{
 const f=fixture(),stack=new LuciaCommandStack({maxBytes:1048576});
 const edit=()=>stack.execute(sessionCommand(f.session,'Edit',[],async()=>{},f.project));
 await edit();await edit();
 const buffer=stack.undoItems[0].projectBefore.assets.get('tex.1001.png').bytes.buffer;
 assert.equal(stack.undoItems[1].projectAfter.assets.get('tex.1001.png').bytes.buffer,buffer);
 assert.ok(estimateHistoryBytes(stack.undoItems)<stack.undoItems.reduce((n,c)=>n+estimateCommandBytes(c),0));
 f.project.assets.get('tex.1001.png').bytes[0]=9;
 await edit();
 assert.notEqual(stack.undoItems[2].projectBefore.assets.get('tex.1001.png').bytes.buffer,buffer);
 assert.equal(new Uint8Array(buffer)[0],1);
 await stack.undo();await stack.undo();
 assert.equal(f.project.assets.get('tex.1001.png').bytes[0],1);
 f.project.assets.get('tex.1001.png').bytes[0]=7;
 assert.equal(new Uint8Array(buffer)[0],1);
});
test('history budget counts shared buffers once across commands',async()=>{
 const f=fixture(),stack=new LuciaCommandStack({maxBytes:4000,maxCommands:5});
 f.project.assets.set('large',{bytes:new Uint8Array(3000)});
 for(let i=0;i<4;i++)await stack.execute(sessionCommand(f.session,'Edit',[],async()=>{},f.project));
 assert.equal(stack.undoItems.length,4);assert.ok(estimateHistoryBytes(stack.undoItems)<=4000);
 stack.clear();assert.equal(estimateHistoryBytes(stack.undoItems),0);
});
test('worker protocol bounds tile transfers and releases failed workers',async()=>{
 const previousWorker=globalThis.Worker;
 let worker;
 globalThis.Worker=class {
  constructor(){worker=this;this.messages=[];this.terminated=false;}
  postMessage(message){this.messages.push(message);}
  terminate(){this.terminated=true;}
 };
 try {
  const f=fixture();
  f.owner.releaseWorker=function(){this.worker?.terminate();this.worker=null;this.workerReject=null;};
  f.owner.cancel=function(){this.workerReject?.(new Error('cancelled'));this.releaseWorker();};
  const request=(...args)=>LuciaUDIMWorkflow.prototype.request.call(f.workflow,...args);
  const pending=request('bake',options);
  worker.onmessage({data:{type:'fetch',id:1,key:'tex.1001.png',maxBytes:10}});
  const transferred=worker.messages.at(-1).bytes;
  assert.deepEqual(transferred,f.project.assets.get('tex.1001.png').bytes);
  assert.notEqual(transferred.buffer,f.project.assets.get('tex.1001.png').bytes.buffer);
  worker.onmessage({data:{type:'fetch',id:2,key:'tex.1001.png',maxBytes:options.udimMemoryBudgetBytes+1}});
  await assert.rejects(pending,/oversized tile/);
  assert.ok(worker.terminated);assert.equal(f.owner.worker,null);
  const unknown=request('inspect',options);
  worker.onmessage({data:{type:'unexpected'}});
  await assert.rejects(unknown,/Unexpected/);assert.ok(worker.terminated);
  await assert.rejects(request('inspect',options,()=>{throw new Error('progress failed');}),/progress failed/);
  assert.ok(worker.terminated);assert.equal(f.owner.workerReject,null);
 } finally {
  if(previousWorker===undefined) delete globalThis.Worker;
  else globalThis.Worker=previousWorker;
 }
});
