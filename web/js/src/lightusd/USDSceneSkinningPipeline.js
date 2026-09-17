import * as THREE from 'three';
import { findNodeByUSDPath, replaceWithSkinnedMesh } from './SkinnedMeshUtils.js';
import {
  addExtendedSkinningAttributes,
  applyExtendedSkinningIfNeeded
} from './ExtendedSkinning.js';
import { collectMeshesInHierarchy } from './SceneGraphUtils.js';

/**
 * Recover a skeleton prim's WORLD transform from the RenderScene node table
 * (next backend). The next-built three tree is flat — every mesh has its world
 * matrix baked — so the Skeleton prim has no named node to parent bones under;
 * without its ancestor transform (e.g. an axis-correcting Z_UP xform) the
 * bones deform in the wrong space. Returns a fixed-matrix Group or null.
 */
function skeletonWorldParentFromScene(usdScene, skelAbsPath) {
  const nodes = usdScene && Array.isArray(usdScene.nodes) ? usdScene.nodes : null;
  if (!nodes || !skelAbsPath) return null;
  const node = nodes.find((n) => n && n.primPath === skelAbsPath);
  const m = node && node.worldMatrix;
  if (!m || m.length !== 16) return null;
  const group = new THREE.Group();
  group.name = `${skelAbsPath}`;
  // USD row-major -> three.js column-major
  group.matrix.set(
    m[0], m[4], m[8], m[12],
    m[1], m[5], m[9], m[13],
    m[2], m[6], m[10], m[14],
    m[3], m[7], m[11], m[15]
  );
  group.matrixAutoUpdate = false;
  return group;
}

function registerMeshVisibility(
  mesh,
  showMesh,
  allSceneMeshes,
  meshVisibility,
  enableShadows
) {
  mesh.castShadow = enableShadows;
  mesh.receiveShadow = enableShadows;
  mesh.visible = showMesh;
  allSceneMeshes.push(mesh);
  meshVisibility.set(mesh, true);
}

function collectUsedSkinBones(skeleton, sceneMeshes) {
  const used = new Set();
  const bones = skeleton?.bones || [];

  for (const mesh of sceneMeshes) {
    if (!mesh.isSkinnedMesh || mesh.skeleton !== skeleton || !mesh.geometry) {
      continue;
    }

    for (const attrName of ['skinIndex', 'skinIndex2']) {
      const skinIndex = mesh.geometry.attributes[attrName];
      if (!skinIndex) {
        continue;
      }

      const weightName = attrName === 'skinIndex' ? 'skinWeight' : 'skinWeight2';
      const skinWeight = mesh.geometry.attributes[weightName];
      for (let i = 0; i < skinIndex.count; i++) {
        for (let j = 0; j < skinIndex.itemSize; j++) {
          if (skinWeight && skinWeight.getComponent(i, j) <= 1.0e-6) {
            continue;
          }
          const boneIndex = skinIndex.getComponent(i, j);
          if (boneIndex >= 0 && boneIndex < bones.length) {
            used.add(bones[boneIndex]);
          }
        }
      }
    }
  }

  const included = new Set();
  for (const bone of used) {
    let current = bone;
    while (current && current.isBone && !current.userData?.isTipBone) {
      included.add(current);
      current = current.parent;
    }
  }

  return included;
}

function lineEndpointForTip(parentPos, childPos, capTips, maxLength, target) {
  target.copy(childPos);
  if (!capTips || !(maxLength > 0)) {
    return target;
  }

  target.sub(parentPos);
  const len = target.length();
  if (len > maxLength) {
    target.multiplyScalar(maxLength / len);
  }
  target.add(parentPos);
  return target;
}

class FilteredSkeletonHelper extends THREE.LineSegments {
  constructor(rootBone, skeleton, sceneMeshes, options = {}) {
    const geometry = new THREE.BufferGeometry();
    const material = new THREE.LineBasicMaterial({
      vertexColors: true,
      depthTest: false,
      transparent: true,
      opacity: 0.95,
      toneMapped: false
    });
    super(geometry, material);

    this.rootBone = rootBone;
    this.skeleton = skeleton;
    this.sceneMeshes = sceneMeshes;
    this.type = 'FilteredSkeletonHelper';
    this.frustumCulled = false;
    this.matrixAutoUpdate = false;
    this.visible = !!options.visible;
    this.options = {
      mode: options.mode || 'full',
      showTips: options.showTips !== false,
      capTips: !!options.capTips,
      tipMaxLength: options.tipMaxLength || 120
    };
    this.edges = [];
    this._parentPos = new THREE.Vector3();
    this._childPos = new THREE.Vector3();
    this._cappedChildPos = new THREE.Vector3();
    this.rebuild();
  }

