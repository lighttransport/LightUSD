#include "sorted_hash_grid_extended.hh"
#include <iostream>
#include <random>
#include <chrono>
#include <vector>
#include <cmath>
#include <iomanip>
#include <fstream>

using namespace lightusd::legacy::spatial;

class CityPointGenerator {
private:
    std::mt19937 rng_;
    std::uniform_real_distribution<float> uniform_;
    std::normal_distribution<float> normal_;

public:
    CityPointGenerator(uint32_t seed = 42)
        : rng_(seed), uniform_(0.0f, 1.0f), normal_(0.0f, 0.01f) {}

    // Generate points on a rectangular plane with some noise
    std::vector<std::array<float, 3>> generatePlane(
        float centerX, float centerY, float centerZ,
        float width, float height,
        size_t pointsPerSquareMeter,
        float noiseLevel = 0.001f,
        bool vertical = false) {

        std::vector<std::array<float, 3>> points;

        float area = width * height;
        size_t numPoints = static_cast<size_t>(area * pointsPerSquareMeter);

        points.reserve(numPoints);

        for (size_t i = 0; i < numPoints; ++i) {
            float u = uniform_(rng_) - 0.5f;
            float v = uniform_(rng_) - 0.5f;

            float x, y, z;

            if (vertical) {
                // Vertical plane (building wall)
                x = centerX + u * width + normal_(rng_) * noiseLevel;
                y = centerY + v * height + normal_(rng_) * noiseLevel;
                z = centerZ + normal_(rng_) * noiseLevel;
            } else {
                // Horizontal plane (ground/roof)
                x = centerX + u * width + normal_(rng_) * noiseLevel;
                y = centerY + normal_(rng_) * noiseLevel;
                z = centerZ + v * height + normal_(rng_) * noiseLevel;
            }

            points.push_back({x, y, z});
        }

        return points;
    }

    // Generate a building with walls and roof
    std::vector<std::array<float, 3>> generateBuilding(
        float baseX, float baseZ,
        float width, float depth, float height,
        size_t pointDensity = 1000) {

        std::vector<std::array<float, 3>> allPoints;

        // Roof (horizontal plane)
        auto roof = generatePlane(baseX, height, baseZ, width, depth, pointDensity * 2, 0.001f, false);
        allPoints.insert(allPoints.end(), roof.begin(), roof.end());

        // Four walls (vertical planes)
        // Front wall
        auto front = generatePlane(baseX, height/2, baseZ - depth/2, width, height, pointDensity, 0.001f, true);
        allPoints.insert(allPoints.end(), front.begin(), front.end());

        // Back wall
        auto back = generatePlane(baseX, height/2, baseZ + depth/2, width, height, pointDensity, 0.001f, true);
        allPoints.insert(allPoints.end(), back.begin(), back.end());

        // Left wall
        auto left = generatePlane(baseX - width/2, height/2, baseZ, depth, height, pointDensity, 0.001f, true);
        allPoints.insert(allPoints.end(), left.begin(), left.end());

        // Right wall
        auto right = generatePlane(baseX + width/2, height/2, baseZ, depth, height, pointDensity, 0.001f, true);
        allPoints.insert(allPoints.end(), right.begin(), right.end());

        return allPoints;
    }

    // Generate a city-like distribution
    std::vector<std::array<float, 3>> generateCity(
        float citySize = 100.0f,
        size_t numBuildings = 50,
        size_t groundPointDensity = 500,
        size_t buildingPointDensity = 1000) {

        std::vector<std::array<float, 3>> allPoints;

        // Generate ground plane with high density
        std::cout << "Generating ground plane...\n";
        auto ground = generatePlane(0, 0, 0, citySize, citySize, groundPointDensity, 0.01f, false);
        allPoints.insert(allPoints.end(), ground.begin(), ground.end());
        std::cout << "  Added " << ground.size() << " ground points\n";

        // Generate buildings in a grid-like pattern with some randomness
        std::cout << "Generating " << numBuildings << " buildings...\n";

        std::uniform_real_distribution<float> buildingPos(-citySize/2 * 0.8f, citySize/2 * 0.8f);
        std::uniform_real_distribution<float> buildingWidth(2.0f, 8.0f);
        std::uniform_real_distribution<float> buildingDepth(2.0f, 8.0f);
        std::uniform_real_distribution<float> buildingHeight(5.0f, 30.0f);

        for (size_t i = 0; i < numBuildings; ++i) {
            float x = buildingPos(rng_);
            float z = buildingPos(rng_);
            float w = buildingWidth(rng_);
            float d = buildingDepth(rng_);
            float h = buildingHeight(rng_);

            auto building = generateBuilding(x, z, w, d, h, buildingPointDensity);
            allPoints.insert(allPoints.end(), building.begin(), building.end());

            if ((i + 1) % 10 == 0) {
                std::cout << "  Generated " << (i + 1) << " buildings\n";
            }
        }

        // Add some street furniture (dense clusters)
        std::cout << "Adding street furniture clusters...\n";
        std::normal_distribution<float> clusterDist(0.0f, 0.1f);
        for (int i = 0; i < 20; ++i) {
            float cx = buildingPos(rng_);
            float cz = buildingPos(rng_);

            // Dense cluster of points (lamp post, bench, etc.)
            for (int j = 0; j < 100; ++j) {
                allPoints.push_back({
                    cx + clusterDist(rng_),
                    uniform_(rng_) * 2.0f,
                    cz + clusterDist(rng_)
                });
            }
        }

        return allPoints;
    }
};

