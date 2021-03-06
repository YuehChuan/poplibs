include(GNUInstallDirs)

get_target_property(POPC_EXECUTABLE popc_bin LOCATION)

find_package(Boost 1.65.1 REQUIRED)

add_library(popsparse SHARED
  codelets.cpp
  BlockSparseMatMul.cpp
  BSMatrix.cpp
  HyperGraph.cpp
  HyperGraphBlock.cpp
  HyperGraphStripV0.cpp
  HyperGraphStrip.cpp
  HyperGraphBlockZoltan.cpp
  HyperGraphBlockNaive.cpp
  HyperGraphPartitioner.cpp
  ZoltanPartitioner.cpp
  BalancedPartitioner.cpp
  BSOps.cpp
  BSUtils.cpp
  BSNonLinearity.cpp
  FullyConnected.cpp
  FullyConnectedOnTile.hpp
  FullyConnectedOnTile.cpp
  FullyConnectedOptions.hpp
  FullyConnectedOptions.cpp
  FullyConnectedParams.cpp
  FullyConnectedPlan.cpp
  FullyConnectedPlan.hpp  
  FullyConnectedPNMapping.cpp
  FullyConnectedPNMapping.hpp
  FullyConnectedUtils.hpp
  FullyConnectedUtils.cpp
  FullyConnectedVector.hpp
  PerformanceEstimation.hpp
  popsparseCycleEstimators.hpp
  popsparseCycleEstimators.cpp
  SparseMetaInfo.hpp
  SparseStorageInternal.hpp
  SparsityParams.cpp
  SparsePartitioner.cpp
  SparsePartitionerImpl.cpp
  SparsePartitionerImpl.hpp
  ${CMAKE_SOURCE_DIR}/include/popsparse/codelets.hpp
  ${CMAKE_SOURCE_DIR}/include/popsparse/experimental/BlockSparseMatMul.hpp
  ${CMAKE_SOURCE_DIR}/include/popsparse/FullyConnectedParams.hpp
  ${CMAKE_SOURCE_DIR}/include/popsparse/FullyConnected.hpp
  ${CMAKE_SOURCE_DIR}/include/popsparse/SparseTensor.hpp
  ${CMAKE_SOURCE_DIR}/include/popsparse/SparsityParams.hpp
  ${CMAKE_SOURCE_DIR}/include/popsparse/SparseStorageFormats.hpp  
  ${CMAKE_SOURCE_DIR}/include/popsparse/SparsePartitioner.hpp  
  )

target_link_libraries(popsparse
  PUBLIC
    poplar poputil popops poplin ${CMAKE_DL_LIBS} ZOLTAN::zoltan ZOLTAN::simpi
  PRIVATE
    poplibs_support
    popsolver
    Boost::boost
)

target_include_directories(popsparse
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    # Needed for popsparse to access ConvReduce functionality for the timebeing
    $<TARGET_PROPERTY:poplin,INCLUDE_DIRECTORIES>
    .
)

add_gp_library(
  NAME popsparse
  CPP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/SparseDenseMatMulElementWise.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/SparseDenseMatMulElementWiseTranspose.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/SparseDenseMatMulGradWElementWise.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/SparseDenseMatMulGradAElementWise.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/SparseGather.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/Utils.cpp

  ASM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/asm/SparseDenseMatMulElementWise_half_float.S
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/asm/SparseDenseMatMulElementWise_float_float.S
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/asm/SparseGatherElementWise.S
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/asm/SparseDenseMatMulGradWElementWise_float_float.S
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/asm/SparseDenseMatMulGradWElementWise_half_float.S
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/asm/SparseDenseMatMulGradAElementWise_half_float.S
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/asm/SparseDenseMatMulGradAElementWise_float_float.S
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/asm/SparseDenseMatMulTranspElementWise_half_float.S
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/asm/SparseDenseMatMulTranspElementWise_float_float.S

  HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/asm/SparseDenseMatMulGradAElementWise.h.S
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/asm/SparseDenseMatMulElementWise.h.S
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/asm/SparseDenseMatMulTranspElementWise.h.S
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/asm/SparseDenseMatMulGradWElementWise.h.S
    ${CMAKE_CURRENT_SOURCE_DIR}/codelets/asm/SparseDenseMatMulStructs.h.S
    ${CMAKE_CURRENT_SOURCE_DIR}/SparseMetaInfo.hpp
)

install(TARGETS popsparse
        EXPORT popsparse
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT popsparse
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT popsparse
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/popsparse
        FILE popsparse-targets.cmake
        COMPONENT popsparse)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/popsparse
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT popsparse)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/popsparse/experimental
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/popsparse
        COMPONENT popsparse)
