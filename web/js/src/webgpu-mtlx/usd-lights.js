// SPDX-License-Identifier: Apache-2.0
import { Matrix4, Matrix3, Vector3 } from 'three';

/** UsdLux radiance = color * intensity * 2^exposure / normalized world area.
 * https://openusd.org/dev/user_guides/schemas/usdLux/LightAPI.html
 * Rectangles emit along local -Z. Unsupported nonphysical controls fail closed.
 */
export function appendRectLights(scene, lights) {
  const result={...scene,positions:Array.from(scene.positions),normals:Array.from(scene.normals||[]),uvs:Array.from(scene.uvs||[]),uvSets:(scene.uvSets||[]).map(values=>Array.from(values||[])),tangents:Array.from(scene.tangents||[]),colors:Array.from(scene.colors||[]),indices:Array.from(scene.indices),materialIds:Array.from(scene.materialIds||new Array(scene.indices.length/3).fill(0)),geompropSets:Object.fromEntries(Object.entries(scene.geompropSets||{}).map(([name,values])=>[name,Array.from(values||[])])),materials:scene.materials.slice()};
  if(result.normals.length!==result.positions.length||result.uvs.length!==result.positions.length/3*2)throw new Error('Light conversion requires scene normals and UVs');
  if(!result.colors.length)result.colors=new Array(result.positions.length/3*4).fill(0).map((v,i)=>i%4===3?1:v);
  else if(result.colors.length===result.positions.length/3*3){const rgb=result.colors;result.colors=[];for(let i=0;i<rgb.length;i+=3)result.colors.push(rgb[i],rgb[i+1],rgb[i+2],1);}
  if(result.colors.length!==result.positions.length/3*4)throw new Error('Light conversion color count mismatch');
  const imported=[];
  const areaLights=[];
  let distant = null;
  const environment=[0,0,0];
  const domes=[];
  let environmentTexture=null;
  const texturedDomes=[];
  const points=[];
  const disks=[];
  const cylinders=[];
  const geometryLights=[];
  for(const light of lights) {
    const lightType=String(light.type||'').toLowerCase().replace(/light$/,'');
    if(lightType==='dome') {
      if(Number.isInteger(light.envmapTextureId)&&light.envmapTextureId>=0||light.enableColorTemperature)throw new Error('Environment-map-ID or temperature-controlled dome lights are unsupported');
      if(light.textureFile&&!light.textureImage)throw new Error('Textured dome requires a decoded environment image');
      const {intensity=1,exposure=0,color=[1,1,1]}=light;
      if(![intensity,exposure,...color].every(Number.isFinite)||intensity<0||color.length!==3||color.some(c=>c<0))throw new Error('Invalid dome light parameters');
      const radiance=color.map(c=>c*intensity*2**exposure);
      if(!radiance.every(Number.isFinite))throw new Error('Invalid dome light radiance');
      if(light.textureImage) {
        environmentTexture={...light.textureImage,colorspace:light.textureImage.colorspace||'lin_rec709',scale:radiance};
        texturedDomes.push({image:light.textureImage,radiance});
        domes.push({path:light.absPath,radiance,texture:true});
      } else {
        radiance.forEach((v,i)=>environment[i]+=v);domes.push({path:light.absPath,radiance});
      }
      continue;
    }
    if(lightType==='distant') {
      if(light.textureFile||light.enableColorTemperature||light.shapingIesFile||light.shapingFocus>0||light.shapingConeAngle<90||light.diffuse!==undefined&&light.diffuse!==1||light.specular!==undefined&&light.specular!==1||light.shadowEnable===false)throw new Error('Unsupported distant light texture, shaping, temperature, or contribution controls');
      const {intensity=1,exposure=0,color=[1,1,1]}=light;
      const direction=Array.isArray(light.direction)&&light.direction.length===3?light.direction:null;
      if(!direction||!direction.every(Number.isFinite)||Math.hypot(...direction)<1e-8||![intensity,exposure,...color].every(Number.isFinite)||intensity<0||color.length!==3||color.some(c=>c<0))throw new Error('Invalid distant light parameters');
      const length=Math.hypot(...direction),towardLight=direction.map(v=>v===0?0:-v/length),radiance=color.map(c=>c*intensity*2**exposure);
      if(!radiance.every(Number.isFinite))throw new Error('Invalid distant light radiance');
      if(distant && (distant.direction.some((v,i)=>Math.abs(v-towardLight[i])>1e-6)||distant.radiance.some((v,i)=>Math.abs(v-radiance[i])>1e-6)))throw new Error('Multiple nonmatching distant lights are unsupported');
      distant={direction:towardLight,radiance};continue;
    }
    if(lightType==='point'||lightType==='sphere') {
      if(light.textureFile||light.enableColorTemperature||light.shapingIesFile||light.shapingFocus>0||light.shapingConeAngle<90||light.diffuse!==undefined&&light.diffuse!==1||light.specular!==undefined&&light.specular!==1||light.shadowEnable===false)throw new Error(`Unsupported ${lightType} light texture, shaping, temperature, or contribution controls`);
      const {intensity=1,exposure=0,color=[1,1,1]}=light;
      const transform=light.transform;
      const position=light.position ?? (Array.isArray(transform)&&transform.length===16 ? [transform[12],transform[13],transform[14]] : null);
      const radius=light.radius===undefined?(lightType==='sphere'?0.5:0.01):Number(light.radius);
      if(!Array.isArray(position)||position.length!==3||!position.every(Number.isFinite)||![radius,intensity,exposure,...color].every(Number.isFinite)||radius<=0||intensity<0||color.length!==3||color.some(c=>c<0))throw new Error(`Invalid ${lightType} light parameters; radius must be positive`);
      const area=4*Math.sqrt(3)*radius*radius,radiance=color.map(c=>c*intensity*2**exposure/area);
      if(!radiance.every(Number.isFinite))throw new Error('Invalid point light radiance');
      const offset=result.positions.length/3,material=result.materials.length,verts=[[1,0,0],[-1,0,0],[0,1,0],[0,-1,0],[0,0,1],[0,0,-1]].map(v=>v.map(c=>c*radius)),faces=[[0,2,4],[2,1,4],[1,3,4],[3,0,4],[2,0,5],[1,2,5],[3,1,5],[0,3,5]];
      result.materials.push({twoSidedEmission:true,nodes:[{name:'emission',category:'uniform_edf',type:'EDF',inputs:{color:{type:'color3',value:radiance}}},{name:'surface',category:'surface',type:'surfaceshader',inputs:{edf:{nodename:'emission'}}}]});
      for(const v of verts){const n=v.map(c=>c/radius);result.positions.push(position[0]+v[0],position[1]+v[1],position[2]+v[2]);result.normals.push(...n);result.uvs.push(0,0);result.colors.push(0,0,0,1);}
      for(const face of faces){result.indices.push(...face.map(i=>i+offset));result.materialIds.push(material);}
      points.push({path:light.absPath,type:lightType,position:[...position],radius,worldArea:area,radiance,materialId:material});continue;
    }
    if(lightType==='spot') {
      if(light.textureFile||light.enableColorTemperature||light.shapingIesFile||light.shapingFocus>0||light.diffuse!==undefined&&light.diffuse!==1||light.specular!==undefined&&light.specular!==1||light.shadowEnable===false)throw new Error('Unsupported spot light texture, shaping, temperature, focus, or contribution controls');
      const {intensity=1,exposure=0,color=[1,1,1]}=light;
      if(!Array.isArray(light.transform)||light.transform.length!==16||!light.transform.every(Number.isFinite)||light.transform[3]!==0||light.transform[7]!==0||light.transform[11]!==0||light.transform[15]!==1||![intensity,exposure,...color].every(Number.isFinite)||intensity<0||color.length!==3||color.some(c=>c<0))throw new Error('Invalid spot light parameters');
      const matrix=new Matrix4().fromArray(light.transform);if(Math.abs(matrix.determinant())<1e-15)throw new Error('Singular spot light transform');
      const position=[matrix.elements[12],matrix.elements[13],matrix.elements[14]],direction=new Vector3(0,0,-1).transformDirection(matrix).normalize();
      const outerAngle=Number(light.angle)*180/Math.PI,softness=Number(light.shapingConeSoftness||0),innerAngle=outerAngle*Math.max(0,1-Math.min(1,softness));
      const radius=light.radius===undefined?.01:Number(light.radius),area=4*Math.sqrt(3)*radius*radius;
      if(!Number.isFinite(outerAngle)||outerAngle<=0||outerAngle>=180||!Number.isFinite(radius)||radius<=0||!Number.isFinite(softness)||softness<0||softness>1)throw new Error('Invalid spot light cone or radius');
      const radiance=color.map(c=>c*intensity*2**exposure/area),offset=result.positions.length/3,material=result.materials.length;
      result.materials.push({twoSidedEmission:false,nodes:[{name:'emission',category:'conical_edf',type:'EDF',inputs:{color:{type:'color3',value:radiance},normal:{type:'vector3',value:direction.toArray()},inner_angle:{type:'float',value:innerAngle},outer_angle:{type:'float',value:outerAngle}}},{name:'surface',category:'surface',type:'surfaceshader',inputs:{edf:{nodename:'emission'}}}]});
      const verts=[[1,0,0],[-1,0,0],[0,1,0],[0,-1,0],[0,0,1],[0,0,-1]].map(v=>v.map(c=>c*radius)),faces=[[0,2,4],[2,1,4],[1,3,4],[3,0,4],[2,0,5],[1,2,5],[3,1,5],[0,3,5]];
      for(const v of verts){const n=v.map(c=>c/radius);result.positions.push(position[0]+v[0],position[1]+v[1],position[2]+v[2]);result.normals.push(...n);result.uvs.push(0,0);result.colors.push(0,0,0,1);}
      for(const face of faces){result.indices.push(...face.map(i=>i+offset));result.materialIds.push(material);}
      points.push({path:light.absPath,type:lightType,position,radius,worldArea:area,radiance,materialId:material,coneDirection:direction.toArray(),coneInnerCos:Math.cos(innerAngle*Math.PI/180),coneOuterCos:Math.cos(outerAngle*Math.PI/180)});continue;
    }
    if(lightType==='disk') {
      if(light.textureFile||light.enableColorTemperature||light.shapingIesFile||light.shapingFocus>0||light.shapingConeAngle<90||light.diffuse!==undefined&&light.diffuse!==1||light.specular!==undefined&&light.specular!==1||light.shadowEnable===false)throw new Error('Unsupported disk light texture, shaping, temperature, or contribution controls');
      const {intensity=1,exposure=0,color=[1,1,1]}=light,radius=light.radius===undefined?1:Number(light.radius);
      if(![radius,intensity,exposure,...color].every(Number.isFinite)||radius<=0||intensity<0||color.length!==3||color.some(c=>c<0)||!Array.isArray(light.transform)||light.transform.length!==16||!light.transform.every(Number.isFinite)||light.transform[3]!==0||light.transform[7]!==0||light.transform[11]!==0||light.transform[15]!==1)throw new Error('Invalid disk light parameters');
      const matrix=new Matrix4().fromArray(light.transform);if(Math.abs(matrix.determinant())<1e-15)throw new Error('Singular disk light transform');
      const segments=16,local=[new Vector3(0,0,0)],vertices=[];for(let i=0;i<segments;i++)local.push(new Vector3(radius*Math.cos(i*2*Math.PI/segments),radius*Math.sin(i*2*Math.PI/segments),0));for(const p of local)vertices.push(p.applyMatrix4(matrix));
      const normal=new Vector3(0,0,-1).applyMatrix3(new Matrix3().getNormalMatrix(matrix)).normalize();
      let area=0;for(let i=1;i<vertices.length;i++){const next=i===vertices.length-1?1:i+1;area+=new Vector3().subVectors(vertices[i],vertices[0]).cross(new Vector3().subVectors(vertices[next],vertices[0])).length()*.5;}
      const radiance=color.map(c=>c*intensity*2**exposure/(light.normalize?area:1));if(!radiance.every(Number.isFinite)||area<=0)throw new Error('Invalid disk light radiance');
      const offset=result.positions.length/3,material=result.materials.length;result.materials.push({twoSidedEmission:false,nodes:[{name:'emission',category:'uniform_edf',type:'EDF',inputs:{color:{type:'color3',value:radiance}}},{name:'surface',category:'surface',type:'surfaceshader',inputs:{edf:{nodename:'emission'}}}]});
      for(let i=0;i<vertices.length;i++){result.positions.push(...vertices[i].toArray());result.normals.push(...normal.toArray());result.uvs.push(i?0.5+0.5*Math.cos((i-1)*2*Math.PI/segments):0.5,i?0.5+0.5*Math.sin((i-1)*2*Math.PI/segments):0.5);result.colors.push(0,0,0,1);}
      for(let i=0;i<segments;i++){const a=offset,b=offset+1+i,c=offset+1+(i+1)%segments;result.indices.push(...(new Vector3().subVectors(vertices[b-offset],vertices[a-offset]).cross(new Vector3().subVectors(vertices[c-offset],vertices[a-offset])).dot(normal)>0?[a,b,c]:[a,c,b]));result.materialIds.push(material);}
      disks.push({path:light.absPath,radius,worldArea:area,radiance,materialId:material});
      areaLights.push({path:light.absPath,position:vertices[0].toArray(),normal:normal.toArray(),worldArea:area,radiance,twoSided:false});continue;
    }
    if(lightType==='cylinder') {
      if(light.textureFile||light.enableColorTemperature||light.shapingIesFile||light.shapingFocus>0||light.shapingConeAngle<90||light.diffuse!==undefined&&light.diffuse!==1||light.specular!==undefined&&light.specular!==1||light.shadowEnable===false)throw new Error('Unsupported cylinder light texture, shaping, temperature, or contribution controls');
      const {radius=.5,length=1,intensity=1,exposure=0,color=[1,1,1]}=light;
      if(![radius,length,intensity,exposure,...color].every(Number.isFinite)||radius<=0||length<=0||intensity<0||color.length!==3||color.some(c=>c<0)||!Array.isArray(light.transform)||light.transform.length!==16||!light.transform.every(Number.isFinite)||light.transform[3]!==0||light.transform[7]!==0||light.transform[11]!==0||light.transform[15]!==1)throw new Error('Invalid cylinder light parameters');
      const matrix=new Matrix4().fromArray(light.transform);if(Math.abs(matrix.determinant())<1e-15)throw new Error('Singular cylinder light transform');
      const segments=24,vertices=[];
      for(let i=0;i<segments;i++) { const a=i*2*Math.PI/segments; const x=radius*Math.cos(a),y=radius*Math.sin(a); vertices.push(new Vector3(x,y,-length/2).applyMatrix4(matrix),new Vector3(x,y,length/2).applyMatrix4(matrix)); }
      const normalMatrix=new Matrix3().getNormalMatrix(matrix), normals=[];
      for(let i=0;i<segments;i++) { const a=i*2*Math.PI/segments; normals.push(new Vector3(Math.cos(a),Math.sin(a),0).applyMatrix3(normalMatrix).normalize()); }
      let area=0;
      for(let i=0;i<segments;i++) { const next=(i+1)%segments, a=vertices[i*2], b=vertices[i*2+1], c=vertices[next*2+1], d=vertices[next*2]; area+=new Vector3().subVectors(b,a).cross(new Vector3().subVectors(d,a)).length()*.5+new Vector3().subVectors(c,b).cross(new Vector3().subVectors(d,b)).length()*.5; }
      const radiance=color.map(c=>c*intensity*2**exposure/(light.normalize?area:1));if(!radiance.every(Number.isFinite)||area<=0)throw new Error('Invalid cylinder light radiance or area');
      const offset=result.positions.length/3,material=result.materials.length;result.materials.push({twoSidedEmission:false,nodes:[{name:'emission',category:'uniform_edf',type:'EDF',inputs:{color:{type:'color3',value:radiance}}},{name:'surface',category:'surface',type:'surfaceshader',inputs:{edf:{nodename:'emission'}}}]});
      for(let i=0;i<segments;i++) { const a=i*2; for(const j of [a,a+1]) { result.positions.push(...vertices[j].toArray());result.normals.push(...normals[i].toArray());const u=i/segments;result.uvs.push(u,j===a?0:1);result.colors.push(0,0,0,1); } }
      for(let i=0;i<segments;i++) { const next=(i+1)%segments,a=offset+i*2,b=a+1,c=offset+next*2+1,d=offset+next*2;result.indices.push(a,d,b,d,c,b);result.materialIds.push(material,material); }
      cylinders.push({path:light.absPath,radius,length,worldArea:area,radiance,materialId:material});
      const center=vertices.reduce((sum,v)=>sum.add(v),new Vector3()).multiplyScalar(1/vertices.length),sampleNormal=new Vector3(1,0,0).applyMatrix3(normalMatrix).normalize();
      areaLights.push({path:light.absPath,position:center.toArray(),normal:sampleNormal.toArray(),worldArea:area,radiance,twoSided:true,shape:'cylinder'});continue;
    }
    if(lightType==='geometry') {
      const targetPath=light.geometryTargetPath||light.geometry_target_path||light.properties?.geometryTargetPath||light.properties?.geometry_target_path;
      const binding=(scene.authored?.bindings||[]).find(item=>item.path===targetPath);
      if(!targetPath||!binding||![binding.vertexOffset,binding.vertexCount,binding.indexOffset,binding.indexCount].every(Number.isInteger)||binding.vertexOffset<0||binding.vertexCount<=0||binding.indexOffset<0||binding.indexCount<=0||binding.indexOffset+binding.indexCount>scene.indices.length)throw new Error('Geometry light requires a bounded mesh target binding');
      const sourcePositions=scene.positions, sourceNormals=scene.normals, sourceUVs=scene.uvs;
      const vo=binding.vertexOffset, vc=binding.vertexCount, io=binding.indexOffset, ic=binding.indexCount, offset=result.positions.length/3;
      const {intensity=1,exposure=0,color=[1,1,1]}=light;
      if(![intensity,exposure,...color].every(Number.isFinite)||intensity<0||color.length!==3||color.some(c=>c<0))throw new Error('Invalid geometry light parameters');
      let area=0, center=new Vector3(), weightedNormal=new Vector3();
      for(let t=io;t<io+ic;t+=3){const ids=[scene.indices[t],scene.indices[t+1],scene.indices[t+2]];if(ids.some(id=>!Number.isInteger(id)||id<vo||id>=vo+vc))throw new Error('Geometry light target indices escape binding');const a=new Vector3(...sourcePositions.slice(ids[0]*3,ids[0]*3+3)),b=new Vector3(...sourcePositions.slice(ids[1]*3,ids[1]*3+3)),c=new Vector3(...sourcePositions.slice(ids[2]*3,ids[2]*3+3));const cross=new Vector3().subVectors(b,a).cross(new Vector3().subVectors(c,a)),triangleArea=cross.length()*.5;if(triangleArea>0){area+=triangleArea;center.addScaledVector(a.clone().add(b).add(c).multiplyScalar(1/3),triangleArea);weightedNormal.add(cross);}}
      if(!(area>0))throw new Error('Geometry light target has no positive area');
      center.multiplyScalar(1/area);const normal=weightedNormal.normalize();const radiance=color.map(c=>c*intensity*2**exposure/(light.normalize?area:1));if(!radiance.every(Number.isFinite))throw new Error('Invalid geometry light radiance');
      const material=result.materials.length;result.materials.push({twoSidedEmission:false,nodes:[{name:'emission',category:'uniform_edf',type:'EDF',inputs:{color:{type:'color3',value:radiance}}},{name:'surface',category:'surface',type:'surfaceshader',inputs:{edf:{nodename:'emission'}}}]});
      for(let v=0;v<vc;v++){const src=vo+v;result.positions.push(...sourcePositions.slice(src*3,src*3+3));result.normals.push(...sourceNormals.slice(src*3,src*3+3));result.uvs.push(...sourceUVs.slice(src*2,src*2+2));if(result.tangents.length)result.tangents.push(...(scene.tangents?.slice(src*4,src*4+4)||[0,0,0,1]));if(result.colors.length)result.colors.push(...(scene.colors?.slice(src*4,src*4+4)||[0,0,0,1]));for(const [name,values] of Object.entries(result.geompropSets))values.push(...(scene.geompropSets?.[name]?.slice(src*4,src*4+4)||[0,0,0,0]));for(let slot=0;slot<result.uvSets.length;slot++)result.uvSets[slot].push(...(scene.uvSets?.[slot]?.slice(src*2,src*2+2)||[0,0]));}
      for(let t=io;t<io+ic;t++)result.indices.push(scene.indices[t]-vo+offset);for(let t=io;t<io+ic;t+=3)result.materialIds.push(material);
      geometryLights.push({path:light.absPath,targetPath,worldArea:area,radiance,materialId:material,materialSyncMode:light.materialSyncMode||light.material_sync_mode||'materialGlowTintsLight'});areaLights.push({path:light.absPath,position:center.toArray(),normal:normal.toArray(),worldArea:area,radiance,twoSided:false,shape:'geometry'});continue;
    }
    if(lightType!=='rect')throw new Error(`Unsupported authored light type: ${light.type}`);
    if(light.textureFile||light.enableColorTemperature||light.shapingIesFile||light.shapingFocus>0||light.shapingConeAngle<90||light.diffuse!==undefined&&light.diffuse!==1||light.specular!==undefined&&light.specular!==1||light.shadowEnable===false)throw new Error('Unsupported rect light texture, shaping, temperature, or contribution controls');
    const {width=1,height=1,intensity=1,exposure=0,color=[1,1,1]}=light;
    if(![width,height,intensity,exposure,...color].every(Number.isFinite)||width<=0||height<=0||intensity<0||color.length!==3||color.some(c=>c<0))throw new Error('Invalid rect light parameters');
    if(!Array.isArray(light.transform)||light.transform.length!==16||!light.transform.every(Number.isFinite)||light.transform[3]!==0||light.transform[7]!==0||light.transform[11]!==0||light.transform[15]!==1)throw new Error('Invalid affine light transform');
    const matrix=new Matrix4().fromArray(light.transform);
    if(Math.abs(matrix.determinant())<1e-15)throw new Error('Singular light transform');
    const vertices=[[-width/2,-height/2,0],[-width/2,height/2,0],[width/2,height/2,0],[width/2,-height/2,0]].map(p=>new Vector3(...p).applyMatrix4(matrix));
    const normal=new Vector3(0,0,-1).applyMatrix3(new Matrix3().getNormalMatrix(matrix)).normalize();
    const cross=new Vector3().subVectors(vertices[1],vertices[0]).cross(new Vector3().subVectors(vertices[3],vertices[0]));
    const area=cross.length(),radiance=color.map(c=>c*intensity*2**exposure/(light.normalize?area:1));
    if(!radiance.every(Number.isFinite)||area<=0)throw new Error('Invalid light radiance or area');
    const offset=result.positions.length/3,material=result.materials.length;
    result.materials.push({twoSidedEmission:false,nodes:[
      {name:'emission',category:'uniform_edf',type:'EDF',inputs:{color:{type:'color3',value:radiance}}},
      {name:'surface',category:'surface',type:'surfaceshader',inputs:{edf:{nodename:'emission'}}}
    ]});
    for(const v of vertices){result.positions.push(...v.toArray());result.normals.push(...normal.toArray());result.colors.push(0,0,0,1);}
    result.uvs.push(0,0,0,1,1,1,1,0);
    const winding=cross.dot(normal)>0?[0,1,2,0,2,3]:[0,2,1,0,3,2];
    result.indices.push(...winding.map(i=>i+offset));result.materialIds.push(material,material);
    const center=vertices.reduce((sum,v)=>sum.add(v),new Vector3()).multiplyScalar(.25);
    imported.push({path:light.absPath,worldArea:area,radiance,materialId:material});
    areaLights.push({path:light.absPath,position:center.toArray(),normal:normal.toArray(),worldArea:area,radiance,twoSided:false});
  }
  if(texturedDomes.length>1) {
    const first=texturedDomes[0].image, pixelCount=first.width*first.height;
    if(!Number.isInteger(first.width)||!Number.isInteger(first.height)||first.width<1||first.height<1||first.data?.length!==pixelCount*4)throw new Error('Invalid textured dome image');
    const data=new Float32Array(first.data.length);
    for(const {image,radiance} of texturedDomes) {
      if(image.width!==first.width||image.height!==first.height||image.data?.length!==data.length)throw new Error('Textured dome images must have matching dimensions');
      for(let i=0;i<data.length;i+=4) { data[i]+=image.data[i]*radiance[0]; data[i+1]+=image.data[i+1]*radiance[1]; data[i+2]+=image.data[i+2]*radiance[2]; data[i+3]=Math.max(data[i+3],image.data[i+3]); }
    }
    environmentTexture={...first,data,colorspace:first.colorspace||'lin_rec709',scale:[1,1,1]};
  }
  result.lighting={environment,directional:distant||{radiance:[0,0,0]},pointLights:points,areaLights,...(environmentTexture?{environmentTexture}: {})};
  result.provenance={...scene.provenance,lightingOverride:false,rectLights:imported,pointLights:points,diskLights:disks,cylinderLights:cylinders,geometryLights,distantLight:distant,domeLights:domes};
  return result;
}
