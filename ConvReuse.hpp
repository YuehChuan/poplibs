#ifndef __ConvReuse_hpp__
#define __ConvReuse_hpp__
#include <poplar/Graph.hpp>
#include <poplar/Program.hpp>
#include "neural_net_common.h"

struct ReusableLayer {
  poplar::program::Program prog;
  std::vector<poplar::Tensor> inputs;
  std::vector<poplar::Tensor> outputs;
  ReusableLayer(poplar::program::Program prog,
                std::vector<poplar::Tensor> inputs,
                std::vector<poplar::Tensor> outputs) :
    prog(prog),
    inputs(std::move(inputs)),
    outputs(std::move(outputs)) {}
};

class ConvImplSpec {
public:
  std::vector<std::vector<size_t>> tensorDims;
  unsigned kernelSize, stride, padding;
  NonLinearityType nonLinearityType;
  ResidualMethod resMethod;
  ConvImplSpec(std::vector<std::vector<size_t>> tensorDims,
               unsigned kernelSize, unsigned stride, unsigned padding,
               NonLinearityType nonLinearityType,
               ResidualMethod resMethod) :
    tensorDims(std::move(tensorDims)),
    kernelSize(kernelSize), stride(stride), padding(padding),
    nonLinearityType(nonLinearityType), resMethod(resMethod) {}

  bool operator<(const ConvImplSpec &other) const;
};


#endif // __ConvReuse_hpp__
