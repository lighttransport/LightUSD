/**
 * USD Animation Converter
 *
 * Converts USD animation data (from LightUSD WASM binding) to Three.js
 * AnimationClip format. Supports both SkeletonJoint channels (skeletal
 * animation) and SceneNode channels (xformOp animation on scene nodes).
 *
 * @module USDAnimationConverter
 */

import * as THREE from 'three';
import {
	markOwnedFloat32Array,
	toOwnedFloat32Array
} from './TypedArrayOwnership.js';

/**
 * Convert USD interpolation mode to Three.js InterpolateMode
 * @param {string} interpolation - USD interpolation mode (Linear, Step, CubicSpline)
 * @returns {number} Three.js InterpolateMode constant
 */
export function getUSDInterpolationMode(interpolation) {
	switch (interpolation) {
		case 'Step':
		case 'STEP':
			return THREE.InterpolateDiscrete;
		case 'CubicSpline':
		case 'CUBICSPLINE':
			return THREE.InterpolateSmooth;
		case 'Linear':
		case 'LINEAR':
		default:
			return THREE.InterpolateLinear;
	}
}

function expandArraySkeletalChannels(usdAnimation, samplers) {
	const syntheticChannels = [];
	const channels = Array.isArray(usdAnimation.channels) ? usdAnimation.channels : [];

	for (const channel of channels) {
		if (!channel || !channel.isSkeletal) continue;
		if (channel.target_type === 'SkeletonJoint') continue;
		if (channel.path === 'Weights') continue;

		const sampler = samplers[channel.sampler];
		const arrayValues = sampler?.arrayValues;
		const times = sampler?.times;
		const stride = Number.isFinite(channel.valueStride)
			? channel.valueStride
			: (Number.isFinite(sampler?.valueStride) ? sampler.valueStride : 0);
		const elementCount = Number.isFinite(channel.elementCount)
			? channel.elementCount
			: (Number.isFinite(sampler?.elementCount) ? sampler.elementCount : 0);
		const jointRemap = Array.isArray(channel.jointRemap) ? channel.jointRemap : [];
		if (!arrayValues || !times || stride <= 0 || elementCount <= 0) continue;

		const frameCount = times.length;
		const expected = frameCount * elementCount * stride;
		if (arrayValues.length < expected) {
			console.warn(`Skipping skeletal array channel ${channel.path}: expected ${expected} values, got ${arrayValues.length}`);
			continue;
		}

		for (let elem = 0; elem < elementCount; ++elem) {
			const jointId = jointRemap.length > elem ? jointRemap[elem] : elem;
			if (!Number.isFinite(jointId) || jointId < 0) continue;

			const values = markOwnedFloat32Array(
				new Float32Array(frameCount * stride),
				'expanded skeletal values');
			for (let frame = 0; frame < frameCount; ++frame) {
				const src = (frame * elementCount + elem) * stride;
				const dst = frame * stride;
				for (let c = 0; c < stride; ++c) {
					values[dst + c] = arrayValues[src + c];
				}
			}

			const samplerId = samplers.length;
			samplers.push({
				index: samplerId,
				interpolation: sampler.interpolation,
				times,
				values,
				valueStride: stride,
				elementCount: 1,
				isSkeletal: true
			});
			syntheticChannels.push({
				sampler: samplerId,
				target_type: 'SkeletonJoint',
				skeleton_id: Number.isFinite(channel.skeleton_id) ? channel.skeleton_id : 0,
				joint_id: jointId,
				path: channel.path,
				isSkeletal: true,
				propertyName: channel.propertyName || ''
			});
		}
	}

	return syntheticChannels;
}

/**
 * Convert USD skeletal animation data to Three.js AnimationClip
 * Extracts only SkeletonJoint animations from USD SkelAnimation
 * @param {Object} usdLoader - LightUSD loader instance
 * @param {Map} boneMaps - Map from skeleton_id to Map(joint_id -> THREE.Bone)
 * @param {number} [timeCodesPerSecond=24] - USD timeCodesPerSecond for time conversion
 * @returns {Array<THREE.AnimationClip>} Array of Three.js AnimationClips
 */
