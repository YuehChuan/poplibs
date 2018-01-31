#ifndef __popconv_ConvPlan_hpp__
#define __popconv_ConvPlan_hpp__
#include <popconv/Convolution.hpp>
#include <string>
#include <poplar/Graph.hpp>
#include <iosfwd>

namespace popconv {

struct Partition {
  // For each spatial dimension the number of parts the input is split into.
  std::vector<unsigned> fieldSplit;
  // The number of parts the batch axis is split into.
  unsigned batchSplit;
  // The number of parts the output channel axis is split into.
  unsigned outChanSplit;
  // For each spatial dimension the number of parts the kernel is split into.
  std::vector<unsigned> kernelSplit;
  // The number of parts the input channel axis is split into.
  unsigned inChanSplit;
  // The number of parts the convolution group axis is split into.
  unsigned convGroupSplit;
  // Grain size to use when splitting each spatial dimension.
  std::vector<unsigned> fieldAxisGrainSize;
  // Grain size to use when splitting the input channels.
  unsigned inChanGrainSize;
  // Grain size to use when splitting the output channels.
  unsigned outChanGrainSize;

  Partition() = default;
  Partition(std::vector<unsigned> fieldSplit_,
            unsigned batchSplit_,
            unsigned outChanSplit_,
            std::vector<unsigned> kernelSplit_,
            unsigned inChanSplit_,
            unsigned convGroupSplit_,
            std::vector<unsigned> fieldAxisGrainSize_,
            unsigned inChanGrainSize_,
            unsigned outChanGrainSize_) :
    fieldSplit(std::move(fieldSplit_)),
    batchSplit(batchSplit_),
    outChanSplit(outChanSplit_),
    kernelSplit(std::move(kernelSplit_)),
    inChanSplit(inChanSplit_),
    convGroupSplit(convGroupSplit_),
    fieldAxisGrainSize(std::move(fieldAxisGrainSize_)),
    inChanGrainSize(inChanGrainSize_),
    outChanGrainSize(outChanGrainSize_) { }
};

struct Plan {
  // Description of how each level of the hierarchy is partitioned.
  std::vector<Partition> partitions;
  unsigned inChansPerGroup;
  unsigned partialChansPerGroup;
  bool floatPartials;
  // The number of additional size 1 dimensions to insert at the front.
  unsigned extraFieldDims = 0;
  bool swapOperands = false;
  // Spatial dimensions that should be expanded by taking the activations
  // multiplied by each weight in each position of the filter in this axis and
  // turning them into different input channels.
  std::vector<unsigned> expandDims;
  // Spatial dimensions that should be flattened into the output channels of the
  // kernel.
  std::vector<unsigned> outChanFlattenDims;
  // Dimensions that should be flattened. The dimensions are numbered such that
  // the batch is dimension 0 and the spatial dimensions start at 1.
  // The dimensions are flattened into the last dimension in reverse order.
  std::vector<unsigned> flattenDims;
  bool useWinograd = false;
  enum class Method {
    // Direction convolution using the MAC instruction.
    MAC,
    // Direction convolution using the AMP instruction.
    AMP,
    // Outer product of two vectors.
    OUTER_PRODUCT,
  } method;
  enum class LinearizeTileOrder {
    STANDARD,
    FC_WU,
    FC_BWD_AS_CONV
  } linearizeTileOrder = LinearizeTileOrder::STANDARD;
  unsigned winogradPatchSize;

  Plan() = default;
  Plan(std::vector<Partition> partitions_,
       unsigned inChansPerGroup_,
       unsigned partialChansPerGroup_,
       bool floatPartials_,
       Plan::Method method_,
       Plan::LinearizeTileOrder linearizeTileOrder_) :
      partitions(std::move(partitions_)),
      inChansPerGroup(inChansPerGroup_),
      partialChansPerGroup(partialChansPerGroup_),
      floatPartials(floatPartials_),
      method(method_),
      linearizeTileOrder(linearizeTileOrder_) { }
  poplar::Type getPartialType() const {
    return floatPartials ? poplar::FLOAT : poplar::HALF;
  }
};

std::vector<unsigned> getTileHierarchy(const poplar::Target &target);

Plan getPlan(const poplar::Graph &graph, const ConvParams &params,
             ConvOptions options);

/// Insert the specified number of dimensions of size 1 at the front.
void addExtraDims(ConvParams &params, unsigned extraDims);

void swapOperands(ConvParams &params);

std::uint64_t getNumberOfMACs(const ConvParams &params);

std::ostream& operator<<(std::ostream &os, const Plan::Method m);
std::ostream& operator<<(std::ostream &os, const Partition &p);
std::ostream& operator<<(std::ostream &os, const Plan &p);

std::uint64_t estimateConvCost(const poplar::Target &target,
                               const ConvParams &params,
                               const ConvOptions &options,
                               const Plan &plan,
                               PlanningCache *cache = nullptr);

}
#endif // __popconv_ConvPlan_hpp__