include(GNUInstallDirs)

add_library(poplibs_test SHARED
  Convolution.cpp
  Embedding.cpp
  FullyConnected.cpp
  GeneralMatrixAdd.cpp
  GeneralMatrixMultiply.cpp
  Lstm.cpp
  Gru.cpp
  Norms.cpp
  Pooling.cpp
  NonLinearity.cpp
  Multirate.cpp
  Pass.cpp
  Rnn.cpp
  Util.cpp
  ${PROJECT_SOURCE_DIR}/include/poplibs_test/Convolution.hpp
  ${PROJECT_SOURCE_DIR}/include/poplibs_test/Embedding.hpp
  ${PROJECT_SOURCE_DIR}/include/poplibs_test/FullyConnected.hpp
  ${PROJECT_SOURCE_DIR}/include/poplibs_test/GeneralMatrixAdd.hpp
  ${PROJECT_SOURCE_DIR}/include/poplibs_test/GeneralMatrixMultiply.hpp
  ${PROJECT_SOURCE_DIR}/include/poplibs_test/Lstm.hpp
  ${PROJECT_SOURCE_DIR}/include/poplibs_test/Gru.hpp
  ${PROJECT_SOURCE_DIR}/include/poplibs_test/Norms.hpp
  ${PROJECT_SOURCE_DIR}/include/poplibs_test/Pooling.hpp
  ${PROJECT_SOURCE_DIR}/include/poplibs_test/NonLinearity.hpp
  ${PROJECT_SOURCE_DIR}/include/poplibs_test/Multirate.hpp
  ${PROJECT_SOURCE_DIR}/include/poplibs_test/Pass.hpp
  ${PROJECT_SOURCE_DIR}/include/poplibs_test/Rnn.hpp
  ${PROJECT_SOURCE_DIR}/include/poplibs_test/Util.hpp
)

target_link_libraries(poplibs_test
  PUBLIC
    poplar poputil Boost::boost
)

target_include_directories(poplibs_test
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
