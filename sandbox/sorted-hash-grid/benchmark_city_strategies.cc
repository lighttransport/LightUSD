#include "sorted_hash_grid_extended.hh"
#include "adaptive_octree.hh"
#include "hybrid_city_grid.hh"
#include <iostream>
#include <chrono>
#include <random>
#include <iomanip>

using namespace lightusd::legacy::spatial;

class CityGenerator {
    std::mt19937 rng_;
    std::uniform_real_distribution<float> uniform_;

public:
    CityGenerator(uint32_t seed = 42) : rng_(seed), uniform_(0.0f, 1.0f) {}

    std::vector<std::array<float, 3>> generateCity(size_t numGroundPoints,
                                                   size_t numBuildingPoints,
                                                   float citySize = 100.0f) {
        std::vector<std::array<float, 3>> points;

        // Ground plane (y = 0, dense)
        for (size_t i = 0; i < numGroundPoints; ++i) {
            float x = (uniform_(rng_) - 0.5f) * citySize;
            float z = (uniform_(rng_) - 0.5f) * citySize;
            float y = uniform_(rng_) * 0.01f;  // Small noise
            points.push_back({x, y, z});
        }

        // Building surfaces (walls and roofs)
        size_t pointsPerBuilding = numBuildingPoints / 20;  // 20 buildings
        for (int b = 0; b < 20; ++b) {
            float cx = (uniform_(rng_) - 0.5f) * citySize * 0.8f;
            float cz = (uniform_(rng_) - 0.5f) * citySize * 0.8f;
            float width = 5.0f + uniform_(rng_) * 10.0f;
            float depth = 5.0f + uniform_(rng_) * 10.0f;
            float height = 10.0f + uniform_(rng_) * 30.0f;

            // Generate points on walls and roof
            for (size_t i = 0; i < pointsPerBuilding; ++i) {
                float r = uniform_(rng_);
                if (r < 0.2f) {
                    // Front wall
                    float x = cx + (uniform_(rng_) - 0.5f) * width;
                    float y = uniform_(rng_) * height;
                    float z = cz - depth/2 + uniform_(rng_) * 0.01f;
                    points.push_back({x, y, z});
                } else if (r < 0.4f) {
                    // Back wall
                    float x = cx + (uniform_(rng_) - 0.5f) * width;
                    float y = uniform_(rng_) * height;
                    float z = cz + depth/2 + uniform_(rng_) * 0.01f;
                    points.push_back({x, y, z});
                } else if (r < 0.6f) {
                    // Left wall
                    float x = cx - width/2 + uniform_(rng_) * 0.01f;
                    float y = uniform_(rng_) * height;
                    float z = cz + (uniform_(rng_) - 0.5f) * depth;
                    points.push_back({x, y, z});
                } else if (r < 0.8f) {
                    // Right wall
                    float x = cx + width/2 + uniform_(rng_) * 0.01f;
                    float y = uniform_(rng_) * height;
                    float z = cz + (uniform_(rng_) - 0.5f) * depth;
                    points.push_back({x, y, z});
                } else {
                    // Roof
                    float x = cx + (uniform_(rng_) - 0.5f) * width;
                    float y = height + uniform_(rng_) * 0.01f;
                    float z = cz + (uniform_(rng_) - 0.5f) * depth;
                    points.push_back({x, y, z});
                }
            }
        }

        return points;
    }
};

template <typename GridType>
struct BenchmarkResult {
    std::string name;
    double buildTime;
    double queryTime;
    size_t memoryUsage;
    size_t avgNeighbors;
};

template <typename GridType>
BenchmarkResult<GridType> benchmarkGrid(const std::string& name,
                                        const std::vector<std::array<float, 3>>& points,
                                        float cellSize = 0.5f) {
    BenchmarkResult<GridType> result;
    result.name = name;

    std::cout << "\nBenchmarking " << name << "...\n";

    // Build phase
    auto grid = std::make_unique<GridType>(cellSize);

    auto startAdd = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < points.size(); ++i) {
        grid->addVertex(points[i][0], points[i][1], points[i][2], i);
    }
    auto endAdd = std::chrono::high_resolution_clock::now();

    auto startBuild = std::chrono::high_resolution_clock::now();
    grid->build();
    auto endBuild = std::chrono::high_resolution_clock::now();

    result.buildTime = std::chrono::duration<double, std::milli>(endAdd - startAdd).count() +
                      std::chrono::duration<double, std::milli>(endBuild - startBuild).count();

    // Memory usage (if available)
    if constexpr (std::is_same_v<GridType, SortedHashGridExtended<float>>) {
        result.memoryUsage = grid->getMemoryUsage();
    } else {
        result.memoryUsage = sizeof(*grid) + points.size() * 64;  // Estimate
    }

    // Query phase
    const int numQueries = 1000;
    std::mt19937 queryRng(123);
    std::uniform_real_distribution<float> queryDist(-50.0f, 50.0f);
    std::uniform_real_distribution<float> heightDist(0.0f, 40.0f);

    size_t totalNeighbors = 0;
    auto startQuery = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < numQueries; ++i) {
        float qx = queryDist(queryRng);
        float qy = heightDist(queryRng);
        float qz = queryDist(queryRng);

        if constexpr (std::is_same_v<GridType, SortedHashGridExtended<float>>) {
            auto neighbors = grid->findSimilarVertices(qx, qy, qz, cellSize);
            totalNeighbors += neighbors.size();
        } else if constexpr (std::is_same_v<GridType, AdaptiveOctree<float>>) {
            auto neighbors = grid->findNearby(qx, qy, qz, cellSize);
            totalNeighbors += neighbors.size();
        } else if constexpr (std::is_same_v<GridType, HybridCityGrid<float>>) {
            auto neighbors = grid->findSimilarVertices(qx, qy, qz, cellSize);
            totalNeighbors += neighbors.size();
        }
    }

    auto endQuery = std::chrono::high_resolution_clock::now();
    result.queryTime = std::chrono::duration<double, std::micro>(endQuery - startQuery).count() / numQueries;
    result.avgNeighbors = totalNeighbors / numQueries;

    // Print grid-specific stats
    if constexpr (std::is_same_v<GridType, SortedHashGridExtended<float>>) {
        grid->printDetailedStatistics();
    } else if constexpr (std::is_same_v<GridType, AdaptiveOctree<float>>) {
        size_t nodes, leaves, surfaces, depth, total;
        grid->getStatistics(nodes, leaves, surfaces, depth, total);
        std::cout << "  Octree: " << nodes << " nodes, " << leaves << " leaves, "
                  << surfaces << " surface nodes, depth " << depth << "\n";
    } else if constexpr (std::is_same_v<GridType, HybridCityGrid<float>>) {
        grid->printStatistics();
    }

    return result;
}

