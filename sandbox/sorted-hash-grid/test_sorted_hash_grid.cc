#include "sorted_hash_grid.hh"
#include <iostream>
#include <random>
#include <chrono>
#include <iomanip>
#include <cassert>

using namespace lightusd::legacy::spatial;

void testBasicFunctionality() {
    std::cout << "Testing basic functionality...\n";

    SortedHashGrid<float> grid(0.1f, 2);

    grid.addVertex(0.0f, 0.0f, 0.0f, 0);
    grid.addVertex(0.05f, 0.05f, 0.05f, 1);
    grid.addVertex(0.5f, 0.5f, 0.5f, 2);
    grid.addVertex(1.0f, 1.0f, 1.0f, 3);

    grid.build();

    auto results = grid.findSimilarVertices(0.03f, 0.03f, 0.03f, 0.1f);

    std::cout << "  Found " << results.size() << " similar vertices near (0.03, 0.03, 0.03)\n";
    for (const auto& r : results) {
        std::cout << "    Vertex ID: " << r.vertexId
                  << ", Distance²: " << r.distanceSquared << "\n";
    }

    assert(results.size() >= 2);
    std::cout << "  ✓ Basic search passed\n\n";
}

void testExactMatch() {
    std::cout << "Testing exact match...\n";

    SortedHashGrid<float> grid(0.01f);

    grid.addVertex(0.5f, 0.5f, 0.5f, 100);
    grid.addVertex(0.5f, 0.5f, 0.5f, 101);
    grid.addVertex(0.50001f, 0.5f, 0.5f, 102);

    grid.build();

    auto exact = grid.findExactVertex(0.5f, 0.5f, 0.5f, 1e-6f);

    std::cout << "  Found " << exact.size() << " exact matches at (0.5, 0.5, 0.5)\n";
    for (uint32_t id : exact) {
        std::cout << "    Vertex ID: " << id << "\n";
    }

    assert(exact.size() == 2);
    std::cout << "  ✓ Exact match test passed\n\n";
}

void testLargeDataset() {
    std::cout << "Testing large dataset with subdivision...\n";

    const size_t numVertices = 100000;
    const float range = 10.0f;

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(0.0f, range);

    SortedHashGrid<float> grid(0.5f, 128, 0, 3);

    std::cout << "  Adding " << numVertices << " random vertices...\n";
    auto startAdd = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < numVertices; ++i) {
        grid.addVertex(dist(rng), dist(rng), dist(rng), static_cast<uint32_t>(i));
    }

    auto endAdd = std::chrono::high_resolution_clock::now();
    auto addTime = std::chrono::duration_cast<std::chrono::milliseconds>(endAdd - startAdd).count();
    std::cout << "  Added vertices in " << addTime << " ms\n";

    std::cout << "  Building grid...\n";
    auto startBuild = std::chrono::high_resolution_clock::now();
    grid.build();
    auto endBuild = std::chrono::high_resolution_clock::now();
    auto buildTime = std::chrono::duration_cast<std::chrono::milliseconds>(endBuild - startBuild).count();
    std::cout << "  Built grid in " << buildTime << " ms\n";

    size_t totalCells, maxCellSize, avgCellSize, subdivCount;
    grid.getStatistics(totalCells, maxCellSize, avgCellSize, subdivCount);

    std::cout << "  Grid statistics:\n";
    std::cout << "    Total cells: " << totalCells << "\n";
    std::cout << "    Max items per cell: " << maxCellSize << "\n";
    std::cout << "    Avg items per cell: " << avgCellSize << "\n";
    std::cout << "    Subdivisions: " << subdivCount << "\n";

    const int numQueries = 1000;
    std::cout << "\n  Running " << numQueries << " proximity queries...\n";

    auto startQuery = std::chrono::high_resolution_clock::now();
    size_t totalFound = 0;

    for (int i = 0; i < numQueries; ++i) {
        float qx = dist(rng);
        float qy = dist(rng);
        float qz = dist(rng);
        auto results = grid.findSimilarVertices(qx, qy, qz, 0.5f);
        totalFound += results.size();
    }

    auto endQuery = std::chrono::high_resolution_clock::now();
    auto queryTime = std::chrono::duration_cast<std::chrono::microseconds>(endQuery - startQuery).count();

    std::cout << "  Completed " << numQueries << " queries in " << queryTime << " µs\n";
    std::cout << "  Average query time: " << queryTime / numQueries << " µs\n";
    std::cout << "  Average results per query: " << totalFound / numQueries << "\n";
    std::cout << "  ✓ Large dataset test passed\n\n";
}

