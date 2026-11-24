// Light Probe Visualizer
// Visualize and inspect environment maps (IBL / Image-Based Lighting)

import * as THREE from 'three';

export class LightProbeVisualizer {
    constructor(scene, renderer) {
        this.scene = scene;
        this.renderer = renderer;
        this.enabled = false;

        // Visualization objects
        this.spherePreview = null;
        this.skyboxPreview = null;
        this.debugPanel = null;

        // Settings
        this.visualizationMode = 'sphere'; // 'sphere', 'skybox', 'split'
        this.spherePosition = { x: 2, y: 1, z: 0 };
        this.sphereSize = 0.5;
        this.showMipLevels = false;
        this.currentMipLevel = 0;
        this.maxMipLevels = 0;
    }

    // Enable visualizer
    enable() {
        this.enabled = true;
        this.createVisualization();
    }

    // Disable visualizer
    disable() {
        this.enabled = false;
        this.removeVisualization();
    }

    // Create visualization objects
    createVisualization() {
        // Get environment map from scene
        const envMap = this.getSceneEnvironmentMap();
        if (!envMap) {
            console.warn('No environment map found in scene');
            return;
        }

        // Calculate mip levels
        if (envMap.image && envMap.image.width) {
            this.maxMipLevels = Math.floor(Math.log2(envMap.image.width)) + 1;
        }

        switch (this.visualizationMode) {
            case 'sphere':
                this.createSpherePreview(envMap);
                break;
            case 'skybox':
                this.createSkyboxPreview(envMap);
                break;
            case 'split':
                this.createSpherePreview(envMap);
                this.createSkyboxPreview(envMap);
                break;
        }
    }

    // Create sphere preview
    createSpherePreview(envMap) {
        if (this.spherePreview) {
            this.scene.remove(this.spherePreview);
            this.spherePreview.geometry.dispose();
            this.spherePreview.material.dispose();
        }

        const geometry = new THREE.SphereGeometry(this.sphereSize, 64, 64);
        const material = new THREE.MeshStandardMaterial({
            envMap: envMap,
            metalness: 1.0,
            roughness: this.showMipLevels ? (this.currentMipLevel / this.maxMipLevels) : 0.0,
            color: 0xffffff,
            envMapIntensity: 1.0
        });

        this.spherePreview = new THREE.Mesh(geometry, material);
        this.spherePreview.position.set(
            this.spherePosition.x,
            this.spherePosition.y,
            this.spherePosition.z
        );
        this.spherePreview.name = 'LightProbeVisualizerSphere';

        this.scene.add(this.spherePreview);
    }

    // Create skybox preview
    createSkyboxPreview(envMap) {
        if (this.skyboxPreview) {
            this.scene.remove(this.skyboxPreview);
            this.skyboxPreview.geometry.dispose();
            this.skyboxPreview.material.dispose();
        }

        const geometry = new THREE.SphereGeometry(500, 60, 40);
        const material = new THREE.ShaderMaterial({
            uniforms: {
                envMap: { value: envMap },
                mipLevel: { value: this.currentMipLevel }
            },
            vertexShader: `
                varying vec3 vWorldDirection;

                void main() {
                    vec4 worldPosition = modelMatrix * vec4(position, 1.0);
                    vWorldDirection = worldPosition.xyz - cameraPosition;
                    gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
                }
            `,
            fragmentShader: `
                uniform samplerCube envMap;
                uniform float mipLevel;
                varying vec3 vWorldDirection;

                void main() {
                    vec3 direction = normalize(vWorldDirection);

                    #ifdef USE_MIP_LEVEL
                        vec4 color = textureCubeLodEXT(envMap, direction, mipLevel);
                    #else
                        vec4 color = textureCube(envMap, direction);
                    #endif

                    gl_FragColor = vec4(color.rgb, 1.0);
                }
            `,
            side: THREE.BackSide,
            depthWrite: false
        });

        if (this.showMipLevels) {
            material.defines = { USE_MIP_LEVEL: '' };
            material.extensions = { derivatives: true };
        }

        this.skyboxPreview = new THREE.Mesh(geometry, material);
        this.skyboxPreview.name = 'LightProbeVisualizerSkybox';
        this.scene.add(this.skyboxPreview);
    }

    // Remove visualization objects
    removeVisualization() {
        if (this.spherePreview) {
            this.scene.remove(this.spherePreview);
            this.spherePreview.geometry.dispose();
            this.spherePreview.material.dispose();
            this.spherePreview = null;
        }

        if (this.skyboxPreview) {
            this.scene.remove(this.skyboxPreview);
            this.skyboxPreview.geometry.dispose();
            this.skyboxPreview.material.dispose();
            this.skyboxPreview = null;
        }
    }