void testCityDistribution() {
    std::cout << "\n=== Testing City-like 3D Point Distribution ===\n\n";

    CityPointGenerator generator(12345);

    // Test different cell sizes
    std::vector<float> cellSizes = {0.1f, 0.5f, 1.0f, 2.0f};

    for (float cellSize : cellSizes) {
        std::cout << "\n--- Testing with cell size: " << cellSize << " ---\n";

        // Generate city points
        auto cityPoints = generator.generateCity(100.0f, 30, 500, 800);
        std::cout << "Total city points generated: " << cityPoints.size() << "\n";

        // Create and populate grid
        SortedHashGridExtended<float> grid(cellSize, 128, 0, 4);

        std::cout << "\nAdding points to grid...\n";
        auto startAdd = std::chrono::high_resolution_clock::now();

        uint32_t id = 0;
        for (const auto& p : cityPoints) {
            grid.addVertex(p[0], p[1], p[2], id++);
        }

        auto endAdd = std::chrono::high_resolution_clock::now();
        auto addTime = std::chrono::duration_cast<std::chrono::milliseconds>(endAdd - startAdd).count();
        std::cout << "Added " << cityPoints.size() << " points in " << addTime << " ms\n";

        // Build grid
        std::cout << "Building spatial index...\n";
        auto startBuild = std::chrono::high_resolution_clock::now();
        grid.build();
        auto endBuild = std::chrono::high_resolution_clock::now();
        auto buildTime = std::chrono::duration_cast<std::chrono::milliseconds>(endBuild - startBuild).count();
        std::cout << "Built grid in " << buildTime << " ms\n";

        // Print detailed statistics
        grid.printDetailedStatistics();

        // Perform sample queries
        std::cout << "\n--- Sample Queries ---\n";
        std::vector<std::array<float, 3>> queryPoints = {
            {0.0f, 0.0f, 0.0f},      // Ground center
            {10.0f, 15.0f, 10.0f},    // Mid-air (building)
            {-20.0f, 0.0f, -20.0f},   // Ground corner
            {5.0f, 0.1f, 5.0f}        // Near ground
        };

        for (const auto& qp : queryPoints) {
            auto results = grid.findSimilarVertices(qp[0], qp[1], qp[2], cellSize * 2);
            std::cout << "Query at (" << qp[0] << ", " << qp[1] << ", " << qp[2]
                      << "): found " << results.size() << " neighbors\n";
        }

        // Performance benchmark
        std::cout << "\n--- Performance Benchmark ---\n";
        const int numQueries = 1000;
        std::uniform_real_distribution<float> queryDist(-50.0f, 50.0f);
        std::uniform_real_distribution<float> queryHeight(0.0f, 30.0f);
        std::mt19937 queryRng(42);

        auto startQuery = std::chrono::high_resolution_clock::now();
        size_t totalResults = 0;

        for (int i = 0; i < numQueries; ++i) {
            float qx = queryDist(queryRng);
            float qy = queryHeight(queryRng);
            float qz = queryDist(queryRng);
            auto results = grid.findSimilarVertices(qx, qy, qz, cellSize);
            totalResults += results.size();
        }

        auto endQuery = std::chrono::high_resolution_clock::now();
        auto queryTime = std::chrono::duration_cast<std::chrono::microseconds>(endQuery - startQuery).count();

        std::cout << "Performed " << numQueries << " queries in " << queryTime << " µs\n";
        std::cout << "Average query time: " << queryTime / numQueries << " µs\n";
        std::cout << "Average neighbors found: " << totalResults / numQueries << "\n";

        std::cout << "\n" << std::string(60, '=') << "\n";
    }
}

void generateVisualizationData() {
    std::cout << "\n=== Generating Visualization Data ===\n";

    CityPointGenerator generator(999);
    auto cityPoints = generator.generateCity(50.0f, 20, 1000, 1500);

    // Save points to file for visualization
    std::ofstream outFile("city_points.xyz");
    if (outFile.is_open()) {
        for (const auto& p : cityPoints) {
            outFile << p[0] << " " << p[1] << " " << p[2] << "\n";
        }
        outFile.close();
        std::cout << "Saved " << cityPoints.size() << " points to city_points.xyz\n";
    }

    // Create grid and analyze distribution
    SortedHashGridExtended<float> grid(0.5f, 128, 0, 4);
    for (size_t i = 0; i < cityPoints.size(); ++i) {
        grid.addVertex(cityPoints[i][0], cityPoints[i][1], cityPoints[i][2], i);
    }
    grid.build();

    // Export cell occupancy for heatmap
    auto histogram = grid.getCellItemHistogram();
    std::ofstream histFile("cell_histogram.csv");
    if (histFile.is_open()) {
        histFile << "Items,Count\n";
        for (const auto& [items, count] : histogram) {
            histFile << items << "," << count << "\n";
        }
        histFile.close();
        std::cout << "Saved histogram data to cell_histogram.csv\n";
    }
}

int main() {
    std::cout << std::fixed << std::setprecision(2);

    testCityDistribution();
    generateVisualizationData();

    std::cout << "\n=== All tests completed ===\n";

    return 0;
}