  setDisplayOptions(options = {}) {
    Object.assign(this.options, options);
    this.rebuild();
  }

  rebuild() {
    const mode = this.options.mode === 'deforming' ? 'deforming' : 'full';
    const includeTips = !!this.options.showTips;
    const includedBones = mode === 'deforming'
      ? collectUsedSkinBones(this.skeleton, this.sceneMeshes)
      : null;

    const isIncluded = (bone) => {
      if (!bone || !bone.isBone) {
        return false;
      }
      if (bone.userData?.isTipBone) {
        return includeTips && isIncluded(bone.parent);
      }
      return mode === 'full' || includedBones.has(bone);
    };

    this.edges = [];
    this.rootBone.traverse((bone) => {
      if (!bone.isBone || !bone.parent?.isBone || !isIncluded(bone) || !isIncluded(bone.parent)) {
        return;
      }
      this.edges.push({
        parent: bone.parent,
        child: bone,
        isTip: !!bone.userData?.isTipBone
      });
    });

    const positions = new Float32Array(this.edges.length * 2 * 3);
    const colors = new Float32Array(this.edges.length * 2 * 3);
    for (let i = 0; i < this.edges.length; i++) {
      const c = i * 6;
      colors[c + 0] = 0.0;
      colors[c + 1] = 1.0;
      colors[c + 2] = 0.45;
      colors[c + 3] = 0.0;
      colors[c + 4] = 0.35;
      colors[c + 5] = 1.0;
    }

    this.geometry.setAttribute('position', new THREE.BufferAttribute(positions, 3));
    this.geometry.setAttribute('color', new THREE.BufferAttribute(colors, 3));
    this.update();
  }

  update() {
    const positionAttr = this.geometry.attributes.position;
    if (!positionAttr) {
      return;
    }

    this.rootBone.updateMatrixWorld(true);
    for (let i = 0; i < this.edges.length; i++) {
      const edge = this.edges[i];
      edge.parent.getWorldPosition(this._parentPos);
      edge.child.getWorldPosition(this._childPos);

      const childPos = edge.isTip
        ? lineEndpointForTip(
            this._parentPos,
            this._childPos,
            this.options.capTips,
            this.options.tipMaxLength,
            this._cappedChildPos)
        : this._childPos;

      positionAttr.setXYZ(i * 2, this._parentPos.x, this._parentPos.y, this._parentPos.z);
      positionAttr.setXYZ(i * 2 + 1, childPos.x, childPos.y, childPos.z);
    }
    positionAttr.needsUpdate = true;
  }

  dispose() {
    this.geometry.dispose();
    this.material.dispose();
  }
}

/**
 * Build skeletons, bind skinned meshes, and collect render meshes for a USD scene node.
 */
