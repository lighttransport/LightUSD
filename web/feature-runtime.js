// SPDX-License-Identifier: Apache-2.0
{
  let info;
  Object.defineProperty(Module, 'featureInfo', {get() {
    if (!info) {
      const fields = ['abiMajor', 'coreMajor', 'pointerBits', 'layoutRevision',
        'valueSize', 'valueAlignment', 'valueStoreOffset', 'descriptorSize',
        'chunksSize', 'valueViewSize', 'valueViewDataOffset', 'valueViewOwnerOffset'];
      info = Object.freeze(Object.fromEntries(fields.map((name, i) => [name, Module['_lu_feature_info'](i)])));
    }
    return info;
  }});
}