export function convertUSDSkeletalAnimationsToThreeJS(usdLoader, boneMaps, timeCodesPerSecond = 24) {
	const animationClips = [];

	// Get number of animations
	const numAnimations = usdLoader.numAnimations();
	console.log(`Found ${numAnimations} animations in USD file`);

	// Get summary of all animations
	const animationInfos = usdLoader.getAllAnimationInfos();
	console.log('Animation summaries:', animationInfos);

	// Convert each animation to Three.js format
	for (let i = 0; i < numAnimations; i++) {
		const usdAnimation = usdLoader.getAnimation(i);
		console.log(`Processing animation ${i}: ${usdAnimation.name}`);

		if (!usdAnimation.channels || !usdAnimation.samplers) {
			console.warn(`Animation ${i} missing channels or samplers`);
			continue;
		}

		const samplers = Array.isArray(usdAnimation.samplers)
			? usdAnimation.samplers.slice()
			: [];
		const expandedArrayChannels = expandArraySkeletalChannels(usdAnimation, samplers);

		// Filter for skeletal animations only (skip node animations)
		const skeletalChannels = usdAnimation.channels.filter(channel => {
			const targetType = channel.target_type || 'SceneNode';
			return targetType === 'SkeletonJoint';
		}).concat(expandedArrayChannels);

		if (skeletalChannels.length === 0) {
			console.log(`Animation ${i} has no SkeletonJoint channels (skipping node-only animation)`);
			continue;
		}

		console.log(`Animation ${i}: ${skeletalChannels.length} skeletal channels (${usdAnimation.channels.length - skeletalChannels.length} node channels skipped)`);

		// Create Three.js KeyframeTracks from USD skeletal animation channels
		const keyframeTracks = [];

		// Group channels by (skeleton_id, joint_id) to combine TRS into hierarchical bone animation
		const jointChannels = new Map();
		const skelIdsInAnimation = new Set();
		for (const channel of skeletalChannels) {
			const skelId = channel.skeleton_id !== undefined ? channel.skeleton_id : 0;
			const jointId = channel.joint_id;
			skelIdsInAnimation.add(skelId);
			const key = `${skelId}_${jointId}`;
			if (!jointChannels.has(key)) {
				jointChannels.set(key, { skeleton_id: skelId, joint_id: jointId, channels: {} });
			}
			const joint = jointChannels.get(key);
			joint.channels[channel.path] = channel;
		}
		console.log(`Animation ${i} (${usdAnimation.name}): targets skeleton IDs: ${Array.from(skelIdsInAnimation).sort().join(', ')}`);

		// Process each joint's animation
		// Animation values are absolute joint-local transforms from SkelAnimation.
		// Bones are positioned from bindTransforms (Blender-style), and animation
		// replaces the local transform with the animated value (Normal blend mode).
		// skinningTransform = bone.matrixWorld * inverse(bindTransform) is correct
		// because bone.matrixWorld is computed by composing animLocal up the hierarchy.
		for (const [key, jointData] of jointChannels) {
			const skelId = jointData.skeleton_id;
			const jointId = jointData.joint_id;
			const channels = jointData.channels;

			// Get the correct boneMap for this skeleton
			const boneMap = boneMaps.get(skelId);
			if (!boneMap) {
				console.warn(`Could not find boneMap for skeleton_id: ${skelId}`);
				continue;
			}

			const bone = boneMap.get(jointId);
			if (!bone) {
				console.warn(`Could not find bone for skeleton_id: ${skelId}, joint_id: ${jointId}`);
				continue;
			}

			const boneName = bone.name || `bone_${jointId}`;

			// Process Translation channel
			if (channels.Translation) {
				const channel = channels.Translation;
				const sampler = samplers[channel.sampler];
				if (sampler && sampler.times && sampler.values) {
					// Copy WASM typed_memory_view arrays into JS-owned buffers.
					// usd_scene.delete() frees C++ data, invalidating views.
					// Keep times in timeCodes (frames), not seconds - mixer handles frame-based playback
					const track = new THREE.VectorKeyframeTrack(
						`${boneName}.position`,
						toOwnedFloat32Array(sampler.times, 'sampler.times'),
						toOwnedFloat32Array(sampler.values, 'sampler.values'),
						getUSDInterpolationMode(sampler.interpolation)
					);
					keyframeTracks.push(track);
				}
			}

			// Process Rotation channel
			if (channels.Rotation) {
				const channel = channels.Rotation;
				const sampler = samplers[channel.sampler];
				if (sampler && sampler.times && sampler.values) {
					// Copy WASM typed_memory_view arrays into JS-owned buffers.
					// Keep times in timeCodes (frames), not seconds
					const track = new THREE.QuaternionKeyframeTrack(
						`${boneName}.quaternion`,
						toOwnedFloat32Array(sampler.times, 'sampler.times'),
						toOwnedFloat32Array(sampler.values, 'sampler.values'),
						getUSDInterpolationMode(sampler.interpolation)
					);
					keyframeTracks.push(track);
				}
			}

			// Process Scale channel
			if (channels.Scale) {
				const channel = channels.Scale;
				const sampler = samplers[channel.sampler];
				if (sampler && sampler.times && sampler.values) {
					// Copy WASM typed_memory_view arrays into JS-owned buffers.
					// Keep times in timeCodes (frames), not seconds
					const track = new THREE.VectorKeyframeTrack(
						`${boneName}.scale`,
						toOwnedFloat32Array(sampler.times, 'sampler.times'),
						toOwnedFloat32Array(sampler.values, 'sampler.values'),
						getUSDInterpolationMode(sampler.interpolation)
					);
					keyframeTracks.push(track);
				}
			}
		}

		// Expand single-keyframe tracks to span the full duration.
		// Three.js doesn't extrapolate/hold tracks beyond their keyframe range.
		// A track with times=[0] only applies at exactly t=0; at t>0 the mixer
		// resets the property to its default (0 for position, identity for quat).
		// USD constant attributes (non-time-sampled translations/scales) produce
		// single-keyframe samplers that must span the full clip to stay applied.
		if (keyframeTracks.length > 0) {
			// Find the max time across all tracks to determine the effective duration
			let maxTime = usdAnimation.duration || 0;
			for (const track of keyframeTracks) {
				const lastTime = track.times[track.times.length - 1];
				if (lastTime > maxTime) maxTime = lastTime;
			}

			if (maxTime > 0) {
				for (let t = 0; t < keyframeTracks.length; t++) {
					const track = keyframeTracks[t];
					if (track.times.length === 1 && track.times[0] < maxTime) {
						// Duplicate the single keyframe at the end of the clip
						const stride = track.getValueSize();
						const newTimes = new Float32Array([track.times[0], maxTime]);
						const newValues = new Float32Array(stride * 2);
						for (let s = 0; s < stride; s++) {
							newValues[s] = track.values[s];
							newValues[stride + s] = track.values[s];
						}
						track.times = newTimes;
						track.values = newValues;
					}
				}
			}
		}

		// Create Three.js AnimationClip
		if (keyframeTracks.length > 0) {
			// Let Three.js auto-calculate duration from the actual track keyframes (use -1)
			// This ensures we get the full animation range from the keyframe data
			const clip = new THREE.AnimationClip(
				usdAnimation.name || `SkeletalAnimation_${i}`,
				-1,  // Auto-calculate from tracks to get actual keyframe range
				keyframeTracks
			);

			animationClips.push(clip);
			console.log(`Created skeletal clip: ${clip.name}, duration: ${clip.duration} frames (auto-calc from tracks), USD duration: ${usdAnimation.duration || 'N/A'}`);
		}
	}

	return animationClips;
}

