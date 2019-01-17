#include "poputil/VertexTemplates.hpp"
#include "poputil/TileMapping.hpp"
#include "NormsInternal.hpp"
#include "poputil/Util.hpp"
#include "popops/ElementWise.hpp"
#include "popnn/BatchNorm.hpp"
#include "popops/Reduce.hpp"
#include "popops/ScaledAdd.hpp"
#include "poplin/Norms.hpp"
#include "poputil/exceptions.hpp"
#include <poplar/Program.hpp>
#include <poplar/Graph.hpp>
#include <poplar/Tensor.hpp>
#include <cassert>
#include <numeric>
#include <functional>
#include <map>

using namespace poplar;
using namespace poplar::program;
using namespace poputil;
using namespace popops;

namespace popnn {
namespace gn {

static Tensor groupActs(const Tensor &acts_, unsigned numGroups) {
  const auto numChannels = acts_.dim(1);
  const auto numBatches = acts_.dim(0);
  if (numChannels % numGroups != 0) {
    throw poplibs_error("Group Norm : Number of channels must be an integral "
                        "multiple of number of groups");
  }
  auto acts =  acts_.reshapePartial(1, 2, {numChannels / numGroups, numGroups})
                    .dimRoll(2, 1)
                    .reshapePartial(0, 2, {numGroups * numBatches})
                    .dimRoll(1, 0);
  return acts;
}

static Tensor ungroupActs(const Tensor &acts_, unsigned numChannels) {
  const auto numBatches = acts_.dim(0) * acts_.dim(1) / numChannels;
  const auto numGroups = numChannels / acts_.dim(0);
  auto acts = acts_.reshapePartial(1, 2, {numBatches, numGroups})
                   .dimRoll(0, 1)
                   .reshapePartial(1, 3, {numChannels});
  return acts;
}

std::pair<Tensor, Tensor>
groupNormStatistics(Graph &graph, const Tensor acts_,
                    float eps,
                    Sequence &prog,
                    unsigned numGroups,
                    bool unbiasedVarEstimate,
                    const Type &partialsType,
                    const std::string &debugPrefix) {
  checkTensorShape(acts_);
  auto acts = groupActs(acts_, numGroups);
  return poplin::normStatistics(graph, acts, eps, prog, unbiasedVarEstimate,
                                partialsType, debugPrefix);
}

Tensor
groupNormWhiten(Graph &graph,
                const Tensor &acts,
                const Tensor &mean,
                const Tensor &iStdDev,
                Sequence &prog,
                const std::string &debugPrefix) {
  const auto rank = acts.rank();
  const auto numChannels = acts.dim(1);
  checkTensorShape(acts);
  const auto batchSize = acts.dim(0);
  assert(mean.dim(0) % batchSize == 0);
  const auto numGroups = mean.dim(0) / batchSize;
  auto groupedActs = groupActs(preProcessNormActs(acts), numGroups);
  auto whitenedActs =
      poplin::normWhiten(graph, groupedActs, mean, iStdDev, prog, debugPrefix);
  return postProcessNormActs(ungroupActs(whitenedActs, numChannels), rank);
}

std::pair<Tensor, Tensor>
groupNormalise(Graph &graph,
               const Tensor &acts,
               const Tensor &gamma,
               const Tensor &beta,
               const Tensor &mean,
               const Tensor &iStdDev,
               Sequence &prog,
               const std::string &debugPrefix) {
  const auto rank = acts.rank();
  checkTensorShape(acts);
  const auto batchSize = acts.dim(0);
  const auto numChannels = acts.dim(1);
  assert(mean.dim(0) % batchSize == 0);
  const auto numGroups = mean.dim(0) / batchSize;
  auto preProcessedActs = preProcessNormActs(acts);
  auto whitenedActs =
      groupNormWhiten(graph, preProcessedActs, mean, iStdDev, prog,
                      debugPrefix);
  auto outputActs =
      poplin::normalise(graph, whitenedActs, gamma, beta, prog, debugPrefix);
  return std::make_pair(postProcessNormActs(outputActs, rank),
                        postProcessNormActs(whitenedActs, rank));
}

std::pair<Tensor, Tensor>
groupNormParamGradients(Graph &graph,
                        const Tensor &actsWhitened,
                        const Tensor &gradsIn,
                        Sequence &prog,
                        const Type &partialsType,
                        const std::string &debugPrefix) {
  checkTensorShape(gradsIn);
  checkTensorShape(actsWhitened);
  return poplin::normParamGradients(graph, actsWhitened, gradsIn, prog,
                                    partialsType, debugPrefix);
}

Tensor groupNormGradients(Graph &graph,
                          const Tensor &actsWhitened_,
                          const Tensor &gradsIn_,
                          const Tensor &iStdDev,
                          const Tensor &gamma,
                          Sequence &prog,
                          const Type &partialsType,
                          const std::string &debugPrefix) {
  const auto rank = actsWhitened_.rank();
  const auto numChans = actsWhitened_.dim(1);
  checkTensorShape(actsWhitened_);
  checkTensorShape(gradsIn_);
  const auto batchSize = actsWhitened_.dim(0);
  assert(iStdDev.dim(0) % batchSize == 0);
  const auto numGroups = iStdDev.dim(0) / batchSize;
  auto actsWhitened = preProcessNormActs(actsWhitened_);
  auto gradsIn = preProcessNormActs(gradsIn_);
  auto gradsNorm =
      poplin::normGradients(graph, gradsIn, gamma, prog, debugPrefix);
  auto groupedActsWhitened = groupActs(actsWhitened, numGroups);
  auto groupedGradsNorm = groupActs(gradsNorm, numGroups);
  auto gradsOut =
      poplin::normStatisticsGradients(graph, groupedActsWhitened,
                                      groupedGradsNorm, iStdDev, prog,
                                      partialsType, debugPrefix);
  return postProcessNormActs(ungroupActs(gradsOut, numChans), rank);
}

void groupNormParamUpdate(Graph &graph,
                          const Tensor &gammaDelta,
                          const Tensor &betaDelta,
                          float learningRate,
                          Tensor &gamma,
                          Tensor &beta,
                          Sequence &prog,
                          const std::string &debugPrefix) {
  const std::string fnPrefix = debugPrefix + "/GN/paramUpdate";
  // Do update of beta and gamma together
  scaledAddTo(graph, concat(beta, gamma), concat(betaDelta, gammaDelta),
              -learningRate, prog, fnPrefix);
}
} // namespace gn
} // namespace popnn