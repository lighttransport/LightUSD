import assert from 'node:assert/strict';

export async function runConcurrencyBrowserChecks(page) {
  const results=await page.evaluate(async()=>{
    const {app}=await import('/lucia-code/src/main.js');
    const check=(value,message)=>{if(!value)throw new Error(message);};
    const layer=name=>`#usda 1.0\ndef Xform "${name}" {}`;
    const slowFile=name=>{
      let resolve;
      return {file:{name:`${name}.usda`,size:64,arrayBuffer:()=>new Promise(done=>{resolve=done;})},
        finish:()=>resolve(new TextEncoder().encode(layer(name)).buffer)};
    };
    const old=slowFile('Old'),oldRead=app.openFile(old.file);
    await app.openFile(new File([layer('Latest')],'Latest.usda'));
    old.finish();await oldRead;
    check(app.session.filename==='Latest.usda' && app.session.exportUSDA().includes('"Latest"'),'Slow import overwrote newer import');
    const pending=slowFile('Pending'),pendingRead=app.openFile(pending.file);
    await app.newProject('empty');pending.finish();await pendingRead;
    check(app.session.filename==='empty.usda','Slow import overwrote New Project');
    const superseded=app.newProject('room'),winner=app.newProject('empty');
    check(await superseded===false && await winner===true,'Superseded New Project committed after acquiring its lease');
    await app.runMutation('Seed history',[],()=>app.session.replaceUSDA(layer('Edited')));
    const before=app.session.exportUSDA(),count=app.commands.undoItems.length;
    const mutation=app.runMutation('Cancelled edit',[],()=>new Promise((resolve,reject)=>{
      app.operations.worker={terminate(){}};app.operations.workerReject=reject;
    }));
    for(let i=0;i<20 && !app.operations.worker;i++)await Promise.resolve();
    check(app.operations.worker,'Edit did not start');
    await app.restoreHistory('undo');
    check(app.commands.undoItems.length===count && app.session.exportUSDA()===before,'Busy undo changed scene or history');
    const replacing=app.newProject('materials');
    check(await mutation===false,'Cancelled edit reported success');
    await replacing;
    check(app.session.filename==='materials.usda' && !app.commands.canUndo && !app.activity.active,`Replacement raced with edit rollback: ${JSON.stringify({filename:app.session.filename,undo:app.commands.undoItems.length,active:!!app.activity.active,pending:app.activity.pending,error:app.$('#messages').textContent.slice(-700)})}`);
    await app.runMutation('Seed rollback history',[],()=>app.session.replaceUSDA(layer('Retained')));
    app.project.assets.set('retained.bin',{bytes:Uint8Array.of(1,2,3)});
    const retained=app.session.exportUSDA(),revision=app.project.revision,undoCount=app.commands.undoItems.length;
    const load=app.session.loadUSDA.bind(app.session);
    app.session.loadUSDA=async(source,name)=>{if(name==='room.usda')throw new Error('Injected creation failure');return load(source,name);};
    try {check(await app.newProject('room')===false,'Failed New Project reported success');}
    finally {app.session.loadUSDA=load;}
    check(app.session.exportUSDA()===retained && app.project.revision===revision && app.project.assets.has('retained.bin') && app.commands.undoItems.length===undoCount,'Failed New Project lost the working state');
    await app.restoreHistory('undo');await app.restoreHistory('redo');
    check(app.session.exportUSDA()===retained && !app.activity.active && !app.activity.pending,'History or activity gate was left stuck');
    check(await app.newProject('room')===true && app.session.exportUSDA().includes('"BackWall"'),'Room template failed to load');
    const validate=app.session.validate,confirm=app.confirmAction,click=HTMLAnchorElement.prototype.click;
    let finishValidation,downloads=0;
    try {
      app.session.validate=()=>new Promise(resolve=>{finishValidation=resolve;});
      const validating=app.runValidation(),replacement=app.newProject('empty');
      finishValidation({ok:true,issues:[]});
      check(await validating===null,'Superseded validation reported success');
      await replacement;
      check(app.validation.label!=='Valid USD' && !app.activity.active,'Old validation overwrote the new scene');
      app.confirmAction=async()=>true;
      HTMLAnchorElement.prototype.click=function(){downloads++;};
      for(const result of [{ok:false,issues:[]},{parse_ok:false,error:'Invalid layer'},{}]) {
        app.session.validate=async()=>result;
        await app.exportDialog('usda');
        check(app.validation.label!=='Valid USD','Failed validation marked the scene valid');
      }
      app.session.validate=async()=>{throw new Error('Injected validation failure');};
      await app.exportDialog('usda');
      check(downloads===0 && !app.activity.active,'Export continued after failed validation');
      app.session.validate=async()=>({ok:true,issues:[{severity:'warning',message:'Optional metadata missing'}]});
      await app.exportDialog('usda');
      check(downloads===1,'Warning-only validation blocked export');
    } finally {
      app.session.validate=validate;app.confirmAction=confirm;HTMLAnchorElement.prototype.click=click;
    }
    return {latestImport:true,newProject:true,rollback:true,history:true,validation:true};
  });
  assert.ok(Object.values(results).every(Boolean));
  console.log('Lucia concurrency browser: latest import, New Project, worker rollback, busy history and validation/export passed');
}
