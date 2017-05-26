#include "popstd/Zero.hpp"

#include <poplar/Graph.hpp>
#include "popstd/Util.hpp"
#include "popstd/VertexTemplates.hpp"

using namespace poplar;
using namespace poplar::program;

namespace popstd {

void
zero(Graph &graph,
     Tensor t,
     const std::vector<Interval<std::size_t>> &tileRegions,
     unsigned tile,
     ComputeSet zeroCS) {
  t = t.flatten();
  const auto dType = t.elementType();
  const auto &deviceInfo = graph.getDevice().getDeviceInfo();
  unsigned vectorWidth;
  if (dType == "float")
    vectorWidth = deviceInfo.getFloatVectorWidth();
  else
    vectorWidth = deviceInfo.getHalfVectorWidth();

  buildTransform2D(
      graph, tileRegions, vectorWidth,
      [&](const std::vector<Interval<std::size_t>> &regions) {
    const auto numRegions = regions.size();
    assert(numRegions != 0);
    VertexRef v;
    if (numRegions == 1) {
      v = graph.addVertex(zeroCS, templateVertex("popstd::Zero", dType));
      const auto &region = regions.front();
      const auto regionBegin = region.begin();
      const auto regionEnd = region.end();
      auto out = t.slice(regionBegin, regionEnd);
      graph.connect(v["out"], out);
    } else {
      v = graph.addVertex(zeroCS, templateVertex("popstd::Zero2D", dType));
      graph.setFieldSize(v["out"], regions.size());
      for (unsigned i = 0; i != numRegions; ++i) {
        const auto &region = regions[i];
        const auto regionBegin = region.begin();
        const auto regionEnd = region.end();
        auto out = t.slice(regionBegin, regionEnd);
        graph.connect(v["out"][i], out);
      }
    }
    graph.setInitialValue(v["dataPathWidth"], deviceInfo.dataPathWidth);
    graph.setTileMapping(v, tile);
  });
}

void
zero(Graph &graph,
     const Tensor &t,
     const std::vector<
       std::vector<Interval<std::size_t>>
     > &mapping,
     ComputeSet zeroCS) {
  const auto &deviceInfo = graph.getDevice().getDeviceInfo();
  const auto numTiles = deviceInfo.getNumTiles();
  for (unsigned tile = 0; tile != numTiles; ++tile) {
    zero(graph, t, mapping[tile], tile, zeroCS);
  }
}

void zero(poplar::Graph &graph, const poplar::Tensor &t,
          poplar::program::Sequence &prog,
          const std::string &debugPrefix) {
  auto cs = graph.addComputeSet(debugPrefix + "/Zero");
  zero(graph, t, graph.getTileMapping(t), cs);
  prog.add(Execute(cs));
}

} // end namespace popstd
