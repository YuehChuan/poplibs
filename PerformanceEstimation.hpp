#ifndef _performance_estimation_h_
#define _performance_estimation_h_

#include <cstdint>

inline std::uint64_t getDenseDotProductCycles(bool isFloat, unsigned size) {
  if (isFloat) {
    return (size + 1) / 2 + 2;
  }
  return (size + 3) / 4 + 2;
}

inline std::uint64_t
getConvPartialCycleEstimate(bool isFloat, unsigned inChansPerGroup,
                            unsigned stride, unsigned kernelSize,
                            unsigned inputGroupsPerOutput,
                            unsigned outputHeight,
                            unsigned outputWidth,
                            unsigned outChansPerGroup)
{
  unsigned vertexOverhead = 5;
  if (!isFloat && inChansPerGroup == 16 && outChansPerGroup == 4 &&
      stride == 1 && kernelSize == 1) {
    unsigned warmUpCycles = 19;
    unsigned innerLoopCycles =
        outputWidth * outputHeight * outChansPerGroup;
    unsigned coolDownCycles = 5;
    unsigned cycleCount = inputGroupsPerOutput *
                          (warmUpCycles + innerLoopCycles + coolDownCycles);
    return cycleCount;
  }
  return vertexOverhead +
         outChansPerGroup * outputWidth * outputHeight * inputGroupsPerOutput *
         (1 + getDenseDotProductCycles(isFloat, kernelSize * inChansPerGroup));
}

inline std::uint64_t
getFullyConnectedPartialCycleEstimate(bool isFloat, unsigned size) {
  return 5 + getDenseDotProductCycles(isFloat, size);
}

#endif // _performance_estimation_h_