/**
 * Build a node index map by DFS-traversing a Three.js scene node.
 * Must be called BEFORE bones are added to the hierarchy, since adding
 * bones would shift DFS indices and break the mapping to USD node indices.
 * @param {THREE.Object3D} threeNode - Root of the Three.js scene hierarchy
 * @returns {Map<number, THREE.Object3D>} Map from DFS index to Three.js object
 */
export function buildNodeIndexMap(threeNode) {
	const nodeIndexMap = new Map();
	let nodeIndex = 0;
	threeNode.traverse((obj) => {
		nodeIndexMap.set(nodeIndex, obj);
		nodeIndex++;
	});
	return nodeIndexMap;
}

/**
 * Convert USD SceneNode animation data to Three.js AnimationClips.
 * Extracts xformOp animations (translate/rotate/scale) on scene graph nodes
 * such as SkelRoot or Xform ancestors of Skeletons.
 *
 * With AttachedBindMode, animated ancestor transforms are handled correctly:
 * bindMatrixInverse = inv(mesh.matrixWorld) updates each frame, so the
 * ancestor transform cancels out in the skinning equation.
 *
 * @param {Object} usdLoader - LightUSD loader instance
 * @param {Map<number, THREE.Object3D>} nodeIndexMap - Pre-built DFS index → Object3D map
 * @returns {Array<THREE.AnimationClip>} Array of Three.js AnimationClips for node animations
 */
