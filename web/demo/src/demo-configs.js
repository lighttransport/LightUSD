export const DEMOS = [
  {
    id: 'hair-fur',
    title: 'USD Hair / Fur Rendering',
    subtitle: 'BasisCurves, MaterialX Principled Hair, furballs, and grass in a lightweight WebGL renderer.',
    // Generate or select a local hair scene; large demo assets are not shipped.
    defaultAsset: '',
    preferredMaterialType: 'auto',
    materialModeLabel: 'MaterialX Chiang Hair BSDF approximation',
    requiredBackend: 'next',
    preserveBuildData: true,
    image: './assets/previews/hair-fur.svg',
    href: './hair-fur.html'
  },
  {
    id: 'materialx-openchess',
    title: 'MaterialX OpenChess Rendering',
    subtitle: 'OpenChessSet across lightweight raster and progressive reference renderers.',
    defaultAsset: './assets/openchess/chess_set.usda',
    externalAssetIndex: './assets/openchess/asset-index.json',
    preferredMaterialType: 'openpbr',
    materialModeLabel: 'MaterialX Standard Surface / OpenPBR',
    useComposition: true,
    useUsdLux: true,
    requiredBackend: 'next',
    openChessRendering: true,
    image: './assets/previews/materialx-openchess.svg',
    href: './materialx-openchess.html'
  },
  {
    id: 'materialx-node',
    title: 'MaterialX Node Graph',
    subtitle: 'OpenPBR / MaterialX material path with a compact node graph inspector.',
    defaultAsset: './assets/fancy-teapot-mtlx.usdz',
    preferredMaterialType: 'openpbr',
    materialBackend: 'nodegraph',
    materialModeLabel: 'OpenPBR node graph',
    enableMaterialGraph: true,
    useUsdLux: true,
    image: './assets/previews/materialx-node.jpg',
    href: './materialx-node.html'
  },
  {
    id: 'materialx-physical',
    title: 'MaterialX MeshPhysicalMaterial',
    subtitle: 'MaterialX / USD material data converted to Three.js MeshPhysicalMaterial.',
    defaultAsset: './assets/fancy-teapot-mtlx.usdz',
    preferredMaterialType: 'usdpreviewsurface',
    materialBackend: 'meshphysical',
    materialModeLabel: 'MeshPhysicalMaterial',
    useUsdLux: true,
    environmentSelector: true,
    defaultEnvironment: 'off',
    image: './assets/previews/materialx-physical.jpg',
    href: './materialx-physical.html'
  },
  {
    id: 'usdlux',
    title: 'USDLux Lighting',
    subtitle: 'Loads UsdLux lights, DomeLight environments, and direct light data.',
    defaultAsset: './assets/envmap-constant-test.usdz',
    preferredMaterialType: 'auto',
    useUsdLux: true,
    useDefaultLights: false,
    image: './assets/previews/usdlux.jpg',
    href: './usdlux.html'
  },
  {
    id: 'metahuman',
    title: 'MetaHuman WebGL2 Rendering',
    subtitle: 'Local UE MetaHuman export with UsdSkel, blendshapes, MaterialX skin SSS, and strand hair.',
    // A tiny public proxy validates the complete path; the real character is
    // selected through the export-folder picker and is never shipped here.
    defaultAsset: './assets/metahuman-fixture/MetaHuman_Hero.usda',
    preferredMaterialType: 'openpbr',
    requiredBackend: 'next',
    openFileLabel: 'Open MetaHuman USDZ',
    enableFolderInput: true,
    useComposition: true,
    enableSkinning: true,
    enableAnimation: true,
    preserveBuildData: true,
    materialModeLabel: 'MaterialX/OpenPBR skin and Chiang-hair approximations',
    image: './assets/previews/hair-fur.svg',
    href: './metahuman.html'
  },
  {
    id: 'skinning',
    title: 'Skinning',
    subtitle: 'Builds skeletons and binds CesiumMan USDZ skinned meshes with helpers visible.',
    defaultAsset: 'https://raw.githubusercontent.com/usd-wg/assets/refs/heads/main/test_assets/USDZ/CesiumMan/CesiumMan.usdz',
    preferredMaterialType: 'auto',
    enableSkinning: true,
    showSkeleton: true,
    image: './assets/previews/skinning.jpg',
    href: './skinning.html'
  },
  {
    id: 'xform-skinning-animation',
    title: 'Node Xform + SkelAnimation',
    subtitle: 'Plays node xform animation together with skeletal animation clips.',
    defaultAsset: './assets/multi-clip-skeleton.usda',
    preferredMaterialType: 'auto',
    enableSkinning: true,
    showSkeleton: true,
    enableAnimation: true,
    image: './assets/previews/xform-skinning-animation.jpg',
    href: './xform-skinning-animation.html'
  },
  {
    id: 'physics',
    title: 'USD Physics',
    subtitle: 'Loads a physics-authored robot arm scene for inspection and rendering.',
    defaultAsset: './assets/physics-robot-arm.usda',
    preferredMaterialType: 'auto',
    image: './assets/previews/physics.jpg',
    href: './physics.html'
  },
  {
    id: 'asset-resolver',
    title: 'Asset Resolver Textures',
    subtitle: 'Loads a textured cat plane and resolves referenced texture assets.',
    defaultAsset: './assets/texture-cat-plane.usda',
    preferredMaterialType: 'usdpreviewsurface',
    image: './assets/previews/asset-resolver.jpg',
    href: './asset-resolver.html'
  },
  {
    id: 'composition',
    title: 'USD Composition',
    subtitle: 'Composes sublayers and renders two Suzanne assets from one root layer.',
    defaultAsset: './assets/usd-composite-sample.usda',
    preferredMaterialType: 'auto',
    useComposition: true,
    image: './assets/previews/composition.jpg',
    href: './composition.html'
  },
  {
    id: 'export',
    title: 'USD Export',
    subtitle: 'Loads USD and exports the current native scene as USDA, USDC, or USDZ.',
    defaultAsset: './assets/suzanne-pbr.usda',
    preferredMaterialType: 'auto',
    useLayerExport: true,
    enableExport: true,
    image: './assets/previews/export.jpg',
    href: './export.html'
  },
  {
    id: 'usd-assets',
    title: 'USD Assets Browser',
    subtitle: 'Browse and preview curated USD assets from the usd-wg/assets corpus over HTTP.',
    defaultAsset: '',
    preferredMaterialType: 'auto',
    image: './assets/previews/usd-assets.svg',
    href: './usd-assets.html'
  },
  {
    id: 'usd-physics',
    title: 'USD Physics + MuJoCo',
    subtitle: 'Simulates a USD physics robotic arm with MuJoCo WASM physics. Interactive joint servo control.',
    defaultAsset: './assets/physics-robot-arm.usda',
    preferredMaterialType: 'auto',
    image: './assets/previews/usd-physics.svg',
    href: './usd-physics.html'
  },
  {
    id: 'material-editor',
    title: 'Material Editor',
    subtitle: 'Load a USD scene and edit PBR material parameters (color, metalness, roughness, clearcoat, IOR) in real time.',
    defaultAsset: './assets/suzanne-pbr.usda',
    preferredMaterialType: 'usdpreviewsurface',
    image: './assets/previews/material-editor.svg',
    href: './material-editor.html'
  },
  {
    id: 'animation-timeline',
    title: 'Animation Timeline',
    subtitle: 'Scrub through USD skeletal and node animations with play/pause, speed control, clip switching, and looping.',
    defaultAsset: './assets/multi-clip-skeleton.usda',
    preferredMaterialType: 'auto',
    enableSkinning: true,
    showSkeleton: true,
    enableAnimation: true,
    image: './assets/previews/animation-timeline.svg',
    href: './animation-timeline.html'
  },
  {
    id: 'usd-inspector',
    title: 'USD Inspector',
    subtitle: 'Explore USD scene structure: prim hierarchy tree, stage metadata, mesh properties, and material parameters.',
    defaultAsset: './assets/suzanne-pbr.usda',
    preferredMaterialType: 'auto',
    image: './assets/previews/usd-inspector.svg',
    href: './usd-inspector.html'
  },
  {
    id: 'composition-viz',
    title: 'Composition Layer Viz',
    subtitle: 'Visualizes how USD composition arcs (subLayers, references, payloads, overrides) stack to produce the final scene.',
    defaultAsset: './assets/usd-composite-sample.usda',
    preferredMaterialType: 'auto',
    useComposition: true,
    image: './assets/previews/composition-viz.svg',
    href: './composition-viz.html'
  },
  {
    id: 'streaming-viz',
    title: 'Streaming Loading Viz',
    subtitle: 'Visualizes WASM heap timeline, HTTP fetch waterfall, and per-phase memory breakdown during USD asset loading.',
    defaultAsset: '',
    preferredMaterialType: 'auto',
    image: './assets/previews/streaming-viz.svg',
    href: './streaming-viz.html'
  },
  {
    id: 'viewer-toolkit',
    title: 'Viewer Toolkit',
    subtitle: 'Full-featured USD viewer with wireframe/flat/shaded/textured modes, exposure control, wireframe overlay, backface culling, and display toggles.',
    defaultAsset: './assets/suzanne-pbr.usda',
    preferredMaterialType: 'auto',
    image: './assets/previews/viewer-toolkit.svg',
    href: './viewer-toolkit.html'
  },
  {
    id: 'usd-viewer',
    title: 'Online USD Viewer',
    subtitle: 'Drag a USD file or folder, compose references first, defer payloads, and preview UsdPreviewSurface or MaterialX shading with animation playback.',
    defaultAsset: '',
    preferredMaterialType: 'auto',
    image: './assets/previews/usd-viewer.svg',
    href: './usd-viewer.html'
  },
  {
    id: 'animation-blending',
    title: 'Animation Blending',
    subtitle: 'Crossfade and blend between USD skeletal animation clips with per-clip weights, timeline scrub, and smooth transitions.',
    defaultAsset: './assets/multi-clip-skeleton.usda',
    preferredMaterialType: 'auto',
    enableSkinning: true,
    showSkeleton: true,
    enableAnimation: true,
    image: './assets/previews/animation-blending.svg',
    href: './animation-blending.html'
  },
  {
    id: 'procedural-usd',
    title: 'Procedural USD Builder',
    subtitle: 'Build USD scenes from JSON with a library of shapes (cube, sphere, cylinder, cone, torus, capsule) and materials.',
    defaultAsset: '',
    preferredMaterialType: 'auto',
    image: './assets/previews/procedural-usd.svg',
    href: './procedural-usd.html'
  },
  {
    id: 'usdz-packager',
    title: 'USDZ Packager',
    subtitle: 'Load a USD scene, inspect embedded textures and assets, and export as USDA, USDC, or USDZ with size breakdown.',
    defaultAsset: './assets/suzanne-pbr.usda',
    preferredMaterialType: 'auto',
    image: './assets/previews/usdz-packager.svg',
    href: './usdz-packager.html'
  },
  {
    id: 'usd-diff',
    title: 'USD Diff',
    subtitle: 'Compare two USD files side by side with synchronized camera and detailed metric differences.',
    defaultAsset: '',
    preferredMaterialType: 'auto',
    image: './assets/previews/usd-diff.svg',
    href: './usd-diff.html'
  },
  {
    id: 'backend-compare',
    title: 'Backend Comparison',
    subtitle: 'Compare legacy vs next rendering backends side by side with synchronized cameras and per-backend stats.',
    defaultAsset: './assets/suzanne-pbr.usda',
    preferredMaterialType: 'auto',
    image: './assets/previews/backend-compare.svg',
    href: './backend-compare.html'
  }
];

export const DEMO_BY_ID = Object.fromEntries(DEMOS.map((demo) => [demo.id, demo]));
