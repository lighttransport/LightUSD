// Material Validation & Linting System
// Automatically detect common material errors and PBR best practices violations

import * as THREE from 'three';

export class MaterialValidator {
    constructor() {
        this.validationRules = [];
        this.registerDefaultRules();
    }

    // Register default validation rules
    registerDefaultRules() {
        // Energy conservation check
        this.addRule({
            id: 'energy_conservation',
            name: 'Energy Conservation',
            severity: 'warning',
            check: (material) => {
                if (material.color && material.metalness !== undefined) {
                    const maxChannel = Math.max(material.color.r, material.color.g, material.color.b);
                    if (maxChannel * material.metalness > 1.0) {
                        return {
                            pass: false,
                            message: `Base color too bright for metallic material (${(maxChannel * material.metalness).toFixed(2)} > 1.0)`
                        };
                    }
                }
                return { pass: true };
            }
        });

        // IOR range validation
        this.addRule({
            id: 'ior_range',
            name: 'IOR Range',
            severity: 'warning',
            check: (material) => {
                if (material.ior !== undefined) {
                    if (material.ior < 1.0) {
                        return {
                            pass: false,
                            message: `IOR ${material.ior.toFixed(2)} < 1.0 (physically impossible)`
                        };
                    }
                    if (material.ior > 3.0) {
                        return {
                            pass: false,
                            message: `IOR ${material.ior.toFixed(2)} unusually high (typical range: 1.0-3.0)`
                        };
                    }
                }
                return { pass: true };
            }
        });

        // Metallic material with wrong IOR
        this.addRule({
            id: 'metal_ior',
            name: 'Metallic IOR',
            severity: 'info',
            check: (material) => {
                if (material.metalness > 0.8 && material.ior !== undefined) {
                    if (Math.abs(material.ior - 1.5) < 0.1) {
                        return {
                            pass: false,
                            message: 'Metallic material using dielectric IOR (1.5). Metals have complex IOR.'
                        };
                    }
                }
                return { pass: true };
            }
        });

        // Texture dimension checks
        this.addRule({
            id: 'texture_power_of_two',
            name: 'Texture Power of Two',
            severity: 'info',
            check: (material) => {
                const issues = [];
                const textureProps = ['map', 'normalMap', 'roughnessMap', 'metalnessMap', 'aoMap', 'emissiveMap'];

                textureProps.forEach(prop => {
                    if (material[prop] && material[prop].image) {
                        const tex = material[prop];
                        if (!this.isPowerOfTwo(tex.image.width) || !this.isPowerOfTwo(tex.image.height)) {
                            issues.push(`${prop}: ${tex.image.width}×${tex.image.height} (not power-of-2)`);
                        }
                    }
                });

                if (issues.length > 0) {
                    return {
                        pass: false,
                        message: `Non-power-of-2 textures may cause issues:\n${issues.join('\n')}`
                    };
                }
                return { pass: true };
            }
        });

        // Base color map colorspace
        this.addRule({
            id: 'base_color_colorspace',
            name: 'Base Color Colorspace',
            severity: 'error',
            check: (material) => {
                if (material.map) {
                    if (material.map.colorSpace !== THREE.SRGBColorSpace) {
                        return {
                            pass: false,
                            message: 'Base color map should use sRGB encoding/colorspace'
                        };
                    }
                }
                return { pass: true };
            }
        });

        // Normal map colorspace (should be linear)
        this.addRule({
            id: 'normal_map_colorspace',
            name: 'Normal Map Colorspace',
            severity: 'error',
            check: (material) => {
                if (material.normalMap) {
                    if (material.normalMap.colorSpace === THREE.SRGBColorSpace) {
                        return {
                            pass: false,
                            message: 'Normal map incorrectly using sRGB encoding (should be Linear)'
                        };
                    }
                }
                return { pass: true };
            }
        });

        // Data texture colorspace (roughness, metalness, AO)
        this.addRule({
            id: 'data_texture_colorspace',
            name: 'Data Texture Colorspace',
            severity: 'error',
            check: (material) => {
                const dataTextures = ['roughnessMap', 'metalnessMap', 'aoMap'];
                const issues = [];

                dataTextures.forEach(prop => {
                    if (material[prop]) {
                        if (material[prop].colorSpace === THREE.SRGBColorSpace) {
                            issues.push(`${prop} using sRGB (should be Linear)`);
                        }
                    }
                });

                if (issues.length > 0) {
                    return {
                        pass: false,
                        message: `Data textures incorrectly using sRGB:\n${issues.join('\n')}`
                    };
                }
                return { pass: true };
            }
        });

        // Missing normal map suggestion
        this.addRule({
            id: 'missing_normal_map',
            name: 'Missing Normal Map',
            severity: 'info',
            check: (material) => {
                if ((material.roughnessMap || material.metalnessMap) && !material.normalMap) {
                    return {
                        pass: false,
                        message: 'Material has PBR maps but no normal map. Consider adding one for more detail.'
                    };
                }
                return { pass: true };
            }
        });

        // Roughness zero (perfect mirror)
        this.addRule({
            id: 'zero_roughness',
            name: 'Zero Roughness',
            severity: 'info',
            check: (material) => {
                if (material.roughness !== undefined && material.roughness < 0.01) {
                    return {
                        pass: false,
                        message: 'Roughness near zero (perfect mirror). Real materials have some roughness (>0.01).'
                    };
                }
                return { pass: true };
            }
        });

        // Metalness intermediate values warning
        this.addRule({
            id: 'intermediate_metalness',
            name: 'Intermediate Metalness',
            severity: 'info',
            check: (material) => {
                if (material.metalness !== undefined) {
                    if (material.metalness > 0.1 && material.metalness < 0.9) {
                        return {
                            pass: false,
                            message: `Metalness ${material.metalness.toFixed(2)} is intermediate. Should usually be 0 (dielectric) or 1 (metal).`
                        };
                    }
                }
                return { pass: true };
            }
        });

        // Very bright base color
        this.addRule({
            id: 'bright_base_color',
            name: 'Bright Base Color',
            severity: 'warning',
            check: (material) => {
                if (material.color) {
                    const maxChannel = Math.max(material.color.r, material.color.g, material.color.b);
                    if (maxChannel > 0.95 && material.metalness < 0.5) {
                        return {
                            pass: false,
                            message: `Base color very bright (${maxChannel.toFixed(2)}). Most dielectrics have albedo < 0.9.`
                        };
                    }
                }
                return { pass: true };
            }
        });

        // Very dark base color
        this.addRule({
            id: 'dark_base_color',
            name: 'Dark Base Color',
            severity: 'info',
            check: (material) => {
                if (material.color && material.metalness > 0.8) {
                    const avgChannel = (material.color.r + material.color.g + material.color.b) / 3.0;
                    if (avgChannel < 0.5) {
                        return {
                            pass: false,
                            message: `Dark base color for metal (avg ${avgChannel.toFixed(2)}). Metals are usually brighter.`
                        };
                    }
                }
                return { pass: true };
            }
        });
    }

