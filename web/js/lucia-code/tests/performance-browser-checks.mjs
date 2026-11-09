import assert from 'node:assert/strict';
export async function runPerformanceBrowserChecks(page) {
  const result=await page.evaluate(async()=>{
    const {app}=await import('/lucia-code/src/main.js');
    const n=256,points=[],indices=[];
    for(let y=0;y<=n;y++)for(let x=0;x<=n;x++)points.push(`(${x/n},0,${y/n})`);
    for(let y=0;y<n;y++)for(let x=0;x<n;x++){
      const a=y*(n+1)+x,b=a+1,c=a+n+1,d=c+1;indices.push(a,c,b,b,c,d);
    }
    const source=`#usda 1.0\n(defaultPrim="World")\ndef Xform "World" { def Mesh "Grid" {
      point3f[] points=[${points.join(',')}]
      int[] faceVertexCounts=[${Array(n*n*2).fill(3).join(',')}]
      int[] faceVertexIndices=[${indices.join(',')}]
      uniform token subdivisionScheme="none"
    }}`;
    const loadStart=performance.now();
    await app.openFile(new File([source],'performance.usda'));
    const importMs=performance.now()-loadStart;
    const rebuilds=[];
    for(let i=0;i<2;i++){const start=performance.now();await app.refreshAll(false);rebuilds.push(performance.now()-start);}
    let triangles=0;
    app.bridge.content.traverse(object=>{if(object.isMesh)triangles+=(object.geometry.index?.count || object.geometry.attributes.position.count)/3;});
    let heartbeats=0;
    const timer=setInterval(()=>heartbeats++,10),analysisStart=performance.now();
    try { await app.showHealthReport(); } finally { clearInterval(timer); }
    const analysisMs=performance.now()-analysisStart;
    if(!app.assetReport || app.assetReport.report.stage.triangles!==triangles)throw new Error('Worker Health report lost geometry');
    if(!heartbeats)throw new Error('Health analysis blocked the UI event loop');
    return {workload:'131072-triangle USDA grid',sourceBytes:new TextEncoder().encode(source).length,importMs,rebuildMs:rebuilds,triangles,analysisMs,heartbeats};
  });
  assert.equal(result.triangles,131072);
  assert.ok(result.rebuildMs.every(Number.isFinite));
  console.log(`Lucia viewport performance: ${JSON.stringify(result)}`);
}
