// SPDX-License-Identifier: Apache-2.0
import { Matrix4, Matrix3, Vector3 } from 'three';

/** UsdLux radiance = color * intensity * 2^exposure / normalized world area.
 * https://openusd.org/dev/user_guides/schemas/usdLux/LightAPI.html
 * Rectangles emit along local -Z. Unsupported nonphysical controls fail closed.
 */
export function appendRectLights(scene, lights) {
  const result={...scene,positions:Array.from(scene.positions),normals:Array.from(scene.normals||[]),uvs:Array.from(scene.uvs||[]),colors:Array.from(scene.colors||[]),indices:Array.from(scene.indices),materialIds:Array.from(scene.materialIds||new Array(scene.indices.length/3).fill(0)),materials:scene.materials.slice()};
  if(result.normals.length!==result.positions.length||result.uvs.length!==result.positions.length/3*2)throw new Error('Light conversion requires scene normals and UVs');
  if(!result.colors.length)result.colors=new Array(result.positions.length/3*4).fill(0).map((v,i)=>i%4===3?1:v);
  else if(result.colors.length===result.positions.length/3*3){const rgb=result.colors;result.colors=[];for(let i=0;i<rgb.length;i+=3)result.colors.push(rgb[i],rgb[i+1],rgb[i+2],1);}
  if(result.colors.length!==result.positions.length/3*4)throw new Error('Light conversion color count mismatch');
  const imported=[];
  let distant = null;
  const environment=[0,0,0];
  const domes=[];
  const points=[];
  for(const light of lights) {
    if(light.type==='dome') {
      if(light.textureFile||Number.isInteger(light.envmapTextureId)&&light.envmapTextureId>=0||light.enableColorTemperature)throw new Error('Textured or temperature-controlled dome lights are unsupported');
      const {intensity=1,exposure=0,color=[1,1,1]}=light;
      if(![intensity,exposure,...color].every(Number.isFinite)||intensity<0||color.length!==3||color.some(c=>c<0))throw new Error('Invalid dome light parameters');
      const radiance=color.map(c=>c*intensity*2**exposure);
      if(!radiance.every(Number.isFinite))throw new Error('Invalid dome light radiance');
      radiance.forEach((v,i)=>environment[i]+=v);domes.push({path:light.absPath,radiance});continue;
    }
    if(light.type==='distant') {
      if(light.textureFile||light.enableColorTemperature||light.shapingIesFile||light.shapingFocus>0||light.shapingConeAngle<90||light.diffuse!==undefined&&light.diffuse!==1||light.specular!==undefined&&light.specular!==1||light.shadowEnable===false)throw new Error('Unsupported distant light texture, shaping, temperature, or contribution controls');
      const {intensity=1,exposure=0,color=[1,1,1]}=light;
      const direction=Array.isArray(light.direction)&&light.direction.length===3?light.direction:null;
      if(!direction||!direction.every(Number.isFinite)||Math.hypot(...direction)<1e-8||![intensity,exposure,...color].every(Number.isFinite)||intensity<0||color.length!==3||color.some(c=>c<0))throw new Error('Invalid distant light parameters');
      const length=Math.hypot(...direction),towardLight=direction.map(v=>v===0?0:-v/length),radiance=color.map(c=>c*intensity*2**exposure);
      if(!radiance.every(Number.isFinite))throw new Error('Invalid distant light radiance');
      if(distant && (distant.direction.some((v,i)=>Math.abs(v-towardLight[i])>1e-6)||distant.radiance.some((v,i)=>Math.abs(v-radiance[i])>1e-6)))throw new Error('Multiple nonmatching distant lights are unsupported');
      distant={direction:towardLight,radiance};continue;
    }
    if(light.type==='point'||light.type==='sphere') {
      if(light.textureFile||light.enableColorTemperature||light.shapingIesFile||light.shapingFocus>0||light.shapingConeAngle<90||light.diffuse!==undefined&&light.diffuse!==1||light.specular!==undefined&&light.specular!==1||light.shadowEnable===false)throw new Error(`Unsupported ${light.type} light texture, shaping, temperature, or contribution controls`);
      const {intensity=1,exposure=0,color=[1,1,1]}=light,position=light.position;
      const radius=light.radius===undefined?(light.type==='sphere'?0.5:0.01):Number(light.radius);
      if(!Array.isArray(position)||position.length!==3||!position.every(Number.isFinite)||![radius,intensity,exposure,...color].every(Number.isFinite)||radius<=0||intensity<0||color.length!==3||color.some(c=>c<0))throw new Error(`Invalid ${light.type} light parameters; radius must be positive`);
      const area=4*Math.sqrt(3)*radius*radius,radiance=color.map(c=>c*intensity*2**exposure/area);
      if(!radiance.every(Number.isFinite))throw new Error('Invalid point light radiance');
      const offset=result.positions.length/3,material=result.materials.length,verts=[[1,0,0],[-1,0,0],[0,1,0],[0,-1,0],[0,0,1],[0,0,-1]].map(v=>v.map(c=>c*radius)),faces=[[0,2,4],[2,1,4],[1,3,4],[3,0,4],[2,0,5],[1,2,5],[3,1,5],[0,3,5]];
      result.materials.push({twoSidedEmission:true,nodes:[{name:'emission',category:'uniform_edf',type:'EDF',inputs:{color:{type:'color3',value:radiance}}},{name:'surface',category:'surface',type:'surfaceshader',inputs:{edf:{nodename:'emission'}}}]});
      for(const v of verts){const n=v.map(c=>c/radius);result.positions.push(position[0]+v[0],position[1]+v[1],position[2]+v[2]);result.normals.push(...n);result.uvs.push(0,0);result.colors.push(0,0,0,1);}
      for(const face of faces){result.indices.push(...face.map(i=>i+offset));result.materialIds.push(material);}
      points.push({path:light.absPath,type:light.type,position:[...position],radius,worldArea:area,radiance,materialId:material});continue;
    }
    if(light.type!=='rect')throw new Error(`Unsupported authored light type: ${light.type}`);
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
    imported.push({path:light.absPath,worldArea:area,radiance,materialId:material});
  }
  result.lighting={environment,directional:distant||{radiance:[0,0,0]}};
  result.provenance={...scene.provenance,lightingOverride:false,rectLights:imported,pointLights:points,distantLight:distant,domeLights:domes};
  return result;
}
