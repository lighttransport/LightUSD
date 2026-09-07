// SPDX-License-Identifier: Apache-2.0
import { packImages, imageWGSL } from './textures.js';

/** Analytic 2x2 image: R,G / B,white, with independent expected samples. */
export async function validateImageKernels(device) {
  const packed = packImages([{ width: 2, height: 2, data: [1,0,0,1, 0,1,0,1, 0,0,1,1, 1,1,1,1] }]);
  const cases = [
    ['red center', [.25,.25], 0, 2, true, [1,0,0,1]],
    ['bilinear center', [.5,.5], 0, 2, true, [.5,.5,.5,1]],
    ['repeat negative', [-.75,.25], 0, 2, false, [1,0,0,1]],
    ['mirror negative', [-.25,.25], 0, 3, false, [1,0,0,1]],
    ['clamp negative', [-3,.25], 0, 1, true, [1,0,0,1]],
    ['constant outside', [-1,.25], 0, 0, true, [0,0,0,0]],
    ['constant edge blend', [0,.25], 0, 0, true, [.5,0,0,.5]],
    ['mip average', [.25,.25], 1, 2, true, [.5,.5,.5,1]],
    ['trilinear', [.25,.25], .5, 2, true, [.75,.25,.25,1]],
    ['LOD clamp', [.25,.25], 99, 2, true, [.5,.5,.5,1]],
  ];
  const module = device.createShaderModule({ code: `${imageWGSL}
    @group(0) @binding(0) var<storage,read_write> result: array<vec4f>;
    @compute @workgroup_size(1) fn main() {
      ${cases.map(([,uv,lod,address,linear], i) => `result[${i}]=imageSample(0u,vec2u(2),2u,vec2f(${uv.join(',')}),${Number(lod).toFixed(1)},vec2u(${address}u),${linear},vec4f(0));`).join('\n')}
    }` });
  const info = await module.getCompilationInfo();
  if (info.messages.some(m => m.type === 'error')) throw new Error(info.messages.map(m => m.message).join('\n'));
  const pipeline = await device.createComputePipelineAsync({ layout: 'auto', compute: { module, entryPoint: 'main' } });
  const images = device.createBuffer({ size: packed.data.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
  const output = device.createBuffer({ size: cases.length * 16, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
  const readback = device.createBuffer({ size: output.size, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
  try {
    device.queue.writeBuffer(images, 0, packed.data);
    const group = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: output } }, { binding: 4, resource: { buffer: images } }] });
    const encoder = device.createCommandEncoder(), pass = encoder.beginComputePass();
    pass.setPipeline(pipeline); pass.setBindGroup(0, group); pass.dispatchWorkgroups(1); pass.end();
    encoder.copyBufferToBuffer(output, 0, readback, 0, output.size); device.queue.submit([encoder.finish()]);
    await readback.mapAsync(GPUMapMode.READ); const actual = new Float32Array(readback.getMappedRange());
    for (let i = 0; i < cases.length; i++) for (let k = 0; k < 4; k++) if (!Number.isFinite(actual[i*4+k]) || Math.abs(actual[i*4+k] - cases[i][5][k]) > 1e-5) throw new Error(`Image ${cases[i][0]} channel ${k}: ${actual[i*4+k]} != ${cases[i][5][k]}; all=${Array.from(actual)}`);
    return cases.map(c => c[0]);
  } finally { images.destroy(); output.destroy(); readback.destroy(); }
}
