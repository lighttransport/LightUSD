// Example demonstrating the new triangulation method option in LightUSD Tydra
//
// This example shows how to use either:
// - Earcut algorithm (default): Robust for complex/concave polygons
// - Triangle Fan: Fast for simple convex polygons

#include <iostream>
#include <vector>
#include <string>
#include "lightusd.hh"
#include "tydra/render-data.hh"
#include "tydra/scene-access.hh"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <input.usd> [fan|earcut]\n";
        std::cout << "\nTriangulation methods:\n";
        std::cout << "  earcut (default): Robust algorithm for complex polygons\n";
        std::cout << "  fan: Fast triangle fan for convex polygons\n";
        return 1;
    }

    std::string filename = argv[1];
    bool use_fan = false;

    if (argc >= 3) {
        std::string method = argv[2];
        if (method == "fan") {
            use_fan = true;
            std::cout << "Using triangle fan triangulation\n";
        } else if (method == "earcut") {
            std::cout << "Using earcut triangulation\n";
        } else {
            std::cerr << "Unknown triangulation method: " << method << "\n";
            return 1;
        }
    } else {
        std::cout << "Using default earcut triangulation\n";
    }

    // Load USD file
    lightusd::legacy::Stage stage;
    std::string warn, err;
    bool ret = lightusd::legacy::LoadUSDFromFile(filename, &stage, &warn, &err);

    if (!warn.empty()) {
        std::cerr << "Warning: " << warn << "\n";
    }

    if (!ret) {
        std::cerr << "Failed to load USD file: " << err << "\n";
        return 1;
    }

    // Set up render scene converter with triangulation method
    lightusd::legacy::tydra::RenderSceneConverterEnv env(stage);

    // Configure mesh conversion
    env.mesh_config.triangulate = true;  // Enable triangulation

    // Set the triangulation method
    if (use_fan) {
        env.mesh_config.triangulation_method =
            lightusd::legacy::tydra::MeshConverterConfig::TriangulationMethod::TriangleFan;
    } else {
        env.mesh_config.triangulation_method =
            lightusd::legacy::tydra::MeshConverterConfig::TriangulationMethod::Earcut;
    }

    // Convert to render scene
    lightusd::legacy::tydra::RenderSceneConverter converter;
    lightusd::legacy::tydra::RenderScene render_scene;

    if (!converter.ConvertToRenderScene(env, &render_scene)) {
        std::cerr << "Failed to convert to render scene: "
                  << converter.GetError() << "\n";
        return 1;
    }

    // Print statistics
    std::cout << "\nConversion complete!\n";
    std::cout << "Number of meshes: " << render_scene.meshes.size() << "\n";

    size_t total_triangles = 0;
    size_t total_original_faces = 0;

    for (const auto& mesh : render_scene.meshes) {
        if (!mesh.triangulatedFaceVertexIndices.empty()) {
            size_t num_triangles = mesh.triangulatedFaceVertexIndices.size() / 3;
            size_t num_original_faces = mesh.usdFaceVertexCounts.size();

            total_triangles += num_triangles;
            total_original_faces += num_original_faces;

            std::cout << "\nMesh: " << mesh.prim_name << "\n";
            std::cout << "  Original faces: " << num_original_faces << "\n";
            std::cout << "  Triangulated faces: " << num_triangles << "\n";

            // Count polygon types
            std::map<uint32_t, size_t> poly_counts;
            for (auto count : mesh.usdFaceVertexCounts) {
                poly_counts[count]++;
            }

            std::cout << "  Original polygon distribution:\n";
            for (const auto& kv : poly_counts) {
                std::cout << "    " << kv.first << "-gons: " << kv.second << "\n";
            }
        }
    }

    std::cout << "\nTotal original faces: " << total_original_faces << "\n";
    std::cout << "Total triangles: " << total_triangles << "\n";

    if (use_fan) {
        std::cout << "\nNote: Triangle fan was used for 5+ vertex polygons.\n";
        std::cout << "This is faster but assumes polygons are convex.\n";
    } else {
        std::cout << "\nNote: Earcut algorithm was used for 5+ vertex polygons.\n";
        std::cout << "This handles complex polygons correctly.\n";
    }

    return 0;
}