// OpenPBRMaterial - Custom Three.js ShaderMaterial implementing OpenPBR specification
// Implements rasterizer-friendly OpenPBR features including Oren-Nayar diffuse,
// coat with color/IOR, and OpenPBR sheen formulation.

import * as THREE from 'three';

// ============================================================================
// Vertex Shader
// ============================================================================

const openpbrVertexShader = `
#define STANDARD

// Custom varyings not provided by Three.js chunks
varying vec3 vViewPosition;
varying vec3 vWorldPosition;

#include <common>
#include <uv_pars_vertex>
#include <envmap_pars_vertex>
#include <color_pars_vertex>
#include <fog_pars_vertex>
#include <normal_pars_vertex>
#include <morphtarget_pars_vertex>
#include <skinning_pars_vertex>
#include <shadowmap_pars_vertex>
#include <logdepthbuf_pars_vertex>
#include <clipping_planes_pars_vertex>

void main() {
    #include <uv_vertex>
    #include <color_vertex>

    #include <beginnormal_vertex>
    #include <morphnormal_vertex>
    #include <skinbase_vertex>
    #include <skinnormal_vertex>
    #include <defaultnormal_vertex>
    #include <normal_vertex>

    #include <begin_vertex>
    #include <morphtarget_vertex>
    #include <skinning_vertex>
    #include <project_vertex>
    #include <logdepthbuf_vertex>
    #include <clipping_planes_vertex>

    vViewPosition = -mvPosition.xyz;
    vWorldPosition = (modelMatrix * vec4(transformed, 1.0)).xyz;

    #include <worldpos_vertex>
    #include <envmap_vertex>
    #include <shadowmap_vertex>
    #include <fog_vertex>
}
`;

// ============================================================================
// Fragment Shader
// ============================================================================

