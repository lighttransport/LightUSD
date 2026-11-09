// SPDX-License-Identifier: Apache-2.0
/** Conservative proof only: unknown/custom/dynamic emission stays sampleable. */
export function mayEmit(document) {
  if(document.spectra?.emission_color||Object.keys(document.definitions||{}).length)return true;
  const nodes=new Map(document.nodes.map(n=>[n.name,n]));const visited=new Set();
  function port(p) {
    if(!p||p.value==='')return false;
    if(!p.nodename||p.nodegraph||p.interfacename)return true;
    return node(nodes.get(p.nodename));
  }
  function node(n) {
    if(!n||n.nodedef||visited.has(n.name))return true;visited.add(n.name);
    if(n.category==='surfacematerial')return port(n.inputs?.surfaceshader);
    if(n.category==='surface')return port(n.inputs?.edf);
    if(['standard_surface','open_pbr_surface'].includes(n.category)) {
      const p=n.inputs?.[n.category==='standard_surface'?'emission':'emission_luminance'];
      return !!p && (!!p.nodename||!!p.nodegraph||!!p.interfacename||Number(p.value)!==0);
    }
    return true;
  }
  return document.output?port(document.output):node(document.nodes.at(-1));
}