    // Add custom validation rule
    addRule(rule) {
        this.validationRules.push(rule);
    }

    // Check if number is power of two
    isPowerOfTwo(n) {
        return n > 0 && (n & (n - 1)) === 0;
    }

    // Validate a single material
    validate(material) {
        const results = {
            material: material.name || 'Unnamed',
            errors: [],
            warnings: [],
            info: [],
            passedCount: 0,
            failedCount: 0
        };

        this.validationRules.forEach(rule => {
            try {
                const result = rule.check(material);

                if (!result.pass) {
                    results.failedCount++;
                    const issue = {
                        rule: rule.name,
                        message: result.message,
                        severity: rule.severity
                    };

                    if (rule.severity === 'error') {
                        results.errors.push(issue);
                    } else if (rule.severity === 'warning') {
                        results.warnings.push(issue);
                    } else {
                        results.info.push(issue);
                    }
                } else {
                    results.passedCount++;
                }
            } catch (error) {
                console.error(`Error running validation rule ${rule.id}:`, error);
            }
        });

        return results;
    }

    // Validate all materials in a scene
    validateScene(scene) {
        const sceneResults = {
            totalMaterials: 0,
            validatedMaterials: 0,
            totalErrors: 0,
            totalWarnings: 0,
            totalInfo: 0,
            materials: []
        };

        const materialsSet = new Set();

        scene.traverse(obj => {
            if (obj.isMesh && obj.material) {
                if (!materialsSet.has(obj.material.uuid)) {
                    materialsSet.add(obj.material.uuid);
                    sceneResults.totalMaterials++;

                    const result = this.validate(obj.material);
                    sceneResults.validatedMaterials++;
                    sceneResults.totalErrors += result.errors.length;
                    sceneResults.totalWarnings += result.warnings.length;
                    sceneResults.totalInfo += result.info.length;
                    sceneResults.materials.push(result);
                }
            }
        });

        return sceneResults;
    }