    // Get environment map from scene
    getSceneEnvironmentMap() {
        // Check scene environment
        if (this.scene.environment) {
            return this.scene.environment;
        }

        // Check scene background
        if (this.scene.background && this.scene.background.isTexture) {
            return this.scene.background;
        }

        // Check materials for envMap
        let foundEnvMap = null;
        this.scene.traverse(obj => {
            if (obj.isMesh && obj.material && obj.material.envMap) {
                foundEnvMap = obj.material.envMap;
            }
        });

        return foundEnvMap;
    }

    // Set visualization mode
    setVisualizationMode(mode) {
        if (['sphere', 'skybox', 'split'].includes(mode)) {
            this.visualizationMode = mode;
            if (this.enabled) {
                this.removeVisualization();
                this.createVisualization();
            }
        }
    }

    // Set sphere position
    setSpherePosition(x, y, z) {
        this.spherePosition = { x, y, z };
        if (this.spherePreview) {
            this.spherePreview.position.set(x, y, z);
        }
    }

    // Set sphere size
    setSphereSize(size) {
        this.sphereSize = size;
        if (this.spherePreview && this.enabled) {
            this.removeVisualization();
            this.createVisualization();
        }
    }

    // Toggle mip level visualization
    setShowMipLevels(show) {
        this.showMipLevels = show;
        if (this.enabled) {
            this.removeVisualization();
            this.createVisualization();
        }
    }

    // Set current mip level
    setMipLevel(level) {
        this.currentMipLevel = Math.max(0, Math.min(level, this.maxMipLevels - 1));
        if (this.enabled) {
            this.removeVisualization();
            this.createVisualization();
        }
    }

    // Analyze environment map
    analyzeEnvironmentMap() {
        const envMap = this.getSceneEnvironmentMap();
        if (!envMap) {
            return {
                hasEnvironmentMap: false,
                error: 'No environment map found'
            };
        }

        const analysis = {
            hasEnvironmentMap: true,
            type: envMap.mapping === THREE.CubeReflectionMapping ? 'Cubemap' :
                  envMap.mapping === THREE.EquirectangularReflectionMapping ? 'Equirectangular' :
                  'Unknown',
            mapping: envMap.mapping,
            format: this.getTextureFormat(envMap.format),
            encoding: this.getTextureEncoding(envMap.colorSpace),
            dataType: this.getTextureType(envMap.type),
            generateMipmaps: envMap.generateMipmaps,
            minFilter: this.getTextureFilter(envMap.minFilter),
            magFilter: this.getTextureFilter(envMap.magFilter),
            wrapS: this.getTextureWrapping(envMap.wrapS),
            wrapT: this.getTextureWrapping(envMap.wrapT),
            flipY: envMap.flipY,
            mipmaps: envMap.mipmaps ? envMap.mipmaps.length : 0
        };

        // Analyze dimensions
        if (envMap.image) {
            if (Array.isArray(envMap.image) && envMap.image.length === 6) {
                // Cubemap
                const face = envMap.image[0];
                analysis.width = face.width;
                analysis.height = face.height;
                analysis.faces = 6;
            } else if (envMap.image.width && envMap.image.height) {
                // Single image (equirectangular)
                analysis.width = envMap.image.width;
                analysis.height = envMap.image.height;
                analysis.faces = 1;
            }
        }

        // Calculate theoretical mip levels
        if (analysis.width) {
            analysis.maxMipLevels = Math.floor(Math.log2(Math.max(analysis.width, analysis.height))) + 1;
        }

        // Check if HDR
        analysis.isHDR = envMap.type === THREE.HalfFloatType ||
                         envMap.type === THREE.FloatType;

        return analysis;
    }

    // Helper: Get texture format name
    getTextureFormat(format) {
        const formats = {
            [THREE.RGBAFormat]: 'RGBA',
            [THREE.RGBFormat]: 'RGB',
            [THREE.RedFormat]: 'Red',
            [THREE.RGFormat]: 'RG',
            [THREE.DepthFormat]: 'Depth'
        };
        return formats[format] || 'Unknown';
    }

    // Helper: Get texture encoding name
    getTextureEncoding(encoding) {
        const encodings = {
            [THREE.LinearSRGBColorSpace]: 'Linear',
            [THREE.NoColorSpace]: 'Data',
            [THREE.SRGBColorSpace]: 'sRGB'
        };
        return encodings[encoding] || 'Unknown';
    }

    // Helper: Get texture type name
    getTextureType(type) {
        const types = {
            [THREE.UnsignedByteType]: 'UnsignedByte',
            [THREE.ByteType]: 'Byte',
            [THREE.FloatType]: 'Float',
            [THREE.HalfFloatType]: 'HalfFloat',
            [THREE.UnsignedShortType]: 'UnsignedShort',
            [THREE.ShortType]: 'Short'
        };
        return types[type] || 'Unknown';
    }