export function convertUSDNodeAnimationsToThreeJS(usdLoader, nodeIndexMap) {
	const animationClips = [];

	const numAnimations = usdLoader.numAnimations();

	for (let i = 0; i < numAnimations; i++) {
		const usdAnimation = usdLoader.getAnimation(i);

		if (!usdAnimation.channels || !usdAnimation.samplers) {
			continue;
		}

		// Filter for SceneNode channels only (inverse of skeletal filter)
		const nodeChannels = usdAnimation.channels.filter(channel => {
			const targetType = channel.target_type || 'SceneNode';
			return targetType === 'SceneNode';
		});

		if (nodeChannels.length === 0) {
			continue;
		}

		const keyframeTracks = [];

		for (const channel of nodeChannels) {
			const sampler = usdAnimation.samplers[channel.sampler];
			if (!sampler || !sampler.times || !sampler.values) {
				continue;
			}

			const targetObject = nodeIndexMap.get(channel.target_node);
			if (!targetObject) {
				console.warn(`Node animation: target_node ${channel.target_node} not found in nodeIndexMap`);
				continue;
			}

			// Matrix-driven nodes (core backend groups set matrix +
			// matrixAutoUpdate=false) ignore the position/quaternion/scale
			// that AnimationMixer writes. Decompose once and switch the
			// target back to TRS-driven updates so tracks take effect.
			if (targetObject.matrixAutoUpdate === false) {
				targetObject.matrix.decompose(
					targetObject.position, targetObject.quaternion, targetObject.scale
				);
				targetObject.matrixAutoUpdate = true;
			}

			// Use UUID for reliable hierarchical animation targeting
			const targetUUID = targetObject.uuid;
			const interpolation = getUSDInterpolationMode(sampler.interpolation);
			// Copy WASM typed_memory_view arrays into JS-owned buffers
			const times = toOwnedFloat32Array(sampler.times, 'sampler.times');
			const values = toOwnedFloat32Array(sampler.values, 'sampler.values');

			let track;
			switch (channel.path) {
				case 'Translation':
					track = new THREE.VectorKeyframeTrack(
						`${targetUUID}.position`, times, values, interpolation
					);
					break;
				case 'Rotation':
					track = new THREE.QuaternionKeyframeTrack(
						`${targetUUID}.quaternion`, times, values, interpolation
					);
					break;
				case 'Scale':
					track = new THREE.VectorKeyframeTrack(
						`${targetUUID}.scale`, times, values, interpolation
					);
					break;
				default:
					continue;
			}

			keyframeTracks.push(track);
		}

		// Expand single-keyframe tracks to span the full duration
		if (keyframeTracks.length > 0) {
			let maxTime = usdAnimation.duration || 0;
			for (const track of keyframeTracks) {
				const lastTime = track.times[track.times.length - 1];
				if (lastTime > maxTime) maxTime = lastTime;
			}

			if (maxTime > 0) {
				for (const track of keyframeTracks) {
					if (track.times.length === 1 && track.times[0] < maxTime) {
						const stride = track.getValueSize();
						const newTimes = new Float32Array([track.times[0], maxTime]);
						const newValues = new Float32Array(stride * 2);
						for (let s = 0; s < stride; s++) {
							newValues[s] = track.values[s];
							newValues[stride + s] = track.values[s];
						}
						track.times = newTimes;
						track.values = newValues;
					}
				}
			}

			const clip = new THREE.AnimationClip(
				usdAnimation.name ? `${usdAnimation.name}_nodes` : `NodeAnimation_${i}`,
				-1,
				keyframeTracks
			);

			animationClips.push(clip);
			console.log(`Created node animation clip: ${clip.name}, duration: ${clip.duration} frames, ${keyframeTracks.length} tracks (targeting ${nodeChannels.length} SceneNode channels)`);
		}
	}

	if (animationClips.length > 0) {
		console.log(`Extracted ${animationClips.length} node animation clip(s)`);
	}

	return animationClips;
}