void testClusteredData() {
    std::cout << "Testing clustered data (triggers subdivision)...\n";

    SortedHashGrid<float> grid(1.0f, 50, 0, 5);

    std::mt19937 rng(123);
    std::normal_distribution<float> cluster1(5.0f, 0.1f);
    std::normal_distribution<float> cluster2(15.0f, 0.1f);

    std::cout << "  Adding clustered vertices...\n";
    uint32_t id = 0;

    for (int i = 0; i < 500; ++i) {
        grid.addVertex(cluster1(rng), cluster1(rng), cluster1(rng), id++);
    }

    for (int i = 0; i < 500; ++i) {
        grid.addVertex(cluster2(rng), cluster2(rng), cluster2(rng), id++);
    }

    grid.build();

    size_t totalCells, maxCellSize, avgCellSize, subdivCount;
    grid.getStatistics(totalCells, maxCellSize, avgCellSize, subdivCount);

    std::cout << "  Clustered grid statistics:\n";
    std::cout << "    Total cells: " << totalCells << "\n";
    std::cout << "    Max items per cell: " << maxCellSize << "\n";
    std::cout << "    Avg items per cell: " << avgCellSize << "\n";
    std::cout << "    Subdivisions triggered: " << subdivCount << "\n";

    auto results1 = grid.findSimilarVertices(5.0f, 5.0f, 5.0f, 0.5f);
    auto results2 = grid.findSimilarVertices(15.0f, 15.0f, 15.0f, 0.5f);

    std::cout << "  Found " << results1.size() << " vertices near cluster 1 center\n";
    std::cout << "  Found " << results2.size() << " vertices near cluster 2 center\n";

    assert(subdivCount > 0);
    std::cout << "  ✓ Subdivision test passed\n\n";
}

void testCustomSimilarity() {
    std::cout << "Testing custom similarity function...\n";

    SortedHashGrid<float> grid(0.1f);

    grid.addVertex(0.0f, 0.0f, 0.0f, 0);
    grid.addVertex(0.05f, 0.0f, 0.0f, 1);
    grid.addVertex(0.0f, 0.05f, 0.0f, 2);
    grid.addVertex(0.0f, 0.0f, 0.05f, 3);
    grid.addVertex(0.05f, 0.05f, 0.05f, 4);

    grid.build();

    auto manhattanSimilarity = [](const SortedHashGrid<float>::Vertex& a,
                                  const SortedHashGrid<float>::Vertex& b,
                                  float threshold) -> bool {
        float manhattanDist = std::abs(a.x - b.x) +
                             std::abs(a.y - b.y) +
                             std::abs(a.z - b.z);
        return manhattanDist <= threshold;
    };

    auto results = grid.findSimilarVertices(0.0f, 0.0f, 0.0f, 0.1f, manhattanSimilarity);

    std::cout << "  Found " << results.size() << " vertices with Manhattan distance <= 0.1\n";
    for (const auto& r : results) {
        const auto& v = grid.getVertex(r.vertexId);
        float manhattan = std::abs(v.x) + std::abs(v.y) + std::abs(v.z);
        std::cout << "    ID: " << r.vertexId
                  << ", Manhattan: " << manhattan
                  << ", Euclidean²: " << r.distanceSquared << "\n";
    }

    std::cout << "  ✓ Custom similarity test passed\n\n";
}

void testMortonCodes() {
    std::cout << "Testing Morton code encoding/decoding...\n";

    for (uint32_t x = 0; x < 100; x += 10) {
        for (uint32_t y = 0; y < 100; y += 10) {
            for (uint32_t z = 0; z < 100; z += 10) {
                uint32_t code = morton3D(x, y, z);
                uint32_t dx, dy, dz;
                decodeMorton3D(code, dx, dy, dz);
                assert(dx == x && dy == y && dz == z);

                uint64_t code64 = morton3D64(x, y, z);
                decodeMorton3D64(code64, dx, dy, dz);
                assert(dx == x && dy == y && dz == z);
            }
        }
    }

    std::cout << "  ✓ Morton code test passed\n\n";
}

int main() {
    std::cout << "=== Sorted Hash Grid Test Suite ===\n\n";

    testMortonCodes();
    testBasicFunctionality();
    testExactMatch();
    testCustomSimilarity();
    testClusteredData();
    testLargeDataset();

    std::cout << "=== All tests passed! ===\n";

    return 0;
}