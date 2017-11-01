#include <algorithm>
#include <boost/multi_array.hpp>
#include <boost/program_options.hpp>
#include <boost/test/tools/floating_point_comparison.hpp>
#include <cassert>
#include <exception>
#include <istream>
#include <ostream>
#include <poplar/Graph.hpp>
#include <poplar/Engine.hpp>
#include <poplar/IPUModel.hpp>
#include <popstd/TileMapping.hpp>
#include <popconv/Convolution.hpp>
#include <popconv/ConvUtil.hpp>
#include <popstd/exceptions.hpp>
#include <poplar/HalfFloat.hpp>
#include <popstd/codelets.hpp>
#include <popstd/Add.hpp>
#include <popreduce/codelets.hpp>
#include <popconv/codelets.hpp>
#include <popnn/NonLinearity.hpp>
#include <poplib_test/Convolution.hpp>
#include <poplib_test/NonLinearity.hpp>
#include <poplib_test/Pass.hpp>
#include <poplib_test/Util.hpp>
#include <util/Compiler.hpp>
#include <random>

using namespace poplar;
using namespace poplar::program;
using namespace poplib_test::util;
using namespace popstd;
using poplib_test::Pass;
int main(int argc, char **argv) {
  namespace po = boost::program_options;

  bool useCpuModel;
  unsigned fwdInChansPerConvGroup;
  unsigned fwdOutChansPerConvGroup;
  unsigned width;
  unsigned height;
  unsigned kernelHeight;
  unsigned kernelWidth;
  unsigned numConvGroups = 1;
  std::vector<int> paddingLower = {0, 0};
  std::vector<int> paddingUpper = {0, 0};
  std::vector<unsigned> inDilation = {1, 1};
  std::vector<int> kernelPaddingLower = {0, 0};
  std::vector<int> kernelPaddingUpper = {0, 0};
  std::vector<unsigned> kernelDilation = {1, 1};
  std::vector<unsigned> stride = {1, 1};
  unsigned batchSize;
  bool bias;
  FPDataType dataType;
  FPDataType partialsType;
  double relativeTolerance;
  IPUModel ipuModel;
  ipuModel.IPUExchangeType =
      IPUModel::ExchangeType::AGGRESSIVE_MULTICAST;
  bool reportPlan;
  bool reportTensorStorage;

  /* these are used when the same value is shared across both height and width*/
  unsigned kernelSize;
  int sharedPadding;
  int sharedPaddingHeight;
  int sharedPaddingWidth;
  unsigned sharedInDilation;
  int sharedKernelPadding;
  int sharedKernelPaddingHeight;
  int sharedKernelPaddingWidth;
  unsigned sharedKernelDilation;
  unsigned sharedStride;
  Pass pass = Pass::ALL;
  popconv::ConvOptions convOptions;
  popconv::PlanningCache cache;
  convOptions.cache = &cache;
  po::options_description desc("Options");
  desc.add_options()
    ("help", "Produce help message")
    ("use-cpu", po::value<bool>(&useCpuModel)->default_value(false),
     "When true, use a CPU model of the device. Otherwise use the IPU model")
    ("input-channels", po::value<unsigned>(&fwdInChansPerConvGroup)->required(),
     "Number of input channels per grouped convolution")
    ("output-channels",
     po::value<unsigned>(&fwdOutChansPerConvGroup)->required(),
     "Number of output channels per grouped convolution")
    ("width", po::value<unsigned>(&width)->required(), "Field width")
    ("height", po::value<unsigned>(&height)->required(), "Field height")
    ("kernel-size",
      po::value<unsigned>(&kernelSize)->default_value(1),
     "Size of square kernel. If set, it is an error to also set either "
     "kernel-height and/or kernel-width")
    ("kernel-height",
      po::value<unsigned>(&kernelHeight)->default_value(1),
     "Size of kernel height")
    ("kernel-width",
      po::value<unsigned>(&kernelWidth)->default_value(1),
     "Size of kernel width")
    ("bias", po::value<bool>(&bias)->default_value(true),
     "Add a bias to each channel")
    ("data-type",
     po::value<FPDataType>(&dataType)->default_value(FPDataType::HALF),
     "Type of the data and the parameters")
    ("partials-type",
     po::value<FPDataType>(&partialsType)->default_value(FPDataType::FLOAT),
     "Type of partials")
    ("padding", po::value<int>(&sharedPadding)->default_value(0),
     "Amount of zero padding for height and width. If set, it is an "
     "error to also set any other padding value")
    ("padding-height", po::value<int>(&sharedPaddingHeight)->default_value(0),
     "Amount of zero padding in the height dimension, upper and lower")
    ("padding-width", po::value<int>(&sharedPaddingWidth)->default_value(0),
     "Amount of zero padding in the width dimension, upper and lower")
    ("padding-height-lower",
     po::value<int>(&paddingLower[0])->default_value(0),
     "Amount of zero padding in the height dimension, lower edge")
    ("padding-width-lower",
     po::value<int>(&paddingLower[1])->default_value(0),
     "Amount of zero padding in the width dimension, lower edge")
    ("padding-height-upper",
     po::value<int>(&paddingUpper[0])->default_value(0),
     "Amount of zero padding in the height dimension, upper edge")
    ("padding-width-upper",
     po::value<int>(&paddingUpper[1])->default_value(0),
     "Amount of zero padding in the width dimension, upper edge")
    ("in-dilation", po::value<unsigned>(&sharedInDilation)->default_value(1),
     "Input dilation for both height and width. If set, it is an error "
     "to also set either inDilation-height and/or inDilation-width")
    ("in-dilation-height",
     po::value<unsigned>(&inDilation[0])->default_value(1),
     "Input dilation in the height dimension")
    ("in-dilation-width", po::value<unsigned>(&inDilation[1])->default_value(1),
     "Input dilation in the width dimension")
    ("kernel-padding",
     po::value<int>(&sharedKernelPadding)->default_value(0),
     "Amount of zero kernel padding for height and width. If set, it is an "
     "error to also set any other kernel padding value")
    ("kernel-padding-height",
     po::value<int>(&sharedKernelPaddingHeight)->default_value(0),
     "Amount of zero kernel padding in the height dimension, upper and lower")
    ("kernel-padding-width",
     po::value<int>(&sharedKernelPaddingWidth)->default_value(0),
     "Amount of zero kernel padding in the width dimension, upper and lower")
    ("kernel-padding-height-lower",
     po::value<int>(&kernelPaddingLower[0])->default_value(0),
     "Amount of zero kernel padding in the height dimension, lower edge")
    ("kernel-padding-width-lower",
     po::value<int>(&kernelPaddingLower[1])->default_value(0),
     "Amount of zero kernel padding in the width dimension, lower edge")
    ("kernel-padding-height-upper",
     po::value<int>(&kernelPaddingUpper[0])->default_value(0),
     "Amount of zero kernel padding in the height dimension, upper edge")
    ("kernel-padding-width-upper",
     po::value<int>(&kernelPaddingUpper[1])->default_value(0),
     "Amount of zero kernel padding in the width dimension, upper edge")
    ("kernel-dilation",
     po::value<unsigned>(&sharedKernelDilation)->default_value(1),
     "Kernel dilation for both height and width. If set, it is an error "
     "to also set either kernelDilation-height and/or kernelDilation-width")
    ("kernel-dilation-height",
     po::value<unsigned>(&kernelDilation[0])->default_value(1),
     "Kernel dilation in the height dimension")
    ("kernel-dilation-width",
     po::value<unsigned>(&kernelDilation[1])->default_value(1),
     "Kernel dilation in the width dimension")
    ("stride", po::value<unsigned>(&sharedStride)->default_value(1),
     "Stride for both height and width. If set, it is an error "
     "to also set either stride-height and/or stride-width")
    ("stride-height", po::value<unsigned>(&stride[0])->default_value(1),
     "Stride in the height dimension")
    ("stride-width", po::value<unsigned>(&stride[1])->default_value(1),
     "Stride in the width dimension")
    ("single-phase",
     po::value<Pass>(&pass)->default_value(pass),
     "Run phase all | fwd | bwd | wu")
    ("tolerance", po::value<double>(&relativeTolerance)->default_value(0.01),
     "Relative tolerance to use when validating results against the reference "
     "model")
    ("tiles-per-ipu",
     po::value<unsigned>(&ipuModel.tilesPerIPU)->
                           default_value(ipuModel.tilesPerIPU),
     "Number of tiles per IPU")
    ("batch-size",
     po::value<unsigned>(&batchSize)->default_value(1),
     "Batch size")
    ("conv-groups",
     po::value<unsigned>(&numConvGroups)->default_value(1),
     "Number of convolution groups in grouped convolution")
    ("use-winograd-conv",
     po::value<bool>(&convOptions.useWinograd)->default_value(0),
     "Use winograd convolution")
    ("winograd-patch-size",
      po::value<unsigned>(&convOptions.winogradPatchSize)->default_value(4),
     "Square patch size to use in winograd convolution")
    ("percent-cyc-excess-for-mem-optim",
     po::value<unsigned>(
       &convOptions.percentageCyclesExcessForMemOptim
     )->default_value(0),
     "Percentage cycles excess to use for memory optimisation. "
     "if 0, no memory optimisation is performed")
    ("weight-update-method",
     po::value<popconv::WeightUpdateMethod>(
         &convOptions.weightUpdateMethod
     )->default_value(convOptions.weightUpdateMethod),
     "Weight update method: amp | auto")
    ("report-plan", po::value<bool>(&reportPlan)->default_value(false),
     "Display plan")
    ("report-tensor-storage",
     po::value<bool>(&reportTensorStorage)->default_value(false),
     "Report tensor storage information")
  ;
  po::variables_map vm;
  try {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    if (vm.count("help")) {
      std::cout << desc << "\n";
      return 1;
    }
    po::notify(vm);
  } catch (std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }

  if (!vm["kernel-size"].defaulted()) {
    if (!vm["kernel-height"].defaulted()) {
      std::cerr << "--kernel as well as --kernel-height set\n";
      return 1;
    }
    if (!vm["kernel-width"].defaulted()) {
      std::cerr << "--kernel as well as --kernel-width set\n";
      return 1;
    }
    kernelHeight = kernelSize;
    kernelWidth = kernelSize;
  }

  if (!vm["padding"].defaulted()) {
    const char *conflictingOptions[] = {
      "padding-height",
      "padding-width",
      "padding-height-lower",
      "padding-width-lower",
      "padding-height-upper",
      "padding-width-upper",
    };
    for (auto option : conflictingOptions) {
      if (!vm[option].defaulted()) {
        std::cerr << "--padding as well as --" << option << " set\n";
        return 1;
      }
    }
    std::fill(paddingLower.begin(), paddingLower.end(), sharedPadding);
    std::fill(paddingUpper.begin(), paddingUpper.end(), sharedPadding);
  }

  if (!vm["padding-height"].defaulted()) {
    if (!vm["padding-height-lower"].defaulted()) {
      std::cerr << "--padding-height as well as --padding-height-lower set\n";
      return 1;
    }
    if (!vm["padding-height-upper"].defaulted()) {
      std::cerr << "--padding-height as well as --padding-height-upper set\n";
      return 1;
    }
    paddingLower[0] = sharedPaddingHeight;
    paddingUpper[0] = sharedPaddingHeight;
  }

  if (!vm["padding-width"].defaulted()) {
    if (!vm["padding-width-lower"].defaulted()) {
      std::cerr << "--padding-width as well as --padding-width-lower set\n";
      return 1;
    }
    if (!vm["padding-width-upper"].defaulted()) {
      std::cerr << "--padding-width as well as --padding-width-upper set\n";
      return 1;
    }
    paddingLower[1] = sharedPaddingWidth;
    paddingUpper[1] = sharedPaddingWidth;
  }

  if (!vm["in-dilation"].defaulted()) {
    if (!vm["in-dilation-height"].defaulted()) {
      std::cerr << "--in-dilation as well as --in-dilation-height set\n";
      return 1;
    }
    if (!vm["in-dilation-width"].defaulted()) {
      std::cerr << "--in-dilation as well as --in-dilation-width set\n";
      return 1;
    }
    std::fill(inDilation.begin(), inDilation.end(), sharedInDilation);
  }

  if (!vm["kernel-padding"].defaulted()) {
    const char *conflictingOptions[] = {
      "kernel-padding-height",
      "kernel-padding-width",
      "kernel-padding-height-lower",
      "kernel-padding-width-lower",
      "kernel-padding-height-upper",
      "kernel-padding-width-upper",
    };
    for (auto option : conflictingOptions) {
      if (!vm[option].defaulted()) {
        std::cerr << "--kernel-padding as well as --" << option << " set\n";
        return 1;
      }
    }
    std::fill(kernelPaddingLower.begin(), kernelPaddingLower.end(),
              sharedKernelPadding);
    std::fill(kernelPaddingUpper.begin(), kernelPaddingUpper.end(),
              sharedKernelPadding);
  }

  if (!vm["kernel-padding-height"].defaulted()) {
    if (!vm["kernel-padding-height-lower"].defaulted()) {
      std::cerr << "--kernel-padding-height as well as "
                   "--kernel-padding-height-lower set\n";
      return 1;
    }
    if (!vm["kernel-padding-height-upper"].defaulted()) {
      std::cerr << "--kernel-padding-height as well as "
                   "--kernel-padding-height-upper set\n";
      return 1;
    }
    kernelPaddingLower[0] = sharedKernelPaddingHeight;
    kernelPaddingUpper[0] = sharedKernelPaddingHeight;
  }

  if (!vm["kernel-padding-width"].defaulted()) {
    if (!vm["kernel-padding-width-lower"].defaulted()) {
      std::cerr << "--kernel-padding-width as well as "
                   "--kernel-padding-width-lower set\n";
      return 1;
    }
    if (!vm["kernel-padding-width-upper"].defaulted()) {
      std::cerr << "--kernel-padding-width as well as "
                   "--kernel-padding-width-upper set\n";
      return 1;
    }
    kernelPaddingLower[1] = sharedKernelPaddingWidth;
    kernelPaddingUpper[1] = sharedKernelPaddingWidth;
  }

  if (!vm["kernel-dilation"].defaulted()) {
    if (!vm["kernel-dilation-height"].defaulted()) {
      std::cerr << "--kernel-dilation as well as "
                   "--kernel-dilation-height set\n";
      return 1;
    }
    if (!vm["kernel-dilation-width"].defaulted()) {
      std::cerr << "--kernel-dilation as well as --kernel-dilation-width set\n";
      return 1;
    }
    std::fill(kernelDilation.begin(), kernelDilation.end(),
              sharedKernelDilation);
  }

  if (!vm["stride"].defaulted()) {
    if (!vm["stride-height"].defaulted()) {
      std::cerr << "--stride as well as --stride-height set\n";
      return 1;
    }
    if (!vm["stride-width"].defaulted()) {
      std::cerr << "--stride as well as --stride-width set\n";
      return 1;
    }
    std::fill(stride.begin(), stride.end(), sharedStride);
  }
  const auto fwdInChans = fwdInChansPerConvGroup * numConvGroups;
  const auto fwdOutChans = fwdOutChansPerConvGroup * numConvGroups;

  bool doFwdPass = pass == Pass::ALL || pass == Pass::FWD;
  bool doBwdPass = pass == Pass::ALL || pass == Pass::BWD;
  bool doWuPass = pass == Pass::ALL || pass == Pass::WU;

  Device dev = useCpuModel ? Device::createCPUDevice() :
                             ipuModel.createDevice();
  Graph graph(dev);
  popstd::addCodelets(graph);
  popreduce::addCodelets(graph);
  popconv::addCodelets(graph);

  std::string dataTypeStr(asString(dataType));
  std::string partialsTypeStr(asString(partialsType));

  const auto params =
      popconv::ConvParams(dataTypeStr,
                          batchSize,
                          {height, width},
                          {kernelHeight, kernelWidth},
                          fwdInChansPerConvGroup,
                          fwdOutChansPerConvGroup,
                          stride,
                          paddingLower,
                          paddingUpper,
                          inDilation,
                          kernelPaddingLower,
                          kernelPaddingUpper,
                          kernelDilation,
                          numConvGroups);
  if (params.getPaddedDilatedInputSize(0) < 0 ||
      params.getPaddedDilatedInputSize(1) < 0) {
    throw popstd::poplib_error("Convolution pass does not support "
                               "padding that truncates more than the input "
                               "size");
  }
  if (params.getPaddedDilatedKernelSize(0) < 0 ||
      params.getPaddedDilatedKernelSize(1) < 0) {
    throw popstd::poplib_error("Convolution pass does not support "
                               "padding that truncates more than the kernel "
                               "size");
  }


  const auto outHeight = params.getOutputHeight();
  const auto outWidth = params.getOutputWidth();

  const auto bwdParams = getGradientParams(params);

  // Create tensors.
  Tensor prevAct =
      popconv::createInput(graph, params, "prevAct", convOptions);
  Tensor weights =
      popconv::createWeights(graph, params, "weights", convOptions);

  Tensor prevDeltas, zDeltas;
  if (doBwdPass || doWuPass) {
    zDeltas = popconv::createInput(graph, bwdParams, "zDeltas", convOptions);
  }

  auto fwdProg = Sequence();
  // Always generate the fwd program as it maps the weights and biases. Only
  // actually create the engined if the fwd pass is to be run
  Tensor nextAct = popconv::convolution(graph, prevAct, weights, params, false,
                                        fwdProg, "", convOptions);
  if (reportPlan) {
    std::cout << "Forward plan:\n";
    popconv::reportPlanInfo(std::cout, graph, params, convOptions);
  }
  Tensor biases;
  if (bias) {
    biases = popconv::createBiases(graph, nextAct);
    popconv::addBias(graph, nextAct, biases, fwdProg, "");
  }
  if (!doFwdPass)
    fwdProg = Sequence();

  auto revProg = Sequence();
  const auto learningRate = 0.5;

  if (doBwdPass) {
    prevDeltas = popconv::convolution(graph, zDeltas, weights, bwdParams,
                                      true, revProg, "",
                                      convOptions);
    if (reportPlan) {
      std::cout << "Backward plan:\n";
      popconv::reportPlanInfo(std::cout, graph, bwdParams, convOptions);
    }
  }
  if (doWuPass) {
    popconv::convolutionWeightUpdate(graph, zDeltas, weights, prevAct,
                                     params, learningRate,
                                     revProg, "", convOptions);
    if (bias) {
      popconv::convolutionBiasUpdate(graph, zDeltas, biases, learningRate,
                                     partialsTypeStr, revProg);
    }
    if (reportPlan) {
      std::cout << "WU plan:\n";
      popconv::reportWeightUpdatePlanInfo(std::cout, graph, zDeltas, prevAct,
                                          params, convOptions);
    }
  }
  std::vector<std::pair<std::string, char *>> tmap;
  auto rawHostPrevAct = allocateHostMemoryForTensor(prevAct, "prevAct", graph,
                                                    tmap);
  auto rawHostWeights = allocateHostMemoryForTensor(weights, "weights", graph,
                                                    tmap);
  std::unique_ptr<char []> rawHostBiases;
  if (bias) {
    rawHostBiases = allocateHostMemoryForTensor(biases, "biases", graph,
                                                tmap);
  }
  auto rawHostNextAct = allocateHostMemoryForTensor(nextAct, "nextAct", graph,
                                                    tmap);
  std::unique_ptr<char[]> rawHostZDeltas;
  std::unique_ptr<char[]> rawHostPrevDeltas;
  if (doBwdPass || doWuPass) {
    rawHostZDeltas = allocateHostMemoryForTensor(zDeltas, "zDeltas", graph,
                                                 tmap);
  }
  if (doBwdPass) {
    rawHostPrevDeltas = allocateHostMemoryForTensor(prevDeltas, "prevDeltas",
                                                    graph, tmap);
  }
  Engine engine(dev, graph, {std::move(fwdProg), std::move(revProg)});

  boost::multi_array<double, 3>
      hostPrevAct(boost::extents[batchSize][fwdInChans][height * width]);
  boost::multi_array<double, 4>
      hostWeights(boost::extents[numConvGroups]
                                [fwdOutChansPerConvGroup]
                                [fwdInChansPerConvGroup]
                                [kernelHeight * kernelWidth]);
  boost::multi_array<double, 1>
      hostBiases(boost::extents[fwdOutChans]);
  boost::multi_array<double, 3>
      hostNextAct(boost::extents[batchSize][fwdOutChans][outHeight * outWidth]);
  std::mt19937 randomEngine;
  writeRandomValues(hostPrevAct, -1.0, +5.0, randomEngine);
  writeRandomValues(hostWeights, -1.0, +7.0, randomEngine);
  if (bias) {
    writeRandomValues(hostBiases, -2.0, +6.0, randomEngine);
  } else {
    std::fill(hostBiases.data(), hostBiases.data() + hostBiases.num_elements(),
              0.0);
  }
  copy(hostPrevAct, dataTypeStr, rawHostPrevAct.get());
  copy(hostWeights, dataTypeStr, rawHostWeights.get());
  if (bias) {
    copy(hostBiases, dataTypeStr, rawHostBiases.get());
  }

  // Run the forward pass.
  upload(engine, tmap);
  engine.run(0); // Run.
  download(engine, tmap);

  // Validate against a reference model.
  bool matchesModel = true;
  copy(dataTypeStr, rawHostNextAct.get(), hostNextAct);
  boost::multi_array<double, 3>
      modelNextAct(boost::extents[batchSize][fwdOutChans]
                                 [outHeight * outWidth]);
  poplib_test::conv::convolution({height, width},
                                 inDilation,
                                 paddingLower,
                                 paddingUpper,
                                 {kernelHeight, kernelWidth},
                                 kernelDilation,
                                 kernelPaddingLower,
                                 kernelPaddingUpper,
                                 stride,
                                 hostPrevAct,
                                 hostWeights, hostBiases, modelNextAct);
  if (doFwdPass) {
    matchesModel &= checkIsClose("fwd", hostNextAct, modelNextAct,
                                 relativeTolerance);
  }

  if (doBwdPass || doWuPass) {
    boost::multi_array<double, 3> hostZDeltas(
      boost::extents[batchSize][bwdParams.getNumInputChans()]
                    [outHeight * outWidth]
    );
    boost::multi_array<double, 3> hostPrevDeltas(
      boost::extents[batchSize][params.getNumInputChans()][height * width]
    );
    auto modelWeights = hostWeights;
    auto modelBiases = hostBiases;
    // Run the backwards and/or weight update passes.
    writeRandomValues(hostZDeltas, -3.0, 7.0, randomEngine);
    copy(hostZDeltas, dataTypeStr, rawHostZDeltas.get());
    upload(engine, tmap);
    engine.run(1); // Run.
    download(engine, tmap);

    copy(dataTypeStr, rawHostZDeltas.get(), hostZDeltas);
    if (doBwdPass) {
      copy(dataTypeStr, rawHostPrevDeltas.get(), hostPrevDeltas);
    }
    copy(dataTypeStr, rawHostWeights.get(), hostWeights);
    if (bias) {
      copy(dataTypeStr, rawHostBiases.get(), hostBiases);
    }

    // Validate against a reference model.
    if (doBwdPass) {
      boost::multi_array<double, 3>
          modelPrevDeltas(boost::extents[batchSize][fwdInChans]
                                        [height * width]);
      poplib_test::conv::convolutionBackward(
              {height, width},
              inDilation,
              paddingLower,
              paddingUpper,
              {kernelHeight, kernelWidth},
              kernelDilation,
              kernelPaddingLower,
              kernelPaddingUpper,
              stride,
              hostZDeltas,
              modelWeights,
              modelPrevDeltas);
      matchesModel &= checkIsClose("bwd", hostPrevDeltas, modelPrevDeltas,
                                   relativeTolerance);
    }
    if (doWuPass) {
      poplib_test::conv::weightUpdate({height, width},
                                      inDilation,
                                      paddingLower,
                                      paddingUpper,
                                      {kernelHeight, kernelWidth},
                                      kernelDilation,
                                      kernelPaddingLower,
                                      kernelPaddingUpper,
                                      stride,
                                      learningRate, hostPrevAct,
                                      hostZDeltas, modelWeights, modelBiases);
      matchesModel &= checkIsClose("weights",
                                  hostWeights, modelWeights, relativeTolerance);
      if (bias) {
        matchesModel &= checkIsClose("biases",
                                     hostBiases, modelBiases,
                                     relativeTolerance);
      }
    }
  }

  if (!useCpuModel) {
    Engine::ReportOptions opt;
    opt.doLayerWiseProfile = true;
    if (reportTensorStorage) {
      opt.showTensorStorage = true;
    }
    engine.report(std::cout, opt);
  }

  if (!matchesModel) {
    std::cerr << "Validation failed\n";
    return 1;
  }
  return 0;
}