/**
 * Convert SkelAnimation blendShapeWeights channels ('Weights') into
 * morphTargetInfluences tracks for meshes that carry morph targets.
 *
 * Weight channels are authored in the animation's blendShapes token order;
 * each mesh maps its own skel:blendShapes tokens (== morph slot names) into
 * that order. Meshes are matched to a channel when at least one primary
 * shape token appears in the channel's blendShapeOrder — skeleton-id
 * matching is not available for unskinned blendshape meshes.
 *
 * In-between shapes occupy their own morph slots (built as
 * "<shape>:<inbetween>" right after the primary slot). A single authored
 * weight w resolves to per-slot influences through the UsdSkel
 * piecewise-linear basis over knots (0, w_ib1, ..., w_ibk, 1), with linear
 * extrapolation at the ends.
 *
 * @param {Object} usdLoader - LightUSD loader / next adapter instance
 * @param {THREE.Object3D} threeRoot - Built three.js scene root
 * @returns {Array<THREE.AnimationClip>} morph animation clips
 */
export function convertUSDMorphAnimationsToThreeJS(usdLoader, threeRoot) {
	const animationClips = [];
	if (!usdLoader || !threeRoot || typeof usdLoader.numAnimations !== 'function') {
		return animationClips;
	}
	const numAnimations = usdLoader.numAnimations();
	if (!numAnimations) return animationClips;

	// Collect morph meshes: slot layout comes from the geometry morph
	// attributes; primary/inbetween structure from userData.usdMesh.
	const morphMeshes = [];
	threeRoot.traverse((obj) => {
		const attrs = obj.geometry?.morphAttributes?.position;
		if (!obj.isMesh || !Array.isArray(attrs) || attrs.length === 0) return;
		const shapeMeta = obj.userData?.usdMesh?.blendShapes;
		if (!Array.isArray(shapeMeta) || shapeMeta.length === 0) return;
		// Rebuild slot indices in build order: primary, then its inbetweens.
		const shapes = [];
		let slot = 0;
		for (const shape of shapeMeta) {
			const entry = { name: shape.name, primarySlot: slot++, inbetweens: [] };
			for (const inbetween of shape.inbetweens || []) {
				if (slot >= attrs.length) break;
				const w = Number(inbetween.weight);
				if (Number.isFinite(w)) entry.inbetweens.push({ slot, weight: w });
				slot++;
			}
			entry.inbetweens.sort((a, b) => a.weight - b.weight);
			shapes.push(entry);
			if (slot >= attrs.length) break;
		}
		morphMeshes.push({ mesh: obj, shapes, slotCount: attrs.length });
	});
	if (!morphMeshes.length) return animationClips;

	// Distribute one authored weight over the shape's morph slots
	// (piecewise-linear UsdSkel in-between basis).
	const applyShapeWeight = (entry, w, out, base) => {
		const knots = [{ w: 0, slot: -1 }];
		for (const ib of entry.inbetweens) knots.push({ w: ib.weight, slot: ib.slot });
		knots.push({ w: 1, slot: entry.primarySlot });
		let j = 0;
		while (j < knots.length - 2 && w > knots[j + 1].w) j++;
		const a = knots[j];
		const b = knots[j + 1];
		const denom = (b.w - a.w) || 1;
		const t = (w - a.w) / denom;
		if (a.slot >= 0) out[base + a.slot] += (1 - t);
		if (b.slot >= 0) out[base + b.slot] += t;
	};

	for (let i = 0; i < numAnimations; i++) {
		const usdAnimation = usdLoader.getAnimation(i);
		if (!usdAnimation?.channels || !usdAnimation.samplers) continue;

		const keyframeTracks = [];
		for (const channel of usdAnimation.channels) {
			if (channel.path !== 'Weights') continue;
			const sampler = usdAnimation.samplers[channel.sampler];
			const order = Array.from(channel.blendShapeOrder || []);
			const times = sampler?.times;
			const values = sampler?.arrayValues;
			const elementCount = Number.isFinite(channel.elementCount) && channel.elementCount > 0
				? channel.elementCount
				: order.length;
			if (!times?.length || !values?.length || !order.length || !elementCount) continue;
			const frameCount = Math.min(times.length, Math.floor(values.length / elementCount));
			if (frameCount <= 0) continue;

			const orderIndex = new Map(order.map((token, idx) => [token, idx]));
			let candidates = morphMeshes.filter(({ shapes }) =>
				shapes.some((shape) => orderIndex.has(shape.name)));
			if (!candidates.length) continue;

			// Scope: shape tokens are not globally unique, so restrict to the
			// meshes under the nearest ancestor of the SkelAnimation prim that
			// contains any token-matching mesh (its SkelRoot in practice). Only
			// when no ancestor scope matches (animation referenced from outside
			// the mesh subtree) fall back to token matching alone.
			const animPath = String(channel.target_prim_path || '');
			const segments = animPath.split('/').filter(Boolean);
			for (let depth = segments.length - 1; depth >= 1; depth--) {
				const prefix = '/' + segments.slice(0, depth).join('/') + '/';
				const scoped = candidates.filter(({ mesh }) => {
					const path = mesh.userData?.usdMesh?.primPath
						|| mesh.userData?.['primMeta.absPath'] || '';
					return path.startsWith(prefix);
				});
				if (scoped.length) { candidates = scoped; break; }
			}

			for (const { mesh, shapes, slotCount } of candidates) {
				const driven = shapes.filter((shape) => orderIndex.has(shape.name));
				if (!driven.length) continue;

				const trackTimes = toOwnedFloat32Array(times, 'weights.times').slice(0, frameCount);
				const trackValues = new Float32Array(frameCount * slotCount);
				for (let f = 0; f < frameCount; f++) {
					const base = f * slotCount;
					for (const shape of driven) {
						const weightIdx = orderIndex.get(shape.name);
						if (weightIdx >= elementCount) continue;
						const w = values[f * elementCount + weightIdx];
						if (Number.isFinite(w)) applyShapeWeight(shape, w, trackValues, base);
					}
				}
				keyframeTracks.push(new THREE.NumberKeyframeTrack(
					`${mesh.uuid}.morphTargetInfluences`,
					trackTimes,
					trackValues,
					getUSDInterpolationMode(sampler.interpolation)
				));
			}
		}

		if (keyframeTracks.length > 0) {
			const clip = new THREE.AnimationClip(
				usdAnimation.name ? `${usdAnimation.name}_morphs` : `MorphAnimation_${i}`,
				-1,
				keyframeTracks
			);
			animationClips.push(clip);
			console.log(`Created morph animation clip: ${clip.name}, duration: ${clip.duration} frames, ${keyframeTracks.length} tracks`);
		}
	}

	return animationClips;
}

export default {
	getUSDInterpolationMode,
	convertUSDSkeletalAnimationsToThreeJS,
	buildNodeIndexMap,
	convertUSDNodeAnimationsToThreeJS,
	convertUSDMorphAnimationsToThreeJS
};
