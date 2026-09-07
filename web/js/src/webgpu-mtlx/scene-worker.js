// SPDX-License-Identifier: Apache-2.0
import { packScene } from './scene.js';
self.onmessage = ({ data }) => {
  try {
    const packed = packScene(data.scene, data.options);
    self.postMessage({ packed }, [packed.nodeData.buffer, packed.triangleData.buffer]);
  } catch (e) { self.postMessage({ error: e.message }); }
};