    // Helper: Get texture filter name
    getTextureFilter(filter) {
        const filters = {
            [THREE.NearestFilter]: 'Nearest',
            [THREE.LinearFilter]: 'Linear',
            [THREE.NearestMipmapNearestFilter]: 'NearestMipmapNearest',
            [THREE.LinearMipmapNearestFilter]: 'LinearMipmapNearest',
            [THREE.NearestMipmapLinearFilter]: 'NearestMipmapLinear',
            [THREE.LinearMipmapLinearFilter]: 'LinearMipmapLinear'
        };
        return filters[filter] || 'Unknown';
    }

    // Helper: Get texture wrapping name
    getTextureWrapping(wrap) {
        const wrappings = {
            [THREE.RepeatWrapping]: 'Repeat',
            [THREE.ClampToEdgeWrapping]: 'ClampToEdge',
            [THREE.MirroredRepeatWrapping]: 'MirroredRepeat'
        };
        return wrappings[wrap] || 'Unknown';
    }

    // Generate report
    generateReport() {
        const analysis = this.analyzeEnvironmentMap();

        if (!analysis.hasEnvironmentMap) {
            return '# Light Probe Analysis\n\n**Status**: No environment map found in scene.\n';
        }

        let report = '# Light Probe Analysis\n\n';
        report += `**Type**: ${analysis.type}\n`;

        if (analysis.width && analysis.height) {
            report += `**Resolution**: ${analysis.width}×${analysis.height}\n`;
        }

        if (analysis.faces) {
            report += `**Faces**: ${analysis.faces}\n`;
        }

        report += `**Format**: ${analysis.format}\n`;
        report += `**Data Type**: ${analysis.dataType}\n`;
        report += `**Encoding**: ${analysis.encoding}\n`;
        report += `**HDR**: ${analysis.isHDR ? 'Yes' : 'No'}\n\n`;

        report += '## Texture Settings\n\n';
        report += `**Min Filter**: ${analysis.minFilter}\n`;
        report += `**Mag Filter**: ${analysis.magFilter}\n`;
        report += `**Wrap S**: ${analysis.wrapS}\n`;
        report += `**Wrap T**: ${analysis.wrapT}\n`;
        report += `**Generate Mipmaps**: ${analysis.generateMipmaps}\n`;
        report += `**Flip Y**: ${analysis.flipY}\n\n`;

        if (analysis.maxMipLevels) {
            report += `## Mip Levels\n\n`;
            report += `**Max Theoretical Levels**: ${analysis.maxMipLevels}\n`;
            report += `**Stored Mipmaps**: ${analysis.mipmaps}\n\n`;
        }

        report += '## Recommendations\n\n';
        if (!analysis.isHDR) {
            report += '⚠️ **LDR Environment Map**: Consider using HDR (EXR/HDR format) for better lighting quality\n';
        }
        if (!analysis.generateMipmaps && analysis.minFilter.includes('Mipmap')) {
            report += '⚠️ **Mipmaps Not Generated**: Enable generateMipmaps or use non-mipmap filter\n';
        }
        if (analysis.encoding === 'sRGB') {
            report += 'ℹ️ **sRGB Encoding**: Environment maps should typically use Linear encoding for correct lighting\n';
        }

        return report;
    }

    // Log analysis to console
    logAnalysis() {
        const analysis = this.analyzeEnvironmentMap();

        console.group('🌍 Light Probe Analysis');

        if (!analysis.hasEnvironmentMap) {
            console.warn('No environment map found');
            console.groupEnd();
            return;
        }

        console.log(`Type: ${analysis.type}`);
        if (analysis.width && analysis.height) {
            console.log(`Resolution: ${analysis.width}×${analysis.height}`);
        }
        console.log(`Format: ${analysis.format}`);
        console.log(`Data Type: ${analysis.dataType}`);
        console.log(`Encoding: ${analysis.encoding}`);
        console.log(`HDR: ${analysis.isHDR}`);

        console.group('Texture Settings');
        console.log(`Min Filter: ${analysis.minFilter}`);
        console.log(`Mag Filter: ${analysis.magFilter}`);
        console.log(`Generate Mipmaps: ${analysis.generateMipmaps}`);
        console.log(`Max Mip Levels: ${analysis.maxMipLevels || 'N/A'}`);
        console.groupEnd();

        console.groupEnd();
    }
}

// Make class globally accessible
if (typeof window !== 'undefined') {
    window.LightProbeVisualizer = LightProbeVisualizer;
}
