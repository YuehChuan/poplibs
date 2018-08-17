#include <poplar/Vertex.hpp>
#include <poplar/HalfFloat.hpp>
#include <cassert>
#include <cmath>
#include <type_traits>

#include "poplibs_support/ExternalCodelet.hpp"

using namespace poplar;

static constexpr auto ONE_PTR = poplar::VectorLayout::ONE_PTR;
static constexpr auto TWO_PTR = poplar::VectorLayout::TWO_PTR;
static constexpr auto DELTAN = poplar::VectorListLayout::DELTAN;

#if defined(__IPU__) && !defined(POPLIBS_DISABLE_ASM_CODELETS)
#define EXTERNAL_CODELET true
#else
#define EXTERNAL_CODELET false
#endif

namespace poplin {

/**
 * Compute nx1 convolutions and accumulate them with partial sums in memory.
 * useLimitedVer is "true" if there are constraints imposed on
 * - size of strides are bounded to strides supported by ISA
 * - worklists-offsets are bounded to fit 16-bits
 * - worklists-number of elements <= maximum count supported by rpt instruction
 **/
template <class FPType, class AccumType, bool useLimitedVer>
class
[[poplar::constraint("elem(**in) != elem(**out)")]]
ConvPartialnx1: public SupervisorVertex {
public:
  using WorkListType =
      typename std::conditional<useLimitedVer, unsigned short, unsigned>::type;
  Vector<Input<Vector<FPType, ONE_PTR, 8>>, ONE_PTR> in;
  Vector<Input<Vector<FPType, ONE_PTR, 16, true>>, ONE_PTR> weights;
  Vector<Output<Vector<AccumType, ONE_PTR, 8, true>>, ONE_PTR> out;
  Input<VectorList<WorkListType, VectorListLayout::DELTAN>> worklists;
  Input<Vector<WorkListType>> zeroWorklist;
  unsigned numOutGroupsM1;
  unsigned numInGroupsM1;
  unsigned kernelOuterSizeM1;
  unsigned kernelInnerElementsM1;
  // This value is
  // (inStrideX - 1 - (ampKernelHeight - 1) * inRowStride)
  //      * inChansPerGroup / convInputLoadElems + 1)
  // Where inStrideX is the actual stride
  int transformedInStride;
  // This output stride also encodes the flip parameter and is given as
  // -6 + outChansPerGroup * (actual output stride) if flipOut = false
  // -6 - outChansPerGroup * (actual output stride) if flipOut = true
  int transformedOutStride;
  unsigned numConvGroupsM1;
  // The number of kernel elements we accumulate across within the AMP unit
  unsigned ampKernelHeightM1;
  // The actual coding of this is
  //  (inRowSride - 1) * inChansPerGroup / convInputLoadElems + 1
  int transformedInRowStride;
  unsigned outChansPerGroup;
  unsigned inChansPerGroup;

  SimOnlyField<unsigned> convInputLoadElems;

  static const bool isExternalCodelet = (EXTERNAL_CODELET) &&
                                        std::is_same<FPType, half>() &&
                                        std::is_same<AccumType, float>() &&
                                        useLimitedVer == true;

  bool compute() {
    const auto numOutGroups = numOutGroupsM1 + 1;
    const auto numInGroups = numInGroupsM1 + 1;
    const auto numConvGroups = numConvGroupsM1 + 1;
    const auto ampKernelHeight = ampKernelHeightM1 + 1;
    const auto kernelOuterSize = kernelOuterSizeM1 + 1;
    const auto kernelInnerElements = kernelInnerElementsM1 + 1;

    int inRowStride =
        (transformedInRowStride - 1) * convInputLoadElems/ inChansPerGroup + 1;

    const auto inStride =
        (transformedInStride - 1) * convInputLoadElems / inChansPerGroup + 1 +
        (ampKernelHeight - 1) * inRowStride;


    const auto usedContexts = worklists.size() / (kernelOuterSize *
                                                  kernelInnerElements);
    assert(zeroWorklist.size() % 2 == 0);
    const auto flipOut = transformedOutStride < -6;
    const auto outStride =
        flipOut ? (-transformedOutStride - 6) / outChansPerGroup :
                  (transformedOutStride + 6) / outChansPerGroup;

    for (unsigned cg = 0; cg != numConvGroups; ++cg) {
      for (unsigned og = 0; og != numOutGroups; ++og) {
        for (unsigned context = 0; context != zeroWorklist.size() / 2;
              ++context) {
          for (unsigned i = 0; i != zeroWorklist[2 * context + 1]; ++i) {
            out[cg * numOutGroups + og][zeroWorklist[2 * context] + i] = 0;
          }
        }
      }
    }
    for (unsigned cg = 0; cg < numConvGroups; ++cg) {
      for (unsigned og = 0; og < numOutGroups; ++og) {
        for (unsigned ig = 0; ig < numInGroups; ++ig) {
          const auto &w = weights[cg * numOutGroups * numInGroups +
                                  ig * numOutGroups +
                                  (numOutGroups - 1 - og)];
          for (unsigned ky = 0; ky < kernelOuterSize; ++ky) {
            for (unsigned kx = 0; kx < kernelInnerElements; ++kx) {
              for (unsigned context = 0; context < usedContexts; ++context) {
                const auto k = (ky * kernelInnerElements + kx);
                const auto &wl = worklists[k * usedContexts + context];
                unsigned wi = 0;
                while (wi < wl.size()) {
                  auto outOffset  = wl[wi];
                  auto numFieldElems   = wl[wi + 1];
                  auto inOffset   = wl[wi + 2];

                  wi += 3;
                  for (unsigned i = 0; i < numFieldElems; ++i) {
                    for (unsigned outChan = 0;
                         outChan < outChansPerGroup;
                         ++outChan) {
                      const auto outIndex =
                          (outOffset + (flipOut ? -i : i) * outStride)
                          * outChansPerGroup + outChan;
                      AccumType sum = out[cg * numOutGroups + og][outIndex];
                      for (unsigned ak = 0; ak < ampKernelHeight; ++ak) {
                        for (unsigned inChan = 0;
                             inChan < inChansPerGroup;
                             ++inChan) {
                          const auto inIndex =
                              (inOffset + i * inStride) * inChansPerGroup +
                              ak * inRowStride * inChansPerGroup +
                              inChan;
                          const auto weightIndex =
                              ky * ampKernelHeight * kernelInnerElements *
                                   outChansPerGroup * inChansPerGroup +
                              kx * outChansPerGroup * inChansPerGroup +
                              ak * kernelInnerElements * outChansPerGroup *
                                   inChansPerGroup +
                              outChan * inChansPerGroup +
                              inChan;
                          sum +=
                              in[cg * numInGroups + ig][inIndex] *
                              w[weightIndex];
                        }
                      }
                      out[cg * numOutGroups + og][outIndex] = sum;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
    return true;
  }
};

template class ConvPartialnx1<float, float, true>;
template class ConvPartialnx1<half, half, true>;
template class ConvPartialnx1<half, float, true>;
template class ConvPartialnx1<float, float, false>;
template class ConvPartialnx1<half, half, false>;
template class ConvPartialnx1<half, float, false>;

/**
 * Compute a sum of 1x1 convolutions over a subset of the input channels for
 * multiple output channels.
 * useLimitedVer is "true" if there are constraints imposed on
 * - size of strides are bounded to strides supported by ISA
 * - worklists-offsets are bounded to fit 16-bits
 * - worklists-number of elements <= maximum count supported by rpt instruction
 **/
template <class FPType, class AccumType, bool useLimitedVer>
class
[[poplar::constraint("elem(**in) != elem(**out)")]]
ConvPartial1x1Out: public SupervisorVertex {
public:
  using WorkListType =
      typename std::conditional<useLimitedVer, unsigned short, unsigned>::type;
  Vector<Input<Vector<FPType, ONE_PTR, 8>>, ONE_PTR> in;
  Vector<Input<Vector<FPType, ONE_PTR, 16, true>>, ONE_PTR> weights;
  Vector<Output<Vector<AccumType, ONE_PTR, 8, true>>, ONE_PTR> out;
  Input<VectorList<WorkListType, VectorListLayout::DELTAN>> worklists;
  unsigned numConvGroupsM1;
  // Actual value is 1 more than this
  unsigned numOutGroupsM1;
  // Actual value is 1 more than this
  unsigned numInGroupsM1;
  // This value is
  // (inStrideX - 1) * inChansPerGroup / convInputLoadElems + 1)
  // Where inStrideX is the actual stride
  int transformedInStride;
  unsigned outChansPerGroup;
  // This stride encodes the flip out parameter
  int transformedOutStride;
  SimOnlyField<unsigned> inChansPerGroup;
  SimOnlyField<unsigned> convInputLoadElems;

  static const bool isExternalCodelet = (EXTERNAL_CODELET) &&
                                        std::is_same<FPType, half>() &&
                                        std::is_same<AccumType, float>() &&
                                        useLimitedVer == true;

  bool compute() {
    const auto usedContexts = worklists.size();
    // modify to set actual values used by vertex
    const auto numConvGroups = numConvGroupsM1 + 1;
    const auto numOutGroups = numOutGroupsM1 + 1;
    const auto numInGroups = numInGroupsM1 + 1;
    const auto inStride =
        (transformedInStride - 1) * convInputLoadElems / inChansPerGroup + 1;
    bool flipOut = transformedOutStride < -6;

    for (unsigned cg = 0; cg < numConvGroups; ++cg) {
      for (unsigned og = 0; og < numOutGroups; ++og) {
        for (unsigned ig = 0; ig < numInGroups ; ++ig) {
          const auto &w = weights[cg * numOutGroups * numInGroups +
                                  ig * numOutGroups +
                                  (numOutGroups - 1 - og)];
          for (unsigned context = 0; context < usedContexts; ++context) {
            const auto &wl = worklists[context];
            unsigned wi = 0;
            while (wi < wl.size()) {
              auto outOffset  = wl[wi];
              auto numFieldElems = wl[wi + 1];
              auto inOffset   = wl[wi + 2];

              wi += 3;
              for (unsigned i = 0; i < numFieldElems; ++i) {
                for (unsigned outChan = 0;
                     outChan < outChansPerGroup;
                     ++outChan) {
                  const auto outIndex =
                      (outOffset + (flipOut ? -i : i)) * outChansPerGroup
                      + outChan;
                  if (ig == 0)
                    out[cg * numOutGroups + og][outIndex] = 0;
                  float sum = 0;
                  for (unsigned inChan = 0;
                       inChan < inChansPerGroup;
                       ++inChan) {
                    const auto inIndex =
                        (inOffset + i * inStride) * inChansPerGroup + inChan;
                    const auto weightIndex =
                        outChan * inChansPerGroup + inChan;
                    sum += in[cg * numInGroups + ig][inIndex] *
                           w[weightIndex];
                  }
                  out[cg * numOutGroups + og][outIndex] += sum;
                }
              }
            }
          }
        }
      }
    }
    return true;
  }
};

template class ConvPartial1x1Out<half, half, true>;
template class ConvPartial1x1Out<half, float, true>;
template class ConvPartial1x1Out<float, half, true>;
template class ConvPartial1x1Out<float, float, true>;
template class ConvPartial1x1Out<half, half, false>;
template class ConvPartial1x1Out<half, float, false>;
template class ConvPartial1x1Out<float, half, false>;
template class ConvPartial1x1Out<float, float, false>;

/* Perform a series of 1x1 convolutions using the MAC instruction were the
 * axis of accumulation is across the vector.
 * useLimitedVer is "true" if there are constraints imposed on
 * - The number of input channels is a multiple of 2
 * - worklists items are bounded to fit 16-bits
 * - number of input channels per group divided by 2 or 4 depending on whether
 *   those channels are multiple of 2 and 4 respectively
 *    <= maximum count supported by rpt instruction
 */
template <class FPType, class AccumType, bool useLimitedVer>
class
[[poplar::constraint("elem(**in) != elem(**weights)")]]
ConvPartialHorizontalMac : public SupervisorVertex {
public:
  using WorkListType =
      typename std::conditional<useLimitedVer, unsigned short, unsigned>::type;
  Vector<Input<Vector<FPType, ONE_PTR, 8>>, ONE_PTR> in;
  Vector<Input<Vector<FPType, ONE_PTR, 8>>, ONE_PTR> weights;
  Vector<InOut<Vector<AccumType, ONE_PTR, 8>>, ONE_PTR> out;
  Input<VectorList<WorkListType, VectorListLayout::DELTAN>> worklists;
  Input<Vector<WorkListType>> zeroWorklist;
  unsigned numOutGroupsM1;
  unsigned numInGroupsM1;
  unsigned kernelSizeM1;
  // transformedInStride =  ("actual input stride" - 1) * inChansPerGroup
  unsigned transformedInStride;
  // transformedOutStride =
  //   = (-1 * "actual output stride" - 1 * outChansPerGroup (if flip output)
  //   = +1 * "actual output stride" * outChansPerGroup
  int transformedOutStride;
  unsigned numConvGroupsM1;
  unsigned outChansPerGroup;
  unsigned inChansPerGroup;

  static const bool isExternalCodelet = (EXTERNAL_CODELET) &&
                                        std::is_same<FPType, half>() &&
                                        std::is_same<AccumType, float>() &&
                                        useLimitedVer == true;
  bool compute() {
    const auto kernelSize = kernelSizeM1 + 1;
    const auto usedContexts = worklists.size() / kernelSize;
    const auto numOutGroups = numOutGroupsM1 + 1;
    const auto numInGroups = numInGroupsM1 + 1;
    const auto numConvGroups = numConvGroupsM1 + 1;
    const auto outStride = transformedOutStride / outChansPerGroup + 1;
    const auto inStride = transformedInStride / inChansPerGroup;

    for (unsigned cg = 0; cg != numConvGroups; ++cg) {
      for (unsigned og = 0; og != numOutGroups; ++og) {
        for (unsigned context = 0; context != zeroWorklist.size()  / 2;
              ++context) {
          for (unsigned i = 0; i != zeroWorklist[2 * context + 1]; ++i) {
            out[cg * numOutGroups + og][zeroWorklist[2 * context] + i] = 0;
          }
        }
      }
    }
    for (unsigned cg = 0; cg != numConvGroups; ++cg) {
      for (unsigned og = 0; og != numOutGroups; ++og) {
        for (unsigned ig = 0; ig != numInGroups; ++ig) {
          const auto &w = weights[cg * numOutGroups * numInGroups +
                                  ig * numOutGroups +
                                  (numOutGroups - 1 - og)];

          for (unsigned k = 0; k != kernelSize; ++k) {
            for (unsigned context = 0; context < usedContexts; ++context) {
              const auto &wl =
                  worklists[k * usedContexts + context];
              unsigned wi = 0;
              while (wi < wl.size()) {
                auto outOffset  = wl[wi];
                auto numConv   = wl[wi + 1];
                auto inOffset   = wl[wi + 2];
                wi += 3;
                for (unsigned i = 0; i != numConv; ++i) {
                  for (unsigned oc = 0; oc != outChansPerGroup; ++oc) {
                    const auto outIndex =
                      (outOffset +  i * outStride) * outChansPerGroup + oc;
                    AccumType sum = out[cg * numOutGroups + og][outIndex];
                    for (unsigned ic = 0; ic != inChansPerGroup; ++ic) {
                      const auto inIndex =
                        (inOffset + i * inStride) * inChansPerGroup + ic;
                      const auto weightIndex =
                            k * outChansPerGroup * inChansPerGroup +
                            oc * inChansPerGroup + ic;
                      sum += in[cg * numInGroups + ig][inIndex]
                               * w[weightIndex];
                    }
                    out[cg * numOutGroups + og][outIndex] = sum;
                  }
                }
              }
            }
          }
        }
      }
    }
    return true;
  }
};
template class ConvPartialHorizontalMac<float, float, true>;
template class ConvPartialHorizontalMac<float, float, false>;
template class ConvPartialHorizontalMac<half, float, true>;
template class ConvPartialHorizontalMac<half, float, false>;
template class ConvPartialHorizontalMac<half, half, true>;
template class ConvPartialHorizontalMac<half, half, false>;


template <class FPType, unsigned patchSizeX, unsigned patchSizeY,
          unsigned kernelX, unsigned kernelY>
class WgdDataTransform : public Vertex {

  /* Set this to true if transform is stored in transposed order */
  static constexpr bool transpose = true;

  FPType rdIn(unsigned base, unsigned row, unsigned col, unsigned el) const {
    return dIn[base + col * patchSizeX + row][el];
  }

  FPType& wrTf(unsigned base, unsigned row, unsigned col, unsigned el) {
    if (!transpose) {
      return dTf[base + col * patchSizeX + row][el];
    }
    else
    {
      return dTf[base + row * patchSizeY + col][el];
    }
  }

public:
  /* The input is an array of one dimensional vectors each of size equal
   * to a number of independent input channels. This implementation differs from
   * assembler implementation in that it assumes a vector for every X,Y point
   * and doesn't require the vector length to be a multiple of 4.
   * The assembler implementation assumes a pointer to every Y with known
   * dim(X)*dim(Z_in_partial).
   */
  Vector<Input<Vector<FPType>>> dIn;

  /* Exactly same implementation details as input vector dIn
   */
  Vector<Output<Vector<FPType>>> dTf;

  bool compute() {

    assert(patchSizeX == 4);
    assert(patchSizeY == 4);
    assert(kernelX == 3);
    assert(kernelY == 3);
    const unsigned numInpCols = patchSizeY;
    const unsigned numInpRows = patchSizeX;
    const unsigned numOutCols = patchSizeY;
    const unsigned numOutRows = patchSizeX;

    const unsigned nPatches = dIn.size() / (numInpRows * numInpCols);

    for (auto patch = 0; patch < nPatches; ++patch) {
      /* patch Base */
      unsigned pBase = patch * numInpCols * numInpRows;

      const unsigned depth = dIn[0].size();

      for (int elem = 0; elem < depth; ++elem) {
        FPType dTemp[numOutCols][numOutCols];

        /* First stage: input tile must be square */
        for (unsigned row = 0; row < numInpRows; ++row) {
          dTemp[row][0] = rdIn(pBase, row, 0, elem) - rdIn(pBase, row, 2, elem);
          dTemp[row][1] = rdIn(pBase, row, 1, elem) + rdIn(pBase, row, 2, elem);

          dTemp[row][2] = rdIn(pBase, row, 2, elem) - rdIn(pBase, row, 1, elem);
          dTemp[row][3] = rdIn(pBase, row, 1, elem) - rdIn(pBase, row, 3, elem);
        }

        /* Final stage: rows==columns for outputs */
        for (unsigned col = 0; col < numOutCols; ++col) {
          wrTf(pBase, 0, col, elem) = dTemp[0][col] - dTemp[2][col];
          wrTf(pBase, 1, col, elem) = dTemp[1][col] + dTemp[2][col];
          wrTf(pBase, 2, col, elem) = dTemp[2][col] - dTemp[1][col];
          wrTf(pBase, 3, col, elem) = dTemp[1][col] - dTemp[3][col];
        }
      }
    }
    return true;
  }
};

template class WgdDataTransform<float, 4, 4, 3, 3>;
template class WgdDataTransform<half, 4, 4, 3, 3>;

template <class FPType, unsigned patchSizeX, unsigned patchSizeY,
          unsigned kernelX, unsigned kernelY>
class WgdKernelTransform : public Vertex {

  /* Set this to true if transform is stored in transposed order */
  static constexpr bool transpose = true;

  /* storage depends on whether transpose or normal form of transform is
   * stored
   */
  FPType& wrTf(const unsigned base, const unsigned row, const unsigned col,
               const unsigned elem) {
    return transpose ? wTf[base + row * patchSizeY + col][elem] :
                           wTf[base + col * patchSizeX + row][elem];
  }

  FPType rdIn(unsigned base, unsigned row, unsigned col, unsigned elem) const {
    return wIn[base + col * kernelX + row][elem];
  }

public:
  /* Each input is a 1D vector of independent channels which may be a mix of
   * input and output channels. Therefore kernelCols*kernelRow vectors are
   * required to have all elements of a kernel. The 1D vectors are stored in row
   * order
   */
  Vector<Input<Vector<FPType>>, ONE_PTR> wIn;

  /* Same as wIn except that numOutCols*numOutRows vectors each of dimension
   * 1xdepth are stored
   */
  Vector<Output<Vector<FPType, ONE_PTR>>> wTf;


  bool compute() {
    const unsigned numOutCols = patchSizeY;
    const unsigned numOutRows = patchSizeX;
    const unsigned nGroups = wTf.size() / (numOutCols * numOutRows);
    assert(numOutCols == 4);
    assert(numOutRows == 4);
    assert(kernelX == 3);
    assert(kernelY == 3);

    for (int group = 0; group < nGroups; ++group) {
      unsigned gBaseIn  = kernelY * kernelX * group;
      unsigned gBaseOut = numOutRows * numOutCols * group;

      const unsigned depth = wIn[0].size();

      for (unsigned elem = 0; elem < depth; ++elem) {
        FPType g[kernelX][kernelY];

        for (unsigned row = 0; row < kernelX; ++row) {
          for (unsigned col = 0; col < kernelY; ++col) {
            g[row][col] = rdIn(gBaseIn, row, col, elem);
          }
        }

        FPType A = (g[0][0] + g[0][1] + g[0][2]) * 0.5;
        FPType B = (g[0][0] - g[0][1] + g[0][2]) * 0.5;

        FPType C = (g[0][0] + g[1][0] + g[2][0]) * 0.5;
        FPType F = (g[0][0] - g[1][0] + g[2][0]) * 0.5;

        FPType D = (g[2][0] + g[2][1] + g[2][2]) * 0.5;
        FPType E = (g[2][0] - g[2][1] + g[2][2]) * 0.5;

        FPType G = (g[1][0] + g[1][1] + g[1][2]) * 0.5;
        FPType H = (g[1][0] - g[1][1] + g[1][2]) * 0.5;

        FPType I = (g[0][2] + g[1][2] + g[2][2]) * 0.5;
        FPType J = (g[0][2] - g[1][2] + g[2][2]) * 0.5;

        wrTf(gBaseOut, 0, 0, elem) = g[0][0];
        wrTf(gBaseOut, 0, 1, elem) = A;
        wrTf(gBaseOut, 0, 2, elem) = B;
        wrTf(gBaseOut, 0, 3, elem) = g[0][2];

        wrTf(gBaseOut, 1, 0, elem) = C;
        wrTf(gBaseOut, 1, 1, elem) = (A + G + D) * 0.5;
        wrTf(gBaseOut, 1, 2, elem) = (B + H + E) * 0.5;
        wrTf(gBaseOut, 1, 3, elem) = I;

        wrTf(gBaseOut, 2, 0, elem) = F;
        wrTf(gBaseOut, 2, 1, elem) = (A - G + D) * 0.5;
        wrTf(gBaseOut, 2, 2, elem) = (B - H + E) * 0.5;
        wrTf(gBaseOut, 2, 3, elem) = J;

        wrTf(gBaseOut, 3, 0, elem) = g[2][0];
        wrTf(gBaseOut, 3, 1, elem) = D;
        wrTf(gBaseOut, 3, 2, elem) = E;
        wrTf(gBaseOut, 3, 3, elem) = g[2][2];
      }
    }
    return true;
  }
};

template class WgdKernelTransform<float, 4, 4, 3, 3>;
template class WgdKernelTransform<half, 4, 4, 3, 3>;


template <class FPType>
class WgdPartials : public SupervisorVertex {
public:
  /* data transform vectors. Each vector is a 1D vector of length inpChanDepth.
   * Every input vector shares the same weight vector.
   * A total of nGroups 1D vectors may be provided.
   */
  Vector<Input<Vector<FPType>>, ONE_PTR> dTf;

  /* kernel transform vector. Each vector is of length inpChanDepth*outChanDepth
   * The same input data is used to generate outChanDepth outputs for each input
   * vector
   */
  Vector<Input<Vector<FPType>>> wTf;

  /* Output for each of the nGroups 1D vectors. Each input vector results in a
   * 1xoutChanDepth vector.
   */
  Vector<InOut<Vector<FPType>>> partials;

  bool compute() {

    const unsigned outChanDepth = partials[0].size();
    const unsigned inpChanDepth = dTf[0].size();
    const unsigned numInpGroups = wTf.size();
    const unsigned comPencils = partials.size();



    /* all feature elements share the same weights */
    assert(wTf[0].size() == inpChanDepth * outChanDepth);

    for (unsigned ig = 0; ig < numInpGroups; ++ig) {
      for (unsigned gr = 0; gr < comPencils; ++gr) {
        for (unsigned oc = 0; oc < outChanDepth; ++oc) {
          FPType acc{0};

          for (unsigned ic = 0; ic < inpChanDepth; ++ic) {
            const auto idx = ig * comPencils + gr;
            acc += dTf[idx][ic] * wTf[ig][oc * inpChanDepth + ic];
          }

          if (ig == 0) {
            partials[gr][oc] = acc;
          } else {
            partials[gr][oc] += acc;
          }
        }
      }
    }
    return true;
  }
};

template class WgdPartials<float>;
template class WgdPartials<half>;


template <class FPType, unsigned patchSizeX, unsigned patchSizeY>
class WgdReduce: public Vertex {

public:
  /* The vector of partial contains 1D vectors of length inpLength. The
   * partialSumLen 1D vectors are summed to produce a single output vector of
   * the same length as the input vector. Several such operations may be
   * performed to produce nGroups vectors of 1D vectors.
   */
  Vector<Input<Vector<FPType, ONE_PTR>>> inPartial;

  /*
   * The output may be a sum of all partials to produce partial sum or a full
   * sum
   */
  Vector<Output<Vector<FPType>>> outPartial;

  bool compute() {
    const unsigned numOutRows = patchSizeX;
    const unsigned numOutCols = patchSizeY;
    const unsigned numElems = outPartial.size();
    const unsigned numOutChans = outPartial[0].size();
    const unsigned numInpChans = inPartial.size() / numElems;



    for (unsigned elem = 0; elem < numElems ; ++elem) {

      auto inIdx = elem * numInpChans;

      for (unsigned oc = 0; oc < numOutChans; ++oc) {

        FPType acc {0};

        for (unsigned ic = 0; ic < numInpChans; ++ic) {
          acc += inPartial[inIdx + ic][oc];
        }

        outPartial[elem][oc] = acc;
      }
    }
    return true;
  }
};

template class WgdReduce<float, 4, 4>;
template class WgdReduce<half, 4, 4>;



template <class FPType, unsigned patchSizeX, unsigned patchSizeY,
          unsigned kernelX, unsigned kernelY>
class WgdInverseTransform : public Vertex {

  /* Set this to true if transform is stored in transposed order */
  static constexpr bool transpose = true;

  FPType rdTf(const unsigned base, const unsigned row, const unsigned col,
              const unsigned el) const {
    return dTf[base+col*patchSizeX+row][el];
  }

  FPType& wrOut(const unsigned base,  unsigned row, const unsigned col,
                const unsigned el) {
    const unsigned numOutCols = patchSizeY - kernelY + 1;
    const unsigned numOutRows = patchSizeX - kernelX + 1;
    if (!transpose) {
      return dOut[base + col * numOutRows + row][el];
    }
    else
    {
      return dOut[base + row * numOutCols + col][el];
    }
  }

public:
  /* The data transform vector dTf is an array of vectors each of length
   * depthDim. The 1D vectors are stacked to have 16 elements called a group
   * which are rows and columns needed to compute the inverse transform.
   */
  Vector<Input<Vector<FPType>>> dTf;

  /* Each output vector in the array of vectors is of length depthDim.
   * numOutCols*numOutRows vectors are produced for each group
   */
  Vector<Output<Vector<FPType, ONE_PTR>>, ONE_PTR> dOut;

  bool compute() {

    const unsigned numInCols = patchSizeY;
    const unsigned numInRows = patchSizeX;
    const unsigned numOutCols = patchSizeY - kernelY + 1;
    const unsigned numOutRows = patchSizeX - kernelX + 1;

    assert(numInCols == 4);
    assert(numInRows == 4);
    assert(kernelX == 3);
    assert(kernelY == 3);

    const unsigned nGroups = dTf.size() / (numInCols * numInRows);

    for (unsigned gr = 0; gr < nGroups; ++gr) {
      unsigned grInOff = gr * numInCols * numInRows;
      unsigned grOutOff = gr * numOutCols * numOutRows;
      const unsigned depthDim = dTf[0].size();

      for (unsigned elem = 0; elem < depthDim; ++elem) {
        FPType e = rdTf(grInOff, 0, 0, elem) + rdTf(grInOff, 0, 1, elem)
                                             + rdTf(grInOff, 0, 2, elem);
        FPType f = rdTf(grInOff, 0, 1, elem) - rdTf(grInOff, 0, 2, elem)
                                             - rdTf(grInOff, 0, 3, elem);

        FPType a = rdTf(grInOff, 1, 0, elem) + rdTf(grInOff, 1, 1, elem)
                                             + rdTf(grInOff, 1, 2, elem);
        FPType c = rdTf(grInOff, 1, 1, elem) - rdTf(grInOff, 1, 2, elem)
                                             - rdTf(grInOff, 1, 3, elem);

        FPType b = rdTf(grInOff, 2, 0, elem) + rdTf(grInOff, 2, 1, elem)
                                             + rdTf(grInOff, 2, 2, elem);
        FPType d = rdTf(grInOff, 2, 1, elem) - rdTf(grInOff, 2, 2, elem)
                                             - rdTf(grInOff, 2, 3, elem);

        FPType g = rdTf(grInOff, 3, 0, elem) + rdTf(grInOff, 3, 1, elem)
                                             + rdTf(grInOff, 3, 2, elem);
        FPType h = rdTf(grInOff, 3, 1, elem) - rdTf(grInOff, 3, 2, elem)
                                             - rdTf(grInOff, 3, 3, elem);

        wrOut(grOutOff, 0, 0, elem) = a + b + e;
        wrOut(grOutOff, 1, 0, elem) = a - b - g;
        wrOut(grOutOff, 0, 1, elem) = c + d + f;
        wrOut(grOutOff, 1, 1, elem) = c - d - h;
      }
    }
    return true;
  }
};

template class WgdInverseTransform<float, 4, 4, 3, 3>;
template class WgdInverseTransform<half, 4, 4, 3, 3>;


template <class FPType>
class WgdConvComplete : public Vertex {

public:
  /* Each input vector is a of length "vecLen"
   */
  Vector<Input<Vector<FPType>>> dIn;

  /* The output activation once non-linearity is applied
   */
  Vector<Output<Vector<FPType, ONE_PTR>>, ONE_PTR> act;

  bool compute() {
    const unsigned nGroups = dIn.size();
    const unsigned vecLen = dIn[0].size();

    for (unsigned gr = 0; gr < nGroups; ++gr) {
      for (unsigned el = 0; el < vecLen; ++el) {
        act[gr][el] = dIn[gr][el];
      }
    }
    return true;
  }
};

template class WgdConvComplete<float>;
template class WgdConvComplete<half>;

template <typename T>
class
[[poplar::constraint("elem(**src) != elem(**dst)")]]
Transpose2d : public Vertex {
public:
  Vector<Input<Vector<T, ONE_PTR,8>>> src;
  Vector<Output<Vector<T, ONE_PTR,8>>, ONE_PTR> dst;
  // TODO specialize the vertex based on the value of this field to avoid extra
  // memory usage.
  unsigned numSrcRows;
  unsigned numSrcColumns;

  IS_EXTERNAL_CODELET(true);

  bool compute() {
    const auto numTranspositions = src.size();
    for (unsigned i = 0; i != numTranspositions; ++i) {
      for (unsigned x = 0; x != numSrcColumns; ++x) {
        for (unsigned y = 0; y != numSrcRows; ++y) {
          dst[i][x * numSrcRows + y] = src[i][y * numSrcColumns + x];
        }
      }
    }
    return true;
  }
};

template class Transpose2d<float>;
template class Transpose2d<half>;

template <class FPType>
class
AddToChannel : public SupervisorVertex {
public:
  Input<Vector<FPType, TWO_PTR, 8>> addend;
  InOut<Vector<FPType, ONE_PTR, 8, true>> acts;
  // actsBlockCount = acts.size() / addend.size();
  // actsBlockCountPacked = (actsBlockCount/6 << 3) | (actsBlockCount % 6)
  uint16_t actsBlockCountPacked;

  IS_EXTERNAL_CODELET(true);

  bool compute() {
    unsigned chansPerGroup = addend.size();
    unsigned actsBlockCount = (actsBlockCountPacked >> 3) * 6
                              + (actsBlockCountPacked & 0x07);
    for (unsigned j = 0; j != actsBlockCount; ++j) {
      for (unsigned k = 0; k != chansPerGroup; ++k) {
        acts[j * chansPerGroup + k] += addend[k];
      }
    }
    return true;
  }
};
template class AddToChannel<float>;
template class AddToChannel<half>;

template <class FPType>
class
AddToChannel2D : public Vertex {
public:
  // n is equal to addend.size(), addendLen.size(), acts.size()
  // and actsBlockCount.size()
  uint32_t n;
  Vector<Input<Vector<FPType, ONE_PTR, 8>>, ONE_PTR> addend;
  Vector<uint16_t, ONE_PTR> addendLen;
  Vector<InOut<Vector<FPType, ONE_PTR, 8, true>>, ONE_PTR> acts;
  Vector<uint16_t, ONE_PTR> actsBlockCount;

  IS_EXTERNAL_CODELET(true);

  bool compute() {
    for (unsigned i = 0; i != n; ++i) {
        unsigned blockCount = actsBlockCount[i];
        unsigned len = addendLen[i];

        for (unsigned b = 0; b != blockCount; ++b) {
            for (unsigned a = 0; a != len; ++a) {
                acts[i][b * len + a] += addend[i][a];
            }
        }
    }

    return true;
  }
};
template class AddToChannel2D<float>;
template class AddToChannel2D<half>;

template <class FPType>
class
ScaledAddToChannel : public SupervisorVertex {
public:
  Input<Vector<FPType, TWO_PTR, 8>> addend;
  InOut<Vector<FPType, ONE_PTR, 8, true>> acts;
  // actsBlockCount = acts.size() / addend.size();
  // actsBlockCountPacked = (actsBlockCount/6 << 3) | (actsBlockCount % 6)
  uint16_t actsBlockCountPacked;
  FPType scale;

  IS_EXTERNAL_CODELET(true);

  bool compute() {
    unsigned chansPerGroup = addend.size();
    unsigned actsBlockCount = (actsBlockCountPacked >> 3) * 6
                              + (actsBlockCountPacked & 0x07);
    for (unsigned j = 0; j != actsBlockCount; ++j) {
      for (unsigned k = 0; k != chansPerGroup; ++k) {
        acts[j * chansPerGroup + k] += addend[k] * scale;
      }
    }
    return true;
  }
};

template class ScaledAddToChannel<float>;
template class ScaledAddToChannel<half>;

template <class FPType>
class
ScaledAddToChannel2D : public Vertex {
public:
  // n is equal to addend.size(), addendLen.size(), acts.size()
  // and actsBlockCount.size()
  uint32_t n;
  Vector<Input<Vector<FPType, ONE_PTR, 8>>, ONE_PTR> addend;
  Vector<uint16_t, ONE_PTR> addendLen;
  Vector<InOut<Vector<FPType, ONE_PTR, 8, true>>, ONE_PTR> acts;
  Vector<uint16_t, ONE_PTR> actsBlockCount;
  FPType scale;

  IS_EXTERNAL_CODELET(true);

  bool compute() {
    for (unsigned i = 0; i != n; ++i) {
        unsigned blockCount = actsBlockCount[i];
        unsigned len = addendLen[i];

        for (unsigned b = 0; b != blockCount; ++b) {
            for (unsigned a = 0; a != len; ++a) {
                acts[i][b * len + a] += addend[i][a] * scale;
            }
        }
    }

    return true;
  }
};

template class ScaledAddToChannel2D<float>;
template class ScaledAddToChannel2D<half>;

template <class FPType>
class
[[poplar::constraint("elem(**actsIn) != elem(**actsOut)",
                     "elem(**actsIn) != elem(**scale)",
                     "elem(**scale) != elem(**actsOut)")]]
ChannelMul2D : public Vertex {
public:
  Vector<Input<Vector<FPType>>> actsIn;
  Vector<Output<Vector<FPType, ONE_PTR>>, ONE_PTR> actsOut;
  Vector<Input<Vector<FPType>>, ONE_PTR> scale;

  bool compute() {
    unsigned n = actsIn.size();
    for (unsigned i = 0; i != n; ++i) {
      unsigned chansPerGroup = scale[i].size();
      unsigned len = actsIn[i].size() / chansPerGroup;
      for (unsigned j = 0; j != len; ++j) {
        for (unsigned k = 0; k != chansPerGroup; ++k) {
          actsOut[i][j * chansPerGroup + k] =
            actsIn[i][j * chansPerGroup + k] * scale[i][k];
        }
      }
    }
    return true;
  }
};

template class ChannelMul2D<float>;
template class ChannelMul2D<half>;

template <class FPType>
class
[[poplar::constraint("elem(*actsIn) != elem(*actsOut)",
                     "elem(*actsIn) != elem(*scale)",
                     "elem(*scale) != elem(*actsOut)")]]
ChannelMul : public SupervisorVertex {
public:
  Input<Vector<FPType>> actsIn;
  Output<Vector<FPType, ONE_PTR>> actsOut;
  Input<Vector<FPType>> scale;

  bool compute() {
    unsigned chansPerGroup = scale.size();
    unsigned len = actsIn.size() / chansPerGroup;
    for (unsigned j = 0; j != len; ++j) {
      for (unsigned k = 0; k != chansPerGroup; ++k) {
        actsOut[j * chansPerGroup + k] =
        actsIn[j * chansPerGroup + k] * scale[k];
      }
    }
    return true;
  }
};

template class ChannelMul<float>;
template class ChannelMul<half>;

template <class MeanType, class PowerType, class OutType>
class InverseStdDeviation : public Vertex {
public:

  Vector<Input<Vector<MeanType>>> mean;
  Vector<Input<Vector<PowerType, ONE_PTR>>, ONE_PTR> power;
  Vector<Output<Vector<OutType, ONE_PTR>>, ONE_PTR> iStdDev;
  float eps;

  bool compute() {

    for (unsigned i = 0; i != mean.size(); ++i) {

      for (unsigned j = 0; j != mean[i].size(); ++j) {
        float varianceEst = power[i][j] - mean[i][j] * mean[i][j] + eps;
        float invStdDev = sqrt(1.0 / varianceEst);
        iStdDev[i][j] = invStdDev;
      }
    }
    return true;
  }
};

template class InverseStdDeviation<float, float, float>;
template class InverseStdDeviation<float, float, half>;
template class InverseStdDeviation<half, float, half>;
template class InverseStdDeviation<half, half, half>;

template <class T>
class
[[poplar::constraint("elem(*weights) != elem(**out)")]]
OuterProduct : public Vertex {
public:
  Input<Vector<T>> in;
  Input<Vector<T, ONE_PTR,8>> weights;
  Vector<Output<Vector<T, ONE_PTR,8>>> out;
  unsigned chansPerGroup;

  IS_EXTERNAL_CODELET(true);
  bool compute() {
    const auto width = in.size();
    const auto numChanGroups = out.size();

     for (unsigned g = 0; g != numChanGroups; ++g) {
      for (unsigned chanInGroup = 0; chanInGroup != chansPerGroup;
           ++chanInGroup) {
        const auto c = chanInGroup + g * chansPerGroup;
        for (unsigned x = 0; x != width; ++x) {
          out[g][chanInGroup + x * chansPerGroup] = in[x] * weights[c];
        }
      }
    }
    return true;
  }
};

template class OuterProduct<float>;
template class OuterProduct<half>;

template <typename OutType, typename PartialsType>
class
ReduceAdd : public Vertex {
public:
  Vector<Input<Vector<PartialsType, ONE_PTR, 8, false>>, ONE_PTR> partials;
  Output<VectorList<OutType, DELTAN, 4>> out;
  unsigned short numPartials;

  IS_EXTERNAL_CODELET(true);
  bool compute() {
    unsigned numReductions = out.size();
    for (unsigned r = 0; r < numReductions; ++r) {
      unsigned numElem = out[r].size();
      for (unsigned i = 0; i < numElem; ++i) {
        float sum = 0;
        for (unsigned j = 0; j < numPartials; ++j) {
          sum += partials[r * numPartials + j][i];
        }
        out[r][i] = sum;
      }
    }
    return true;
  }
};

template class ReduceAdd<float, float>;
template class ReduceAdd<half, float>;
template class ReduceAdd<float, half>;
template class ReduceAdd<half, half>;

} // end namespace poplin