const openpbrFragmentShader = `
#define STANDARD

// Custom varyings not provided by Three.js chunks
varying vec3 vViewPosition;
varying vec3 vWorldPosition;

// Include Three.js chunks first - they define uniforms like map, normalMap, normalScale, envMapIntensity, etc.
#include <common>
#include <packing>
#include <dithering_pars_fragment>
#include <color_pars_fragment>
#include <uv_pars_fragment>
#include <map_pars_fragment>
#include <alphamap_pars_fragment>
#include <alphatest_pars_fragment>
#include <aomap_pars_fragment>
#include <lightmap_pars_fragment>
#include <envmap_common_pars_fragment>
#include <envmap_pars_fragment>
#include <fog_pars_fragment>
#include <lights_pars_begin>
#include <normal_pars_fragment>
#include <shadowmap_pars_fragment>
#include <bumpmap_pars_fragment>
#include <normalmap_pars_fragment>
#include <logdepthbuf_pars_fragment>
#include <clipping_planes_pars_fragment>

// OpenPBR uniforms (custom, not in Three.js chunks)
uniform vec3 diffuse; // Alias for base_color for Three.js compatibility
uniform float opacity;

// OpenPBR Base Layer
uniform float base_weight;
uniform vec3 base_color;
uniform float base_metalness;
uniform float base_diffuse_roughness;

// OpenPBR Specular Layer
uniform float specular_weight;
uniform vec3 specular_color;
uniform float specular_roughness;
uniform float specular_ior;
uniform float specular_anisotropy;
uniform float specular_rotation;

// OpenPBR Coat Layer
uniform float coat_weight;
uniform vec3 coat_color;
uniform float coat_roughness;
uniform float coat_ior;

// OpenPBR Fuzz Layer (Sheen)
uniform float fuzz_weight;
uniform vec3 fuzz_color;
uniform float fuzz_roughness;

// OpenPBR Thin Film
uniform float thin_film_weight;
uniform float thin_film_thickness;
uniform float thin_film_ior;

// OpenPBR Transmission (simplified)
uniform float transmission_weight;
uniform vec3 transmission_color;

// OpenPBR Emission
uniform float emission_luminance;
uniform vec3 emission_color;

// OpenPBR Geometry
uniform float geometry_opacity;

// Texture maps not provided by Three.js chunks
uniform sampler2D roughnessMap;
uniform sampler2D metalnessMap;
uniform sampler2D emissiveMap;

// ============================================================================
// OpenPBR BRDF Functions
// ============================================================================

// IOR to F0 conversion
float iorToF0(float ior) {
    float r = (ior - 1.0) / (ior + 1.0);
    return r * r;
}

// Fresnel Schlick
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel Schlick with roughness (for IBL)
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// GGX Normal Distribution Function
float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

// Smith GGX Geometry Function (height-correlated)
float geometrySmithGGX1(float NdotX, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotX2 = NdotX * NdotX;
    return 2.0 * NdotX / (NdotX + sqrt(a2 + (1.0 - a2) * NdotX2));
}

float geometrySmithGGX(float NdotV, float NdotL, float roughness) {
    return geometrySmithGGX1(NdotV, roughness) * geometrySmithGGX1(NdotL, roughness);
}

// Oren-Nayar diffuse BRDF (for base_diffuse_roughness)
vec3 orenNayarDiffuse(vec3 baseColor, float sigma, float NdotL, float NdotV, float LdotV) {
    float sigma2 = sigma * sigma;
    float A = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
    float B = 0.45 * sigma2 / (sigma2 + 0.09);

    // Compute cos(phi_i - phi_r)
    float cosPhi = (LdotV - NdotL * NdotV) / max(0.001, sqrt((1.0 - NdotL * NdotL) * (1.0 - NdotV * NdotV)));

    // sin(alpha) * tan(beta)
    float sinAlpha = sqrt(1.0 - min(NdotL * NdotL, NdotV * NdotV));
    float tanBeta = sqrt(1.0 - max(NdotL * NdotL, NdotV * NdotV)) / max(0.001, max(NdotL, NdotV));

    return baseColor / PI * (A + B * max(0.0, cosPhi) * sinAlpha * tanBeta);
}

// Thin film interference (simplified iridescence)
vec3 thinFilmFresnel(float cosTheta, float thickness, float filmIOR, vec3 baseF0, float weight) {
    if (weight <= 0.0) return baseF0;

    // Optical path difference
    float delta = 2.0 * filmIOR * thickness * cosTheta;

    // Phase for RGB (simplified - assumes 650nm, 550nm, 450nm)
    float phaseR = mod(delta / 650.0 * 2.0 * PI, 2.0 * PI);
    float phaseG = mod(delta / 550.0 * 2.0 * PI, 2.0 * PI);
    float phaseB = mod(delta / 450.0 * 2.0 * PI, 2.0 * PI);

    // Interference pattern
    vec3 interference = vec3(
        0.5 + 0.5 * cos(phaseR),
        0.5 + 0.5 * cos(phaseG),
        0.5 + 0.5 * cos(phaseB)
    );

    return mix(baseF0, interference * baseF0 + interference * 0.5, weight);
}

// OpenPBR Fuzz/Sheen BRDF (inverted Fresnel model)
vec3 fuzzBRDF(vec3 fuzzColor, float fuzzRoughness, float NdotV, float NdotL) {
    // OpenPBR fuzz uses inverted Fresnel - stronger at grazing angles
    float sheenFactor = pow(1.0 - NdotV, 3.0) * pow(1.0 - NdotL, 0.5);

    // Roughness affects the spread
    float spread = mix(1.0, 0.5, fuzzRoughness);

    return fuzzColor * sheenFactor * spread;
}

// ============================================================================
// Main Fragment
// ============================================================================

void main() {
    #include <clipping_planes_fragment>

    // Face direction for double-sided rendering and normal mapping
    float faceDirection = gl_FrontFacing ? 1.0 : -1.0;

    // Initialize output
    vec4 diffuseColor = vec4(base_color, geometry_opacity);

    // Sample base color texture
    #ifdef USE_MAP
        vec4 texelColor = texture2D(map, vUv);
        diffuseColor *= texelColor;
    #endif

    vec3 albedo = diffuseColor.rgb;
    float alpha = diffuseColor.a;

    // Sample metalness
    float metalness = base_metalness;
    #ifdef USE_METALNESSMAP
        metalness *= texture2D(metalnessMap, vUv).b;
    #endif

    // Sample roughness
    float roughness = specular_roughness;
    #ifdef USE_ROUGHNESSMAP
        roughness *= texture2D(roughnessMap, vUv).g;
    #endif
    roughness = max(roughness, 0.04); // Minimum roughness

    // Normal - use geometric normal from vertex shader (via normal_pars_fragment)
    vec3 normal = normalize(vNormal);
    vec3 N = normal * faceDirection;

    #ifdef USE_NORMALMAP
        vec3 mapN = texture2D(normalMap, vUv).xyz * 2.0 - 1.0;
        mapN.xy *= normalScale;

        #ifdef USE_TANGENT
            mat3 TBN = mat3(normalize(vTangent), normalize(vBitangent), N);
            N = normalize(TBN * mapN);
        #else
            N = perturbNormal2Arb(-vViewPosition, N, mapN, faceDirection);
        #endif
    #endif

    // View direction
    vec3 V = normalize(cameraPosition - vWorldPosition);
    float NdotV = max(dot(N, V), 0.001);

    // Calculate F0
    float dielectricF0 = iorToF0(specular_ior);
    vec3 F0 = mix(vec3(dielectricF0), albedo, metalness);

    // Apply thin film if enabled
    F0 = thinFilmFresnel(NdotV, thin_film_thickness, thin_film_ior, F0, thin_film_weight);

    // Accumulate lighting
    vec3 Lo = vec3(0.0);

    // ========================================================================
    // Direct Lighting
    // ========================================================================

    #if ( NUM_DIR_LIGHTS > 0 )
        DirectionalLight directionalLight;

        #pragma unroll_loop_start
        for (int i = 0; i < NUM_DIR_LIGHTS; i++) {
            directionalLight = directionalLights[i];

            vec3 L = normalize(directionalLight.direction);
            vec3 H = normalize(V + L);

            float NdotL = max(dot(N, L), 0.0);
            float NdotH = max(dot(N, H), 0.0);
            float VdotH = max(dot(V, H), 0.0);
            float LdotV = dot(L, V);

            if (NdotL > 0.0) {
                // Specular BRDF
                float D = distributionGGX(NdotH, roughness);
                float G = geometrySmithGGX(NdotV, NdotL, roughness);
                vec3 F = fresnelSchlick(VdotH, F0);

                vec3 specular = specular_weight * specular_color * (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);

                // Diffuse BRDF
                vec3 diffuse;
                if (base_diffuse_roughness > 0.0) {
                    // Oren-Nayar for rough diffuse
                    diffuse = orenNayarDiffuse(albedo, base_diffuse_roughness, NdotL, NdotV, LdotV);
                } else {
                    // Lambertian
                    diffuse = albedo / PI;
                }
                diffuse *= base_weight * (1.0 - metalness) * (1.0 - F);

                // Fuzz/Sheen layer
                vec3 fuzz = vec3(0.0);
                if (fuzz_weight > 0.0) {
                    fuzz = fuzz_weight * fuzzBRDF(fuzz_color, fuzz_roughness, NdotV, NdotL);
                }

                // Combine and apply light
                vec3 radiance = directionalLight.color;
                Lo += (diffuse + specular + fuzz) * radiance * NdotL;
            }
        }
        #pragma unroll_loop_end
    #endif

    #if ( NUM_POINT_LIGHTS > 0 )
        PointLight pointLight;

        #pragma unroll_loop_start
        for (int i = 0; i < NUM_POINT_LIGHTS; i++) {
            pointLight = pointLights[i];

            vec3 lVector = pointLight.position - vWorldPosition;
            float lightDistance = length(lVector);
            vec3 L = normalize(lVector);
            vec3 H = normalize(V + L);

            float NdotL = max(dot(N, L), 0.0);
            float NdotH = max(dot(N, H), 0.0);
            float VdotH = max(dot(V, H), 0.0);
            float LdotV = dot(L, V);

            if (NdotL > 0.0) {
                float attenuation = 1.0 / (lightDistance * lightDistance);

                // Specular BRDF
                float D = distributionGGX(NdotH, roughness);
                float G = geometrySmithGGX(NdotV, NdotL, roughness);
                vec3 F = fresnelSchlick(VdotH, F0);

                vec3 specular = specular_weight * specular_color * (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);

                // Diffuse BRDF
                vec3 diffuse;
                if (base_diffuse_roughness > 0.0) {
                    diffuse = orenNayarDiffuse(albedo, base_diffuse_roughness, NdotL, NdotV, LdotV);
                } else {
                    diffuse = albedo / PI;
                }
                diffuse *= base_weight * (1.0 - metalness) * (1.0 - F);

                // Fuzz layer
                vec3 fuzz = vec3(0.0);
                if (fuzz_weight > 0.0) {
                    fuzz = fuzz_weight * fuzzBRDF(fuzz_color, fuzz_roughness, NdotV, NdotL);
                }

                vec3 radiance = pointLight.color * attenuation;
                Lo += (diffuse + specular + fuzz) * radiance * NdotL;
            }
        }
        #pragma unroll_loop_end
    #endif

    // ========================================================================
    // Image-Based Lighting (IBL)
    // ========================================================================

    #ifdef USE_ENVMAP
        vec3 R = reflect(-V, N);

        // Sample environment map
        #ifdef ENVMAP_TYPE_CUBE
            vec3 prefilteredColor = textureCube(envMap, R).rgb;
            vec3 irradiance = textureCube(envMap, N).rgb;
        #else
            vec3 prefilteredColor = texture2D(envMap, equirectUv(R)).rgb;
            vec3 irradiance = texture2D(envMap, equirectUv(N)).rgb;
        #endif

        prefilteredColor *= envMapIntensity;
        irradiance *= envMapIntensity;

        // Fresnel for IBL
        vec3 F_ibl = fresnelSchlickRoughness(NdotV, F0, roughness);

        // Specular IBL
        vec3 specularIBL = prefilteredColor * F_ibl * specular_weight;

        // Diffuse IBL
        vec3 diffuseIBL = irradiance * albedo * base_weight * (1.0 - metalness) * (1.0 - F_ibl);

        Lo += specularIBL + diffuseIBL;
    #endif

    // ========================================================================
    // Coat Layer
    // ========================================================================

    if (coat_weight > 0.0) {
        float coatF0 = iorToF0(coat_ior);
        vec3 coatF = fresnelSchlick(NdotV, vec3(coatF0));

        // Coat attenuates underlying layers
        Lo = Lo * (1.0 - coat_weight * coatF.r);

        // Add coat specular (simplified - uses same roughness-based NDF)
        // For proper implementation, would need separate coat roughness evaluation
        #ifdef USE_ENVMAP
            vec3 R = reflect(-V, N);
            #ifdef ENVMAP_TYPE_CUBE
                vec3 coatReflection = textureCube(envMap, R).rgb * envMapIntensity;
            #else
                vec3 coatReflection = texture2D(envMap, equirectUv(R)).rgb * envMapIntensity;
            #endif
            Lo += coat_color * coatF * coatReflection * coat_weight;
        #else
            Lo += coat_color * coatF * coat_weight * 0.1; // Fallback ambient
        #endif
    }

    // ========================================================================
    // Emission
    // ========================================================================

    vec3 emission = emission_color * emission_luminance;
    #ifdef USE_EMISSIVEMAP
        emission *= texture2D(emissiveMap, vUv).rgb;
    #endif
    Lo += emission;

    // ========================================================================
    // Ambient Occlusion
    // ========================================================================

    #ifdef USE_AOMAP
        float ao = (texture2D(aoMap, vUv).r - 1.0) * aoMapIntensity + 1.0;
        Lo *= ao;
    #endif

    // ========================================================================
    // Final Output
    // ========================================================================

    vec3 outColor = Lo;

    #include <tonemapping_fragment>
    #include <colorspace_fragment>
    #include <fog_fragment>
    #include <premultiplied_alpha_fragment>
    #include <dithering_fragment>

    gl_FragColor = vec4(outColor, alpha);
}
`;