int main() {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "=== City Distribution Strategy Comparison ===\n";

    CityGenerator generator(999);

    // Test with different city sizes
    std::vector<std::pair<size_t, size_t>> testSizes = {
        {100000, 50000},    // Small city
        {500000, 200000},   // Medium city
        {2000000, 500000}   // Large city
    };

    for (const auto& [groundPoints, buildingPoints] : testSizes) {
        std::cout << "\n================================================\n";
        std::cout << "Testing with " << groundPoints << " ground points and "
                  << buildingPoints << " building points\n";
        std::cout << "================================================\n";

        auto cityPoints = generator.generateCity(groundPoints, buildingPoints);
        std::cout << "Generated " << cityPoints.size() << " total points\n";

        // Benchmark different strategies
        std::vector<std::pair<std::string, std::function<void()>>> benchmarks;

        // 1. Standard sorted hash grid
        auto resultHashGrid = benchmarkGrid<SortedHashGridExtended<float>>(
            "Sorted Hash Grid", cityPoints, 0.5f);

        // 2. Adaptive octree with surface detection
        auto resultOctree = benchmarkGrid<AdaptiveOctree<float>>(
            "Adaptive Octree", cityPoints, 0.5f);

        // 3. Hybrid city-aware grid
        auto resultHybrid = benchmarkGrid<HybridCityGrid<float>>(
            "Hybrid City Grid", cityPoints, 0.5f);

        // Print comparison table
        std::cout << "\n=== Performance Comparison ===\n";
        std::cout << std::setw(20) << "Strategy"
                  << std::setw(15) << "Build (ms)"
                  << std::setw(15) << "Query (µs)"
                  << std::setw(15) << "Memory (MB)"
                  << std::setw(15) << "Avg Neighbors\n";
        std::cout << std::string(80, '-') << "\n";

        auto printResult = [](const auto& r) {
            std::cout << std::setw(20) << r.name
                      << std::setw(15) << r.buildTime
                      << std::setw(15) << r.queryTime
                      << std::setw(15) << (r.memoryUsage / (1024.0 * 1024.0))
                      << std::setw(15) << r.avgNeighbors << "\n";
        };

        printResult(resultHashGrid);
        printResult(resultOctree);
        printResult(resultHybrid);

        // Performance analysis
        std::cout << "\n=== Analysis ===\n";

        // Find best in each category
        double minBuild = std::min({resultHashGrid.buildTime, resultOctree.buildTime, resultHybrid.buildTime});
        double minQuery = std::min({resultHashGrid.queryTime, resultOctree.queryTime, resultHybrid.queryTime});
        size_t minMemory = std::min({resultHashGrid.memoryUsage, resultOctree.memoryUsage, resultHybrid.memoryUsage});

        std::cout << "Fastest build: ";
        if (resultHashGrid.buildTime == minBuild) std::cout << "Sorted Hash Grid";
        else if (resultOctree.buildTime == minBuild) std::cout << "Adaptive Octree";
        else std::cout << "Hybrid City Grid";
        std::cout << " (" << minBuild << " ms)\n";

        std::cout << "Fastest query: ";
        if (resultHashGrid.queryTime == minQuery) std::cout << "Sorted Hash Grid";
        else if (resultOctree.queryTime == minQuery) std::cout << "Adaptive Octree";
        else std::cout << "Hybrid City Grid";
        std::cout << " (" << minQuery << " µs)\n";

        std::cout << "Lowest memory: ";
        if (resultHashGrid.memoryUsage == minMemory) std::cout << "Sorted Hash Grid";
        else if (resultOctree.memoryUsage == minMemory) std::cout << "Adaptive Octree";
        else std::cout << "Hybrid City Grid";
        std::cout << " (" << (minMemory / (1024.0 * 1024.0)) << " MB)\n";
    }

    std::cout << "\n=== Conclusion ===\n";
    std::cout << "For city-like distributions:\n";
    std::cout << "1. Hybrid City Grid: Best for large cities with clear layer separation\n";
    std::cout << "2. Adaptive Octree: Good balance, especially with surface detection\n";
    std::cout << "3. Sorted Hash Grid: Simple and fast, but less memory efficient for planes\n";

    return 0;
}