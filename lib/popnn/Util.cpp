#include "Util.hpp"

#include <algorithm>
#include <numeric>

void mergeAdjacentRegions(
    std::vector<std::pair<unsigned, unsigned>> &regions) {
  std::vector<std::pair<unsigned, unsigned>> newRegions;
  std::sort(regions.begin(), regions.end());
  for (const auto &region : regions) {
    if (region.first == region.second)
      continue;
    assert(newRegions.empty() || newRegions.back().second <= region.first);
    if (!newRegions.empty() && newRegions.back().second == region.first) {
      newRegions.back().second = region.second;
    } else {
      newRegions.push_back(region);
    }
  }
  std::swap(regions, newRegions);
}

void mergeAdjacentRegions(
    std::vector<std::vector<std::pair<unsigned, unsigned>>> &mapping) {
  const auto numTiles = mapping.size();
  for (unsigned tile = 0; tile != numTiles; ++tile) {
    mergeAdjacentRegions(mapping[tile]);
  }
}

void splitRegions(
    const std::vector<std::pair<unsigned, unsigned>> &regions,
    std::vector<std::vector<std::pair<unsigned, unsigned>>> &vertexRegions,
    unsigned grainSize, unsigned maxPartitions,
    unsigned minElementsPerPartition) {
  vertexRegions.clear();
  const auto numElements =
      std::accumulate(regions.begin(), regions.end(), 0U,
                      [](unsigned numElements,
                         const std::pair<unsigned, unsigned> &region) {
    return numElements + region.second - region.first;
  });
  if (numElements == 0)
    return;
  const auto numGroups = (numElements + grainSize - 1) / grainSize;
  auto maxGroupsPerPartition =
    (numGroups + maxPartitions - 1) / maxPartitions;
  if (minElementsPerPartition) {
    const auto minGroupsPerPartition =
      (minElementsPerPartition + grainSize - 1) / grainSize;
    const auto maxVerticesToCreate =
      std::max(1U, numGroups / minGroupsPerPartition);
    maxGroupsPerPartition =
        std::max(maxGroupsPerPartition,
                 (numGroups + maxVerticesToCreate - 1) / maxVerticesToCreate);
  }
  const auto verticesToCreate =
    (numGroups + maxGroupsPerPartition - 1) / maxGroupsPerPartition;
  auto it = regions.begin();
  unsigned count = 0;
  vertexRegions.resize(verticesToCreate);
  for (unsigned vertex = 0; vertex != verticesToCreate; ++vertex) {
    const auto groupBegin = (vertex * numGroups) / verticesToCreate;
    const auto groupEnd = ((vertex + 1) * numGroups) / verticesToCreate;
    const auto elemBegin = groupBegin * grainSize;
    const auto elemEnd = std::min(numElements, groupEnd * grainSize);
    auto vertexElements = elemEnd - elemBegin;
    while (vertexElements) {
      if (count == it->second - it->first) {
        count = 0;
        ++it;
      }
      const auto vertexRegionSize = std::min(vertexElements,
                                             it->second - it->first - count);
      const auto vertexRegionBegin = it->first + count;
      const auto vertexRegionEnd = vertexRegionBegin + vertexRegionSize;
      vertexRegions[vertex].emplace_back(vertexRegionBegin, vertexRegionEnd);
      count += vertexRegionSize;
      vertexElements -= vertexRegionSize;
    }
  }
}

void splitRegionsBetweenWorkers(
    const poplar::DeviceInfo &deviceInfo,
    const std::vector<std::pair<unsigned, unsigned>> &regions,
    std::vector<std::vector<std::pair<unsigned, unsigned>>> &vertexRegions,
    unsigned grainSize,
    unsigned minElementsPerVertex) {
  const auto workersPerTile = deviceInfo.numWorkerContexts;
  return splitRegions(regions, vertexRegions, grainSize, workersPerTile,
                      minElementsPerVertex);
}