// ============================================================================
// OpenPBRMaterial Class
// ============================================================================

export class OpenPBRMaterial extends THREE.ShaderMaterial {
    constructor(parameters = {}) {
        super();

        this.type = 'OpenPBRMaterial';
        this.isOpenPBRMaterial = true;
        this.isMeshStandardMaterial = true; // For Three.js compatibility
        this.envMapRotation = new THREE.Euler();
        this.aoMapIntensity = 1;
        this.lightMapIntensity = 1;
        this.reflectivity = 0.5;
        this.refractionRatio = 0.98;
        this.normalMapType = THREE.TangentSpaceNormalMap;

        // Define uniforms
        this.uniforms = THREE.UniformsUtils.merge([
            // isMeshStandardMaterial enables PMREM/environment integration.
            // Three also refreshes the standard uniform set for that flag,
            // even though this material uses a custom fragment shader.
            THREE.ShaderLib.standard.uniforms,
            THREE.UniformsLib.envmap,
            THREE.UniformsLib.normalmap,
            THREE.UniformsLib.lights,
            THREE.UniformsLib.fog,
            {
                // Base compatibility
                diffuse: { value: new THREE.Color(0.8, 0.8, 0.8) },
                opacity: { value: 1.0 },

                // OpenPBR Base Layer
                base_weight: { value: 1.0 },
                base_color: { value: new THREE.Color(0.8, 0.8, 0.8) },
                base_metalness: { value: 0.0 },
                base_diffuse_roughness: { value: 0.0 },

                // OpenPBR Specular Layer
                specular_weight: { value: 1.0 },
                specular_color: { value: new THREE.Color(1.0, 1.0, 1.0) },
                specular_roughness: { value: 0.3 },
                specular_ior: { value: 1.5 },
                specular_anisotropy: { value: 0.0 },
                specular_rotation: { value: 0.0 },

                // OpenPBR Coat Layer
                coat_weight: { value: 0.0 },
                coat_color: { value: new THREE.Color(1.0, 1.0, 1.0) },
                coat_roughness: { value: 0.0 },
                coat_ior: { value: 1.5 },

                // OpenPBR Fuzz Layer
                fuzz_weight: { value: 0.0 },
                fuzz_color: { value: new THREE.Color(1.0, 1.0, 1.0) },
                fuzz_roughness: { value: 0.5 },

                // OpenPBR Thin Film
                thin_film_weight: { value: 0.0 },
                thin_film_thickness: { value: 500.0 },
                thin_film_ior: { value: 1.5 },

                // OpenPBR Transmission (simplified)
                transmission_weight: { value: 0.0 },
                transmission_color: { value: new THREE.Color(1.0, 1.0, 1.0) },

                // OpenPBR Emission
                emission_luminance: { value: 0.0 },
                emission_color: { value: new THREE.Color(1.0, 1.0, 1.0) },

                // OpenPBR Geometry
                geometry_opacity: { value: 1.0 },

                // Texture maps
                map: { value: null },
                normalMap: { value: null },
                normalScale: { value: new THREE.Vector2(1, 1) },
                roughnessMap: { value: null },
                metalnessMap: { value: null },
                emissiveMap: { value: null },
                aoMap: { value: null },
                aoMapIntensity: { value: 1.0 },

                // Environment
                envMapIntensity: { value: 1.0 }
            }
        ]);

        this.vertexShader = openpbrVertexShader;
        this.fragmentShader = openpbrFragmentShader;

        this.lights = true;
        this.fog = true;

        this.defines = {};

        // Apply parameters
        this.setValues(parameters);

        // Sync color with base_color
        if (parameters.color) {
            this.uniforms.base_color.value.copy(parameters.color);
            this.uniforms.diffuse.value.copy(parameters.color);
        }
    }