export function applyUSDSceneSkinningPipeline(options = {}) {
  const logger = options.logger || console;
  const threeNode = options.threeNode;
  const characterGroup = options.characterGroup;
  const helperScene = options.helperScene;
  const skeletonDataArray = options.skeletonDataArray || [];
  const allSkinnedMeshUSDData = options.allSkinnedMeshUSDData || new Map();
  const skinnedMeshDataByName = options.skinnedMeshDataByName || new Map();
  const usdScene = options.usdScene;
  const showMesh = options.showMesh !== false;
  const enableShadows = options.enableShadows !== false;
  const showSkeleton = !!options.showSkeleton;
  const skeletonDisplayMode = options.skeletonDisplayMode || 'full';
  const showSkeletonTips = options.showSkeletonTips !== false;
  const capSkeletonTips = !!options.capSkeletonTips;
  const skeletonTipMaxLength = options.skeletonTipMaxLength || 120;
  const useWASMBoneTexture = !!options.useWASMBoneTexture;
  const textureLoadingManager = options.textureLoadingManager || null;

  if (!threeNode || !characterGroup) {
    throw new Error('applyUSDSceneSkinningPipeline requires threeNode and characterGroup');
  }

  characterGroup.add(threeNode);

  const allMeshes = collectMeshesInHierarchy(threeNode);
  logger.log(
    `Found ${allMeshes.length} meshes in hierarchy, ${allSkinnedMeshUSDData.size} have USD skinning data`
  );

  const skeletons = new Map();
  const skeletonHelpers = [];
  const allSceneMeshes = [];
  const meshVisibility = new Map();

  let firstSkeleton = null;
  let firstSkeletonHelper = null;
  let firstSkinnedMesh = null;
  let firstRenderableMesh = null;
  let processedSkinnedCount = 0;

  if (allMeshes.length > 0 && skeletonDataArray.length > 0) {
    for (const skelData of skeletonDataArray) {
      const {
        skelId,
        bones: skelBones,
        boneInverses: skelBoneInverses,
        rootBone: skelRootBone,
        skeletonAbsPath: skelAbsPath
      } = skelData;

      const skelThree = new THREE.Skeleton(skelBones, skelBoneInverses);
      logger.log(`Created skeleton ${skelId} with ${skelBones.length} bones`);
      skeletons.set(skelId, skelThree);

      if (skelId === 0) {
        firstSkeleton = skelThree;
      }

      if (!skelRootBone || !(skelRootBone instanceof THREE.Bone)) {
        logger.error(`Invalid rootBone for skeleton ${skelId}:`, skelRootBone);
        throw new Error(`rootBone for skeleton ${skelId} is not a valid THREE.Bone`);
      }

      let boneParent = threeNode;
      if (skelAbsPath) {
        const skeletonNode = findNodeByUSDPath(threeNode, skelAbsPath);
        if (skeletonNode) {
          boneParent = skeletonNode;
          logger.log(
            `Placing rootBone ${skelId} at skeleton node: ${boneParent.name} (path: ${skelAbsPath})`
          );
        } else {
          // Flat (next) trees have no per-prim nodes; rebuild the skeleton
          // prim's world transform from the RenderScene node table so bones
          // deform in the same space the baked mesh worlds use.
          const worldParent = skeletonWorldParentFromScene(usdScene, skelAbsPath);
          if (worldParent) {
            threeNode.add(worldParent);
            boneParent = worldParent;
            logger.log(
              `Placing rootBone ${skelId} under reconstructed skeleton world node (path: ${skelAbsPath})`
            );
          } else {
            logger.warn(
              `Could not find skeleton node "${skelAbsPath}" in hierarchy for skeleton ${skelId}, falling back to threeNode root`
            );
          }
        }
      }
      boneParent.add(skelRootBone);
    }

    characterGroup.updateMatrixWorld(true);

    // USD bindTransforms are authored in skeleton space.  Once the bone
    // hierarchy is parented under its Skeleton prim, calculate inverses from
    // the resulting world-space bind pose.  This includes the complete
    // Skeleton ancestor transform (scale/orientation) and matches Three.js's
    // world-space bind() contract; using raw USD inverses here double-applies
    // transforms such as Mixamo's 0.01 Armature scale.
    for (const skeleton of skeletons.values()) {
      skeleton.calculateInverses();
    }

    for (const mesh of allMeshes) {
      const meshName = mesh.name;
      const meshAbsPath = mesh.userData?.['primMeta.absPath'] || '';
      let meshUSDData = allSkinnedMeshUSDData.get(meshAbsPath);
      if (!meshUSDData && !meshAbsPath && skinnedMeshDataByName.has(meshName)) {
        meshUSDData = skinnedMeshDataByName.get(meshName);
        if (meshUSDData === null) {
          logger.warn(
            `Mesh ${meshName}: ambiguous name fallback (duplicate names), skipping skinning without absPath match`
          );
        }
      }

      if (meshUSDData) {
        logger.log(
          `Processing mesh: ${meshName} (path: ${meshAbsPath}, skel_id: ${meshUSDData.skel_id})`
        );

        const meshSkelId =
          meshUSDData.skel_id !== undefined ? meshUSDData.skel_id : 0;
        const meshSkeleton = skeletons.get(meshSkelId);

        if (!meshSkeleton) {
          logger.warn(
            `  Skeleton ${meshSkelId} not found for mesh ${meshName}, skipping`
          );
          continue;
        }

        const geometry = mesh.geometry;
        if (!geometry || !geometry.attributes.position) {
          logger.warn(`  Mesh ${meshName}: missing position attribute, skipping`);
          continue;
        }

        const vertexCount = geometry.attributes.position.count;
        const influencesPerVertex = meshUSDData.elementSize || 4;
        const usdVertexCount = Math.floor(
          meshUSDData.jointIndices.length / influencesPerVertex
        );

        logger.log(
          `  Adding skinning: ${vertexCount} vertices, ${influencesPerVertex} influences/vertex`
        );

        if (vertexCount !== usdVertexCount) {
          logger.warn(
            `  Vertex count mismatch: Three.js=${vertexCount}, USD=${usdVertexCount}`
          );
        }

        const skinningConfig = addExtendedSkinningAttributes(
          geometry,
          meshUSDData.jointIndices,
          meshUSDData.jointWeights,
          influencesPerVertex,
          { normalize: true }
        );

        logger.log(`  Skinning mode: ${skinningConfig.mode}`);

        const newSkinnedMesh = replaceWithSkinnedMesh(mesh);
        registerMeshVisibility(
          newSkinnedMesh,
          showMesh,
          allSceneMeshes,
          meshVisibility,
          enableShadows
        );

        newSkinnedMesh.bind(meshSkeleton);
        logger.log(
          `  Bound to skeleton ${meshSkelId} (bindMatrix = mesh.matrixWorld)`
        );

        let wasmBoneTexture = null;
        if (useWASMBoneTexture && meshUSDData.elementSize > 8 && usdScene) {
          try {
            wasmBoneTexture = usdScene.generateBoneTexture(meshUSDData.meshId, 0);
            if (wasmBoneTexture && wasmBoneTexture.error) {
              logger.warn(
                `WASM bone texture generation failed: ${wasmBoneTexture.error}`
              );
              wasmBoneTexture = null;
            }
          } catch (texErr) {
            logger.warn(`WASM bone texture error: ${texErr.message}`);
            wasmBoneTexture = null;
          }
        }

        const previousMaterial = newSkinnedMesh.material;
        if (applyExtendedSkinningIfNeeded(newSkinnedMesh, { wasmBoneTexture })) {
          const replacementMaterial = newSkinnedMesh.material;
          if (textureLoadingManager?.rebindMaterial) {
            const previousMaterials = Array.isArray(previousMaterial)
              ? previousMaterial : [previousMaterial];
            const replacementMaterials = Array.isArray(replacementMaterial)
              ? replacementMaterial : [replacementMaterial];
            previousMaterials.forEach((item, index) => {
              textureLoadingManager.rebindMaterial(
                item,
                replacementMaterials[index] || replacementMaterials[0]
              );
            });
          }
          logger.log('  Extended skinning material applied');
        }

        if (!firstSkinnedMesh) {
          firstSkinnedMesh = newSkinnedMesh;
        }
        if (!firstRenderableMesh) {
          firstRenderableMesh = newSkinnedMesh;
        }

        processedSkinnedCount++;
      } else {
        logger.log(
          `Mesh ${meshName} (${meshAbsPath || 'no absPath'}): no USD skinning data, keeping as regular mesh`
        );
        registerMeshVisibility(
          mesh,
          showMesh,
          allSceneMeshes,
          meshVisibility,
          enableShadows
        );
        if (!firstRenderableMesh) {
          firstRenderableMesh = mesh;
        }
      }
    }

    logger.log(`Processed ${processedSkinnedCount} skinned meshes (hierarchy preserved)`);

    logger.log('=== Mesh-to-Skeleton Binding Summary ===');
    const skelToMeshes = new Map();
    for (const [meshPath, meshData] of allSkinnedMeshUSDData) {
      const skelId = meshData.skel_id;
      if (!skelToMeshes.has(skelId)) {
        skelToMeshes.set(skelId, []);
      }
      skelToMeshes.get(skelId).push(meshPath);
    }
    for (const [skelId, meshNames] of skelToMeshes) {
      logger.log(`Skeleton ${skelId}: meshes = [${meshNames.join(', ')}]`);
    }
    logger.log('=== End Binding Summary ===');

    if (helperScene) {
      for (const skelData of skeletonDataArray) {
        const { skelId, rootBone: skelRootBone } = skelData;
        const helper = new FilteredSkeletonHelper(
          skelRootBone,
          skeletons.get(skelId),
          allSceneMeshes,
          {
            visible: showSkeleton,
            mode: skeletonDisplayMode,
            showTips: showSkeletonTips,
            capTips: capSkeletonTips,
            tipMaxLength: skeletonTipMaxLength
          }
        );
        helper.name = `SkeletonHelper_${skelId}`;
        helperScene.add(helper);
        skeletonHelpers.push(helper);
        logger.log(`Created skeleton helper for skeleton ${skelId}`);

        if (skelId === 0) {
          firstSkeletonHelper = helper;
        }
      }
    }
  } else {
    logger.log('No skeleton data or no meshes, scene added as-is');
    for (const child of allMeshes) {
      registerMeshVisibility(
        child,
        showMesh,
        allSceneMeshes,
        meshVisibility,
        enableShadows
      );
      if (!firstRenderableMesh) {
        firstRenderableMesh = child;
      }
    }
  }

  return {
    allMeshes,
    allSceneMeshes,
    meshVisibility,
    skeletons,
    skeletonHelpers,
    firstSkeleton,
    firstSkeletonHelper,
    firstSkinnedMesh,
    primaryMesh: firstSkinnedMesh || firstRenderableMesh,
    processedSkinnedCount,
    hasSkeletonHelpers: skeletonHelpers.length > 0
  };
}