    // Generate validation report
    generateReport(sceneResults) {
        let report = '# Material Validation Report\n\n';
        report += `**Total Materials**: ${sceneResults.totalMaterials}\n`;
        report += `**Validated**: ${sceneResults.validatedMaterials}\n`;
        report += `**Errors**: ${sceneResults.totalErrors}\n`;
        report += `**Warnings**: ${sceneResults.totalWarnings}\n`;
        report += `**Info**: ${sceneResults.totalInfo}\n\n`;

        report += '---\n\n';

        sceneResults.materials.forEach(materialResult => {
            report += `## ${materialResult.material}\n\n`;
            report += `Passed: ${materialResult.passedCount} | Failed: ${materialResult.failedCount}\n\n`;

            if (materialResult.errors.length > 0) {
                report += '### ❌ Errors\n';
                materialResult.errors.forEach(err => {
                    report += `- **${err.rule}**: ${err.message}\n`;
                });
                report += '\n';
            }

            if (materialResult.warnings.length > 0) {
                report += '### ⚠️ Warnings\n';
                materialResult.warnings.forEach(warn => {
                    report += `- **${warn.rule}**: ${warn.message}\n`;
                });
                report += '\n';
            }

            if (materialResult.info.length > 0) {
                report += '### ℹ️ Info\n';
                materialResult.info.forEach(info => {
                    report += `- **${info.rule}**: ${info.message}\n`;
                });
                report += '\n';
            }

            report += '---\n\n';
        });

        return report;
    }

    // Display validation results in console
    logResults(sceneResults) {
        console.group('🔍 Material Validation Results');
        console.log(`Materials: ${sceneResults.validatedMaterials}/${sceneResults.totalMaterials}`);
        console.log(`❌ Errors: ${sceneResults.totalErrors}`);
        console.log(`⚠️ Warnings: ${sceneResults.totalWarnings}`);
        console.log(`ℹ️ Info: ${sceneResults.totalInfo}`);

        sceneResults.materials.forEach(materialResult => {
            const hasIssues = materialResult.errors.length > 0 ||
                            materialResult.warnings.length > 0 ||
                            materialResult.info.length > 0;

            if (hasIssues) {
                console.group(`Material: ${materialResult.material}`);

                materialResult.errors.forEach(err => {
                    console.error(`❌ ${err.rule}: ${err.message}`);
                });

                materialResult.warnings.forEach(warn => {
                    console.warn(`⚠️ ${warn.rule}: ${warn.message}`);
                });

                materialResult.info.forEach(info => {
                    console.info(`ℹ️ ${info.rule}: ${info.message}`);
                });

                console.groupEnd();
            }
        });

        console.groupEnd();
    }
}

// Make class globally accessible
if (typeof window !== 'undefined') {
    window.MaterialValidator = MaterialValidator;
}