    // ========================================================================
    // Property Getters/Setters for Three.js Compatibility
    // ========================================================================

    get color() { return this.uniforms.base_color.value; }
    set color(v) {
        this.uniforms.base_color.value.copy(v);
        this.uniforms.diffuse.value.copy(v);
    }

    get metalness() { return this.uniforms.base_metalness.value; }
    set metalness(v) { this.uniforms.base_metalness.value = v; }

    get roughness() { return this.uniforms.specular_roughness.value; }
    set roughness(v) { this.uniforms.specular_roughness.value = v; }

    get ior() { return this.uniforms.specular_ior.value; }
    set ior(v) { this.uniforms.specular_ior.value = v; }

    get clearcoat() { return this.uniforms.coat_weight.value; }
    set clearcoat(v) { this.uniforms.coat_weight.value = v; }

    get clearcoatRoughness() { return this.uniforms.coat_roughness.value; }
    set clearcoatRoughness(v) { this.uniforms.coat_roughness.value = v; }

    get sheen() { return this.uniforms.fuzz_weight.value; }
    set sheen(v) { this.uniforms.fuzz_weight.value = v; }

    get sheenColor() { return this.uniforms.fuzz_color.value; }
    set sheenColor(v) { this.uniforms.fuzz_color.value.copy(v); }

    get sheenRoughness() { return this.uniforms.fuzz_roughness.value; }
    set sheenRoughness(v) { this.uniforms.fuzz_roughness.value = v; }

    get iridescence() { return this.uniforms.thin_film_weight.value; }
    set iridescence(v) { this.uniforms.thin_film_weight.value = v; }

    get iridescenceIOR() { return this.uniforms.thin_film_ior.value; }
    set iridescenceIOR(v) { this.uniforms.thin_film_ior.value = v; }

    get emissive() { return this.uniforms.emission_color.value; }
    set emissive(v) { this.uniforms.emission_color.value.copy(v); }

    get emissiveIntensity() { return this.uniforms.emission_luminance.value; }
    set emissiveIntensity(v) { this.uniforms.emission_luminance.value = v; }

    get envMap() { return this.uniforms.envMap.value; }
    set envMap(v) {
        this.uniforms.envMap.value = v;
        if (v) {
            this.defines.USE_ENVMAP = '';
            if (v.isCubeTexture) {
                this.defines.ENVMAP_TYPE_CUBE = '';
            }
        } else {
            delete this.defines.USE_ENVMAP;
            delete this.defines.ENVMAP_TYPE_CUBE;
        }
        this.needsUpdate = true;
    }

    get envMapIntensity() { return this.uniforms.envMapIntensity.value; }
    set envMapIntensity(v) { this.uniforms.envMapIntensity.value = v; }

    get map() { return this.uniforms.map.value; }
    set map(v) {
        this.uniforms.map.value = v;
        if (v) {
            this.defines.USE_MAP = '';
            this.defines.USE_UV = '';
        } else {
            delete this.defines.USE_MAP;
        }
        this.needsUpdate = true;
    }

    get normalScale() { return this.uniforms.normalScale.value; }
    set normalScale(v) { this.uniforms.normalScale.value.copy(v); }

    get normalMap() { return this.uniforms.normalMap.value; }
    set normalMap(v) {
        this.uniforms.normalMap.value = v;
        if (v) {
            this.defines.USE_NORMALMAP = '';
            this.defines.USE_UV = '';
        } else {
            delete this.defines.USE_NORMALMAP;
        }
        this.needsUpdate = true;
    }

    get roughnessMap() { return this.uniforms.roughnessMap.value; }
    set roughnessMap(v) {
        this.uniforms.roughnessMap.value = v;
        if (v) {
            this.defines.USE_ROUGHNESSMAP = '';
            this.defines.USE_UV = '';
        } else {
            delete this.defines.USE_ROUGHNESSMAP;
        }
        this.needsUpdate = true;
    }

    get metalnessMap() { return this.uniforms.metalnessMap.value; }
    set metalnessMap(v) {
        this.uniforms.metalnessMap.value = v;
        if (v) {
            this.defines.USE_METALNESSMAP = '';
            this.defines.USE_UV = '';
        } else {
            delete this.defines.USE_METALNESSMAP;
        }
        this.needsUpdate = true;
    }

    get emissiveMap() { return this.uniforms.emissiveMap.value; }
    set emissiveMap(v) {
        this.uniforms.emissiveMap.value = v;
        if (v) {
            this.defines.USE_EMISSIVEMAP = '';
            this.defines.USE_UV = '';
        } else {
            delete this.defines.USE_EMISSIVEMAP;
        }
        this.needsUpdate = true;
    }

    get aoMap() { return this.uniforms.aoMap.value; }
    set aoMap(v) {
        this.uniforms.aoMap.value = v;
        if (v) {
            this.defines.USE_AOMAP = '';
            this.defines.USE_UV = '';
        } else {
            delete this.defines.USE_AOMAP;
        }
        this.needsUpdate = true;
    }

    // ========================================================================
    // OpenPBR-Specific Properties
    // ========================================================================

    get baseWeight() { return this.uniforms.base_weight.value; }
    set baseWeight(v) { this.uniforms.base_weight.value = v; }

    get baseDiffuseRoughness() { return this.uniforms.base_diffuse_roughness.value; }
    set baseDiffuseRoughness(v) { this.uniforms.base_diffuse_roughness.value = v; }

    get specularWeight() { return this.uniforms.specular_weight.value; }
    set specularWeight(v) { this.uniforms.specular_weight.value = v; }

    get specularColor() { return this.uniforms.specular_color.value; }
    set specularColor(v) { this.uniforms.specular_color.value.copy(v); }

    get coatWeight() { return this.uniforms.coat_weight.value; }
    set coatWeight(v) { this.uniforms.coat_weight.value = v; }

    get coatColor() { return this.uniforms.coat_color.value; }
    set coatColor(v) { this.uniforms.coat_color.value.copy(v); }

    get coatIor() { return this.uniforms.coat_ior.value; }
    set coatIor(v) { this.uniforms.coat_ior.value = v; }

    get fuzzWeight() { return this.uniforms.fuzz_weight.value; }
    set fuzzWeight(v) { this.uniforms.fuzz_weight.value = v; }

    get fuzzColor() { return this.uniforms.fuzz_color.value; }
    set fuzzColor(v) { this.uniforms.fuzz_color.value.copy(v); }

    get thinFilmWeight() { return this.uniforms.thin_film_weight.value; }
    set thinFilmWeight(v) { this.uniforms.thin_film_weight.value = v; }

    get thinFilmThickness() { return this.uniforms.thin_film_thickness.value; }
    set thinFilmThickness(v) { this.uniforms.thin_film_thickness.value = v; }

    // ========================================================================
    // Clone
    // ========================================================================

    clone() {
        const material = new OpenPBRMaterial();

        // Copy uniforms
        for (const key in this.uniforms) {
            const uniform = this.uniforms[key];
            if (uniform.value && uniform.value.clone) {
                material.uniforms[key].value = uniform.value.clone();
            } else {
                material.uniforms[key].value = uniform.value;
            }
        }

        // Copy defines
        material.defines = { ...this.defines };

        // Copy other properties
        material.transparent = this.transparent;
        material.side = this.side;
        material.depthTest = this.depthTest;
        material.depthWrite = this.depthWrite;

        return material;
    }
}

// Export for global access
if (typeof window !== 'undefined') {
    window.OpenPBRMaterial = OpenPBRMaterial;
}
