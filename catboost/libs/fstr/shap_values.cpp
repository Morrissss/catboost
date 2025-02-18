#include "shap_values.h"

#include "util.h"

#include <catboost/libs/algo/features_data_helpers.h>
#include <catboost/libs/algo/index_calcer.h>
#include <catboost/libs/data_new/features_layout.h>
#include <catboost/libs/helpers/exception.h>
#include <catboost/libs/loggers/logger.h>
#include <catboost/libs/logging/profile_info.h>
#include <catboost/libs/options/restrictions.h>

#include <util/generic/algorithm.h>
#include <util/generic/cast.h>
#include <util/generic/utility.h>
#include <util/generic/ymath.h>
#include <catboost/libs/model/cpu/quantization.h>


using namespace NCB;


namespace {
    struct TFeaturePathElement {
        int Feature;
        double ZeroPathsFraction;
        double OnePathsFraction;
        double Weight;

        TFeaturePathElement() = default;

        TFeaturePathElement(int feature, double zeroPathsFraction, double onePathsFraction, double weight)
            : Feature(feature)
            , ZeroPathsFraction(zeroPathsFraction)
            , OnePathsFraction(onePathsFraction)
            , Weight(weight)
        {
        }
    };
} //anonymous

static TVector<TFeaturePathElement> ExtendFeaturePath(
    const TVector<TFeaturePathElement>& oldFeaturePath,
    double zeroPathsFraction,
    double onePathsFraction,
    int feature
) {
    const size_t pathLength = oldFeaturePath.size();

    TVector<TFeaturePathElement> newFeaturePath(pathLength + 1);
    Copy(oldFeaturePath.begin(), oldFeaturePath.begin() + pathLength, newFeaturePath.begin());

    const double weight = pathLength == 0 ? 1.0 : 0.0;
    newFeaturePath[pathLength] = TFeaturePathElement(feature, zeroPathsFraction, onePathsFraction, weight);

    for (int elementIdx = pathLength - 1; elementIdx >= 0; --elementIdx) {
        newFeaturePath[elementIdx + 1].Weight += onePathsFraction * newFeaturePath[elementIdx].Weight * (elementIdx + 1) / (pathLength + 1);
        newFeaturePath[elementIdx].Weight = zeroPathsFraction * newFeaturePath[elementIdx].Weight * (pathLength - elementIdx) / (pathLength + 1);
    }

    return newFeaturePath;
}

static TVector<TFeaturePathElement> UnwindFeaturePath(
    const TVector<TFeaturePathElement>& oldFeaturePath,
    size_t eraseElementIdx)
{
    const size_t pathLength = oldFeaturePath.size();
    CB_ENSURE(pathLength > 0, "Path to unwind must have at least one element");

    TVector<TFeaturePathElement> newFeaturePath(
        oldFeaturePath.begin(),
        oldFeaturePath.begin() + pathLength - 1);

    for (size_t elementIdx = eraseElementIdx; elementIdx < pathLength - 1; ++elementIdx) {
        newFeaturePath[elementIdx].Feature = oldFeaturePath[elementIdx + 1].Feature;
        newFeaturePath[elementIdx].ZeroPathsFraction = oldFeaturePath[elementIdx + 1].ZeroPathsFraction;
        newFeaturePath[elementIdx].OnePathsFraction = oldFeaturePath[elementIdx + 1].OnePathsFraction;
    }

    const double onePathsFraction = oldFeaturePath[eraseElementIdx].OnePathsFraction;
    const double zeroPathsFraction = oldFeaturePath[eraseElementIdx].ZeroPathsFraction;
    double weightDiff = oldFeaturePath[pathLength - 1].Weight;

    if (!FuzzyEquals(1 + onePathsFraction, 1 + 0.0)) {
        for (int elementIdx = pathLength - 2; elementIdx >= 0; --elementIdx) {
            double oldWeight = newFeaturePath[elementIdx].Weight;
            newFeaturePath[elementIdx].Weight = weightDiff * pathLength
                / (onePathsFraction * (elementIdx + 1));
            weightDiff = oldWeight
                - newFeaturePath[elementIdx].Weight * zeroPathsFraction * (pathLength - elementIdx - 1)
                    / pathLength;
        }
    } else {
        for (int elementIdx = pathLength - 2; elementIdx >= 0; --elementIdx) {
            newFeaturePath[elementIdx].Weight *= pathLength
                / (zeroPathsFraction * (pathLength - elementIdx - 1));
        }
    }

    return newFeaturePath;
}

//TODO(kirillovs): remove asap
static size_t CalcLeafToFallForDocument(
    const NCB::NModelEvaluation::IModelEvaluator* modelEvaluator,
    size_t treeIdx,
    const NCB::NModelEvaluation::IQuantizedData* binarizedFeaturesForBlock,
    size_t documentIdx
) {
    const NModelEvaluation::TCPUEvaluatorQuantizedData* dataPtr = reinterpret_cast<const NModelEvaluation::TCPUEvaluatorQuantizedData*>(binarizedFeaturesForBlock);
    Y_ASSERT(dataPtr);
    auto blockId = documentIdx / NModelEvaluation::FORMULA_EVALUATION_BLOCK_SIZE;
    auto subBlock = dataPtr->ExtractBlock(blockId);
    TVector<NCB::NModelEvaluation::TCalcerIndexType> indexes(subBlock.GetObjectsCount());
    modelEvaluator->CalcLeafIndexes(&subBlock, treeIdx, treeIdx + 1, indexes);

    return indexes[documentIdx % NModelEvaluation::FORMULA_EVALUATION_BLOCK_SIZE];
}

static void CalcInternalShapValuesForLeafRecursive(
    const TObliviousTrees& forest,
    const TVector<int>& binFeatureCombinationClass,
    size_t documentLeafIdx,
    size_t treeIdx,
    int depth,
    const TVector<TVector<double>>& subtreeWeights,
    size_t nodeIdx,
    const TVector<TFeaturePathElement>& oldFeaturePath,
    double zeroPathsFraction,
    double onePathsFraction,
    int feature,
    bool calcInternalValues,
    TVector<TShapValue>* shapValuesInternal
) {
    TVector<TFeaturePathElement> featurePath = ExtendFeaturePath(
        oldFeaturePath,
        zeroPathsFraction,
        onePathsFraction,
        feature);
    auto firstLeafPtr = forest.GetFirstLeafPtrForTree(treeIdx);
    if (depth == forest.TreeSizes[treeIdx]) {
        for (size_t elementIdx = 1; elementIdx < featurePath.size(); ++elementIdx) {
           TVector<TFeaturePathElement> unwoundPath = UnwindFeaturePath(featurePath, elementIdx);
           double weightSum = 0.0;
           for (const TFeaturePathElement& unwoundPathElement : unwoundPath) {
               weightSum += unwoundPathElement.Weight;
           }
           const TFeaturePathElement& element = featurePath[elementIdx];
           const int approxDimension = forest.ApproxDimension;
           const auto sameFeatureShapValue = FindIf(
               shapValuesInternal->begin(),
               shapValuesInternal->end(),
               [element](const TShapValue& shapValue) {
                   return shapValue.Feature == element.Feature;
               }
           );
           double coefficient = weightSum * (element.OnePathsFraction - element.ZeroPathsFraction);
           if (sameFeatureShapValue == shapValuesInternal->end()) {
                shapValuesInternal->emplace_back(element.Feature, approxDimension);
                for (int dimension = 0; dimension < approxDimension; ++dimension) {
                    double value = coefficient * firstLeafPtr[nodeIdx * approxDimension + dimension];
                    shapValuesInternal->back().Value[dimension] = value;

                }
            } else {
                for (int dimension = 0; dimension < approxDimension; ++dimension) {
                    double addValue = coefficient * firstLeafPtr[nodeIdx * approxDimension + dimension];
                    sameFeatureShapValue->Value[dimension] += addValue;
                }
            }
        }
    } else {
        double newZeroPathsFraction = 1.0;
        double newOnePathsFraction = 1.0;

        const size_t remainingDepth = forest.TreeSizes[treeIdx] - depth - 1;
        const int combinationClass = binFeatureCombinationClass[
            forest.TreeSplits[forest.TreeStartOffsets[treeIdx] + remainingDepth]
        ];

        const auto sameFeatureElement = FindIf(
            featurePath.begin(),
            featurePath.end(),
            [combinationClass](const TFeaturePathElement& element) {
                return element.Feature == combinationClass;
            }
        );

        if (sameFeatureElement != featurePath.end()) {
            const size_t sameFeatureIndex = sameFeatureElement - featurePath.begin();
            newZeroPathsFraction = featurePath[sameFeatureIndex].ZeroPathsFraction;
            newOnePathsFraction = featurePath[sameFeatureIndex].OnePathsFraction;
            featurePath = UnwindFeaturePath(featurePath, sameFeatureIndex);
        }

        const bool isGoRight = (documentLeafIdx >> remainingDepth) & 1;
        const size_t goNodeIdx = nodeIdx * 2 + isGoRight;
        const size_t skipNodeIdx = nodeIdx * 2 + !isGoRight;

        if (!FuzzyEquals(1 + subtreeWeights[depth + 1][goNodeIdx], 1 + 0.0)) {
            double newZeroPathsFractionGoNode = newZeroPathsFraction * subtreeWeights[depth + 1][goNodeIdx]
                / subtreeWeights[depth][nodeIdx];
            CalcInternalShapValuesForLeafRecursive(
                forest,
                binFeatureCombinationClass,
                documentLeafIdx,
                treeIdx,
                depth + 1,
                subtreeWeights,
                goNodeIdx,
                featurePath,
                newZeroPathsFractionGoNode,
                newOnePathsFraction,
                combinationClass,
                calcInternalValues,
                shapValuesInternal
            );
        }

        if (!FuzzyEquals(1 + subtreeWeights[depth + 1][skipNodeIdx], 1 + 0.0)) {
            double newZeroPathsFractionSkipNode = newZeroPathsFraction * subtreeWeights[depth + 1][skipNodeIdx]
                / subtreeWeights[depth][nodeIdx];
            CalcInternalShapValuesForLeafRecursive(
                forest,
                binFeatureCombinationClass,
                documentLeafIdx,
                treeIdx,
                depth + 1,
                subtreeWeights,
                skipNodeIdx,
                featurePath,
                newZeroPathsFractionSkipNode,
                /*onePathFraction*/ 0,
                combinationClass,
                calcInternalValues,
                shapValuesInternal
            );
        }
    }
}

static void UnpackInternalShaps(const TVector<TShapValue>& shapValuesInternal, const TVector<TVector<int>>& combinationClassFeatures,  TVector<TShapValue>* shapValues) {
    shapValues->clear();
    if (shapValuesInternal.empty()) {
        return;
    }
    const int approxDimension = shapValuesInternal[0].Value.ysize();
    for (const auto & shapValueInternal: shapValuesInternal) {
        const TVector<int> &flatFeatures = combinationClassFeatures[shapValueInternal.Feature];

        for (int flatFeatureIdx : flatFeatures) {
            const auto sameFeatureShapValue = FindIf(
                shapValues->begin(),
                shapValues->end(),
                [flatFeatureIdx](const TShapValue &shapValue) {
                    return shapValue.Feature == flatFeatureIdx;
                }
            );
            double coefficient = flatFeatures.size();
            if (sameFeatureShapValue == shapValues->end()) {
                shapValues->emplace_back(flatFeatureIdx, approxDimension);
                for (int dimension = 0; dimension < approxDimension; ++dimension) {
                    double value = shapValueInternal.Value[dimension] / coefficient;
                    shapValues->back().Value[dimension] = value;
                }
            } else {
                for (int dimension = 0; dimension < approxDimension; ++dimension) {
                    double addValue = shapValueInternal.Value[dimension] / coefficient;
                    sameFeatureShapValue->Value[dimension] += addValue;
                }
            }
        }
    }
}

static inline void CalcShapValuesForLeaf(
    const TObliviousTrees& forest,
    const TVector<int>& binFeatureCombinationClass,
    const TVector<TVector<int>>& combinationClassFeatures,
    size_t documentLeafIdx,
    size_t treeIdx,
    const TVector<TVector<double>>& subtreeWeights,
    bool calcInternalValues,
    TVector<TShapValue>* shapValues
) {
    shapValues->clear();

    TVector<TFeaturePathElement> initialFeaturePath;
    if (calcInternalValues) {
        CalcInternalShapValuesForLeafRecursive(
            forest,
            binFeatureCombinationClass,
            documentLeafIdx,
            treeIdx,
            /*depth*/ 0,
            subtreeWeights,
            /*nodeIdx*/ 0,
            initialFeaturePath,
            /*zeroPathFraction*/ 1,
            /*onePathFraction*/ 1,
            /*feature*/ -1,
            calcInternalValues,
            shapValues
        );
    } else {
        TVector<TShapValue> shapValuesInternal;
        CalcInternalShapValuesForLeafRecursive(
            forest,
            binFeatureCombinationClass,
            documentLeafIdx,
            treeIdx,
            /*depth*/ 0,
            subtreeWeights,
            /*nodeIdx*/ 0,
            initialFeaturePath,
            /*zeroPathFraction*/ 1,
            /*onePathFraction*/ 1,
            /*feature*/ -1,
            calcInternalValues,
            &shapValuesInternal
        );
        UnpackInternalShaps(shapValuesInternal, combinationClassFeatures, shapValues);
    }
}

static TVector<double> CalcMeanValueForTree(
    const TObliviousTrees& forest,
    const TVector<TVector<double>>& subtreeWeights,
    size_t treeIdx
) {
    const int approxDimension = forest.ApproxDimension;
    TVector<double> meanValue(approxDimension, 0.0);
    auto firstLeafPtr = forest.GetFirstLeafPtrForTree(treeIdx);
    const size_t maxDepth = forest.TreeSizes[treeIdx];

    for (size_t leafIdx = 0; leafIdx < (size_t(1) << maxDepth); ++leafIdx) {
        for (int dimension = 0; dimension < approxDimension; ++dimension) {
            meanValue[dimension] += firstLeafPtr[leafIdx * approxDimension + dimension]
                                  * subtreeWeights[maxDepth][leafIdx];
        }
    }

    for (int dimension = 0; dimension < approxDimension; ++dimension) {
        meanValue[dimension] /= subtreeWeights[0][0];
    }

    return meanValue;
}

static TVector<TVector<double>> CalcSubtreeWeightsForTree(const TVector<double>& leafWeights, int treeDepth) {
    TVector<TVector<double>> subtreeWeights(treeDepth + 1);

    subtreeWeights[treeDepth] = leafWeights;

    for (int depth = treeDepth - 1; depth >= 0; --depth) {
        const size_t nodeCount = size_t(1) << depth;
        subtreeWeights[depth].resize(nodeCount);
        for (size_t nodeIdx = 0; nodeIdx < nodeCount; ++nodeIdx) {
            subtreeWeights[depth][nodeIdx] = subtreeWeights[depth + 1][nodeIdx * 2] + subtreeWeights[depth + 1][nodeIdx * 2 + 1];
        }
    }
    return subtreeWeights;
}

static void MapBinFeaturesToClasses(
    const TObliviousTrees& forest,
    TVector<int>* binFeatureCombinationClass,
    TVector<TVector<int>>* combinationClassFeatures
) {
    const NCB::TFeaturesLayout layout(forest.FloatFeatures, forest.CatFeatures);
    TVector<TVector<int>> featuresCombinations;
    TVector<size_t> featureBucketSizes;

    for (const TFloatFeature& floatFeature : forest.FloatFeatures) {
        if (!floatFeature.UsedInModel()) {
            continue;
        }
        featuresCombinations.emplace_back();
        featuresCombinations.back() = { floatFeature.Position.FlatIndex };
        featureBucketSizes.push_back(floatFeature.Borders.size());
    }

    for (const TOneHotFeature& oneHotFeature: forest.OneHotFeatures) {
        featuresCombinations.emplace_back();
        featuresCombinations.back() = {
            (int)layout.GetExternalFeatureIdx(oneHotFeature.CatFeatureIndex,
            EFeatureType::Categorical)
        };
        featureBucketSizes.push_back(oneHotFeature.Values.size());
    }

    for (const TCtrFeature& ctrFeature : forest.CtrFeatures) {
        const TFeatureCombination& combination = ctrFeature.Ctr.Base.Projection;
        featuresCombinations.emplace_back();
        for (int catFeatureIdx : combination.CatFeatures) {
            featuresCombinations.back().push_back(
                layout.GetExternalFeatureIdx(catFeatureIdx, EFeatureType::Categorical));
        }
        featureBucketSizes.push_back(ctrFeature.Borders.size());
    }
    TVector<size_t> featureFirstBinBucket(featureBucketSizes.size(), 0);
    for (size_t i = 1; i < featureBucketSizes.size(); ++i) {
        featureFirstBinBucket[i] = featureFirstBinBucket[i - 1] + featureBucketSizes[i - 1];
    }
    TVector<int> sortedBinFeatures(featuresCombinations.size());
    Iota(sortedBinFeatures.begin(), sortedBinFeatures.end(), 0);
    Sort(
        sortedBinFeatures.begin(),
        sortedBinFeatures.end(),
        [featuresCombinations](int feature1, int feature2) {
            return featuresCombinations[feature1] < featuresCombinations[feature2];
        }
    );

    *binFeatureCombinationClass = TVector<int>(forest.GetBinaryFeaturesFullCount());
    *combinationClassFeatures = TVector<TVector<int>>();

    int equivalenceClassesCount = 0;
    for (ui32 featureIdx = 0; featureIdx < featuresCombinations.size(); ++featureIdx) {
        int currentFeature = sortedBinFeatures[featureIdx];
        int previousFeature = featureIdx == 0 ? -1 : sortedBinFeatures[featureIdx - 1];
        if (featureIdx == 0 || featuresCombinations[currentFeature] != featuresCombinations[previousFeature]) {
            combinationClassFeatures->push_back(featuresCombinations[currentFeature]);
            ++equivalenceClassesCount;
        }
        for (size_t binBucketId = featureFirstBinBucket[currentFeature];
             binBucketId < featureFirstBinBucket[currentFeature] + featureBucketSizes[currentFeature];
             ++binBucketId)
        {
            (*binFeatureCombinationClass)[binBucketId] = equivalenceClassesCount - 1;
        }
    }
}

void CalcShapValuesForDocumentMulti(
    const TFullModel& model,
    const TShapPreparedTrees& preparedTrees,
    const NCB::NModelEvaluation::IQuantizedData* binarizedFeaturesForBlock,
    int flatFeatureCount,
    size_t documentIdx,
    TVector<TVector<double>>* shapValues
) {
    const int approxDimension = model.GetDimensionsCount();
    shapValues->assign(approxDimension, TVector<double>(flatFeatureCount + 1, 0.0));
    const size_t treeCount = model.GetTreeCount();
    for (size_t treeIdx = 0; treeIdx < treeCount; ++treeIdx) {
        size_t leafIdx = CalcLeafToFallForDocument(
            model.GetCurrentEvaluator().Get(),
            treeIdx,
            binarizedFeaturesForBlock,
            documentIdx
        );
        if (preparedTrees.CalcShapValuesByLeafForAllTrees) {
            for (const TShapValue& shapValue : preparedTrees.ShapValuesByLeafForAllTrees[treeIdx][leafIdx]) {
                for (int dimension = 0; dimension < approxDimension; ++dimension) {
                    (*shapValues)[dimension][shapValue.Feature] += shapValue.Value[dimension];
                }
            }
        } else {
            TVector<TShapValue> shapValuesByLeaf;

            CalcShapValuesForLeaf(
                *model.ObliviousTrees.Get(),
                preparedTrees.BinFeatureCombinationClass,
                preparedTrees.CombinationClassFeatures,
                leafIdx,
                treeIdx,
                preparedTrees.SubtreeWeightsForAllTrees[treeIdx],//subtreeWeights,
                preparedTrees.CalcInternalValues,
                &shapValuesByLeaf
            );

            for (const TShapValue& shapValue : shapValuesByLeaf) {
                for (int dimension = 0; dimension < approxDimension; ++dimension) {
                    (*shapValues)[dimension][shapValue.Feature] += shapValue.Value[dimension];
                }
            }
        }

        for (int dimension = 0; dimension < approxDimension; ++dimension) {
            (*shapValues)[dimension][flatFeatureCount] +=
                    preparedTrees.MeanValuesForAllTrees[treeIdx][dimension];
        }
    }
}

static void CalcShapValuesForDocumentBlockMulti(
    const TFullModel& model,
    const IFeaturesBlockIterator& featuresBlockIterator,
    int flatFeatureCount,
    const TShapPreparedTrees& preparedTrees,
    size_t start,
    size_t end,
    NPar::TLocalExecutor* localExecutor,
    TVector<TVector<TVector<double>>>* shapValuesForAllDocuments
) {
    const size_t documentCount = end - start;

    auto binarizedFeaturesForBlock = MakeQuantizedFeaturesForEvaluator(model, featuresBlockIterator, start, end);

    const int oldShapValuesSize = shapValuesForAllDocuments->size();
    shapValuesForAllDocuments->resize(oldShapValuesSize + end - start);

    NPar::TLocalExecutor::TExecRangeParams blockParams(0, documentCount);
    localExecutor->ExecRange([&] (size_t documentIdx) {
        TVector<TVector<double>>& shapValues = (*shapValuesForAllDocuments)[oldShapValuesSize + documentIdx];

        CalcShapValuesForDocumentMulti(
            model,
            preparedTrees,
            binarizedFeaturesForBlock.Get(),
            flatFeatureCount,
            documentIdx,
            &shapValues
        );

    }, blockParams, NPar::TLocalExecutor::WAIT_COMPLETE);
}

static void CalcShapValuesByLeafForTreeBlock(
    const TObliviousTrees& forest,
    const TVector<TVector<double>>& leafWeights,
    int start,
    int end,
    bool calcInternalValues,
    NPar::TLocalExecutor* localExecutor,
    TShapPreparedTrees* preparedTrees
) {
    TVector<int> binFeatureCombinationClass = preparedTrees->BinFeatureCombinationClass;
    TVector<TVector<int>> combinationClassFeatures = preparedTrees->CombinationClassFeatures;

    NPar::TLocalExecutor::TExecRangeParams blockParams(start, end);
    localExecutor->ExecRange([&] (size_t treeIdx) {
        const size_t leafCount = (size_t(1) << forest.TreeSizes[treeIdx]);
        TVector<TVector<TShapValue>>& shapValuesByLeaf = preparedTrees->ShapValuesByLeafForAllTrees[treeIdx];
        if (preparedTrees->CalcShapValuesByLeafForAllTrees) {
            shapValuesByLeaf.resize(leafCount);
        }

        TVector<TVector<double>> subtreeWeights
            = CalcSubtreeWeightsForTree(leafWeights[treeIdx], forest.TreeSizes[treeIdx]);

        if (preparedTrees->CalcShapValuesByLeafForAllTrees) {
            for (size_t leafIdx = 0; leafIdx < leafCount; ++leafIdx) {
                CalcShapValuesForLeaf(
                    forest,
                    binFeatureCombinationClass,
                    combinationClassFeatures,
                    leafIdx,
                    treeIdx,
                    subtreeWeights,
                    calcInternalValues,
                    &shapValuesByLeaf[leafIdx]
                );
            }
        } else {
            preparedTrees->SubtreeWeightsForAllTrees[treeIdx] = subtreeWeights;
        }

        preparedTrees->MeanValuesForAllTrees[treeIdx]
                = CalcMeanValueForTree(forest, subtreeWeights, treeIdx);
    }, blockParams, NPar::TLocalExecutor::WAIT_COMPLETE);
}

bool PrepareTreesCalcShapValues(
    const TFullModel& model,
    const TDataProvider* dataset,
    EPreCalcShapValues mode
) {
    switch (mode) {
        case EPreCalcShapValues::UsePreCalc:
            return true;
        case EPreCalcShapValues::NoPreCalc:
            return false;
        case EPreCalcShapValues::Auto:
            if (dataset==nullptr) {
                return true;
            } else {
                const size_t treeCount = model.GetTreeCount();
                double treesAverageLeafCount = 0;
                const TObliviousTrees& forest = *model.ObliviousTrees;
                for (size_t treeIdx = 0; treeIdx < treeCount; ++treeIdx) {
                    treesAverageLeafCount += (size_t(1) << forest.TreeSizes[treeIdx]);
                }
                treesAverageLeafCount /= treeCount;
                return treesAverageLeafCount < dataset->ObjectsGrouping->GetObjectCount();
            }
    }
}

TShapPreparedTrees PrepareTrees(
    const TFullModel& model,
    const TDataProvider* dataset, // can be nullptr if model has LeafWeights
    int logPeriod,
    EPreCalcShapValues mode,
    NPar::TLocalExecutor* localExecutor,
    bool calcInternalValues
) {
    const size_t treeCount = model.GetTreeCount();
    const size_t treeBlockSize = CB_THREAD_LIMIT; // least necessary for threading

    TImportanceLogger treesLogger(treeCount, "trees processed", "Processing trees...", logPeriod);

    // use only if model.ObliviousTrees->LeafWeights is empty
    TVector<TVector<double>> leafWeights;
    if (model.ObliviousTrees->LeafWeights.empty()) {
        CB_ENSURE(
                dataset,
                "PrepareTrees requires either non-empty LeafWeights in model or provided dataset"
        );
        CB_ENSURE(dataset->ObjectsGrouping->GetObjectCount() != 0, "no docs in pool");
        CB_ENSURE(dataset->MetaInfo.GetFeatureCount() > 0, "no features in pool");
        leafWeights = CollectLeavesStatistics(*dataset, model, localExecutor);
    }

    TShapPreparedTrees preparedTrees;
    preparedTrees.CalcShapValuesByLeafForAllTrees = PrepareTreesCalcShapValues(model, dataset, mode);

    if (!preparedTrees.CalcShapValuesByLeafForAllTrees) {
        preparedTrees.LeafWeightsForAllTrees
            = model.ObliviousTrees->LeafWeights.empty() ? leafWeights : model.ObliviousTrees->LeafWeights;
    }

    preparedTrees.ShapValuesByLeafForAllTrees.resize(treeCount);
    preparedTrees.SubtreeWeightsForAllTrees.resize(treeCount);
    preparedTrees.MeanValuesForAllTrees.resize(treeCount);
    preparedTrees.CalcInternalValues = calcInternalValues;

    const TObliviousTrees& forest = *model.ObliviousTrees;
    MapBinFeaturesToClasses(
        forest,
        &preparedTrees.BinFeatureCombinationClass,
        &preparedTrees.CombinationClassFeatures
    );

    TProfileInfo processTreesProfile(treeCount);

    for (size_t start = 0; start < treeCount; start += treeBlockSize) {
        size_t end = Min(start + treeBlockSize, treeCount);

        processTreesProfile.StartIterationBlock();

        CalcShapValuesByLeafForTreeBlock(
            *model.ObliviousTrees,
            model.ObliviousTrees->LeafWeights.empty() ? leafWeights : model.ObliviousTrees->LeafWeights,
            start,
            end,
            calcInternalValues,
            localExecutor,
            &preparedTrees
        );

        processTreesProfile.FinishIterationBlock(end - start);
        auto profileResults = processTreesProfile.GetProfileResults();
        treesLogger.Log(profileResults);
    }

    return preparedTrees;
}

TShapPreparedTrees PrepareTrees(
    const TFullModel& model,
    NPar::TLocalExecutor* localExecutor
) {
    CB_ENSURE(
        !model.ObliviousTrees->LeafWeights.empty(),
        "Model must have leaf weights or sample pool must be provided"
    );
    return PrepareTrees(model, nullptr, 0, EPreCalcShapValues::Auto, localExecutor);
}

void CalcShapValuesInternalForFeature(
    const TShapPreparedTrees& preparedTrees,
    const TFullModel& model,
    int /*logPeriod*/,
    ui32 start,
    ui32 end,
    ui32 featuresCount,
    const NCB::TObjectsDataProvider& objectsData,
    TVector<TVector<TVector<double>>>* shapValues, // [docIdx][featureIdx][dim]
    NPar::TLocalExecutor* localExecutor
) {
    CB_ENSURE(start <= end && end <= objectsData.GetObjectCount());
    const TObliviousTrees& forest = *model.ObliviousTrees;
    shapValues->clear();
    const ui32 documentCount = end - start;
    shapValues->resize(documentCount);

    auto binarizedFeaturesForBlock = MakeQuantizedFeaturesForEvaluator(model, objectsData, start, end);
    const ui32 documentBlockSize = CB_THREAD_LIMIT;
    for (ui32 startIdx = 0; startIdx < documentCount; startIdx += documentBlockSize) {
        NPar::TLocalExecutor::TExecRangeParams blockParams(startIdx, startIdx + Min(documentBlockSize, documentCount - startIdx));
        localExecutor->ExecRange([&](ui32 documentIdx) {
            TVector<TVector<double>> &docShapValues = (*shapValues)[documentIdx];
            docShapValues.assign(featuresCount, TVector<double>(forest.ApproxDimension + 1, 0.0));
            for (size_t treeIdx = 0; treeIdx < forest.GetTreeCount(); ++treeIdx) {
                size_t leafIdx = CalcLeafToFallForDocument(
                    model.GetCurrentEvaluator().Get(),
                    treeIdx,
                    binarizedFeaturesForBlock.Get(),
                    documentIdx
                );

                if (preparedTrees.CalcShapValuesByLeafForAllTrees) {
                    for (const TShapValue& shapValue : preparedTrees.ShapValuesByLeafForAllTrees[treeIdx][leafIdx]) {
                        for (int dimension = 0; dimension < forest.ApproxDimension; ++dimension) {
                            docShapValues[shapValue.Feature][dimension] += shapValue.Value[dimension];
                        }
                    }
                } else {
                    TVector<TShapValue> shapValuesByLeaf;

                    CalcShapValuesForLeaf(
                        forest,
                        preparedTrees.BinFeatureCombinationClass,
                        preparedTrees.CombinationClassFeatures,
                        leafIdx,
                        treeIdx,
                        preparedTrees.SubtreeWeightsForAllTrees[treeIdx],
                        preparedTrees.CalcInternalValues,
                        &shapValuesByLeaf
                    );

                    for (const TShapValue& shapValue : shapValuesByLeaf) {
                        for (int dimension = 0; dimension < forest.ApproxDimension; ++dimension) {
                            docShapValues[shapValue.Feature][dimension] += shapValue.Value[dimension];
                        }
                    }
                }
            }
        }, blockParams, NPar::TLocalExecutor::WAIT_COMPLETE);
    }
}

TVector<TVector<TVector<double>>> CalcShapValuesMulti(
    const TFullModel& model,
    const TDataProvider& dataset,
    int logPeriod,
    EPreCalcShapValues mode,
    NPar::TLocalExecutor* localExecutor
) {
    TShapPreparedTrees preparedTrees = PrepareTrees(
        model,
        &dataset,
        logPeriod,
        mode,
        localExecutor,
        /*calcInternalValues=*/false
    );

    const size_t documentCount = dataset.ObjectsGrouping->GetObjectCount();
    const size_t documentBlockSize = CB_THREAD_LIMIT; // least necessary for threading

    const int flatFeatureCount = SafeIntegerCast<int>(dataset.MetaInfo.GetFeatureCount());

    TImportanceLogger documentsLogger(documentCount, "documents processed", "Processing documents...", logPeriod);

    TVector<TVector<TVector<double>>> shapValues;
    shapValues.reserve(documentCount);

    TProfileInfo processDocumentsProfile(documentCount);

    THolder<IFeaturesBlockIterator> featuresBlockIterator
        = CreateFeaturesBlockIterator(model, *dataset.ObjectsData, 0, documentCount);

    for (size_t start = 0; start < documentCount; start += documentBlockSize) {
        size_t end = Min(start + documentBlockSize, documentCount);

        processDocumentsProfile.StartIterationBlock();

        featuresBlockIterator->NextBlock(end - start);

        CalcShapValuesForDocumentBlockMulti(
            model,
            *featuresBlockIterator,
            flatFeatureCount,
            preparedTrees,
            start,
            end,
            localExecutor,
            &shapValues
        );

        processDocumentsProfile.FinishIterationBlock(end - start);
        auto profileResults = processDocumentsProfile.GetProfileResults();
        documentsLogger.Log(profileResults);
    }

    return shapValues;
}

TVector<TVector<double>> CalcShapValues(
    const TFullModel& model,
    const TDataProvider& dataset,
    int logPeriod,
    EPreCalcShapValues mode,
    NPar::TLocalExecutor* localExecutor
) {
    CB_ENSURE(model.ObliviousTrees->ApproxDimension == 1, "Model must not be trained for multiclassification.");
    TVector<TVector<TVector<double>>> shapValuesMulti = CalcShapValuesMulti(
        model,
        dataset,
        logPeriod,
        mode,
        localExecutor
    );

    size_t documentsCount = dataset.ObjectsGrouping->GetObjectCount();
    TVector<TVector<double>> shapValues(documentsCount);

    for (size_t documentIdx = 0; documentIdx < documentsCount; ++documentIdx) {
        shapValues[documentIdx] = std::move(shapValuesMulti[documentIdx][0]);
    }
    return shapValues;
}

static void OutputShapValuesMulti(const TVector<TVector<TVector<double>>>& shapValues, TFileOutput& out) {
    for (const auto& shapValuesForDocument : shapValues) {
        for (const auto& shapValuesForClass : shapValuesForDocument) {
            int valuesCount = shapValuesForClass.size();
            for (int valueIdx = 0; valueIdx < valuesCount; ++valueIdx) {
                out << shapValuesForClass[valueIdx] << (valueIdx + 1 == valuesCount ? '\n' : '\t');
            }
        }
    }
}

void CalcAndOutputShapValues(
    const TFullModel& model,
    const TDataProvider& dataset,
    const TString& outputPath,
    int logPeriod,
    EPreCalcShapValues mode,
    NPar::TLocalExecutor* localExecutor
) {
    TShapPreparedTrees preparedTrees = PrepareTrees(
        model,
        &dataset,
        logPeriod,
        mode,
        localExecutor,
        /*calcInternalValues=*/false
    );

    const int flatFeatureCount = SafeIntegerCast<int>(dataset.MetaInfo.GetFeatureCount());

    const size_t documentCount = dataset.ObjectsGrouping->GetObjectCount();
    const size_t documentBlockSize = CB_THREAD_LIMIT; // least necessary for threading

    TImportanceLogger documentsLogger(documentCount, "documents processed", "Processing documents...", logPeriod);

    TProfileInfo processDocumentsProfile(documentCount);

    THolder<IFeaturesBlockIterator> featuresBlockIterator
        = CreateFeaturesBlockIterator(model, *dataset.ObjectsData, 0, documentCount);

    TFileOutput out(outputPath);
    for (size_t start = 0; start < documentCount; start += documentBlockSize) {
        size_t end = Min(start + documentBlockSize, documentCount);
        processDocumentsProfile.StartIterationBlock();

        TVector<TVector<TVector<double>>> shapValuesForBlock;
        shapValuesForBlock.reserve(end - start);

        featuresBlockIterator->NextBlock(end - start);

        CalcShapValuesForDocumentBlockMulti(
            model,
            *featuresBlockIterator,
            flatFeatureCount,
            preparedTrees,
            start,
            end,
            localExecutor,
            &shapValuesForBlock
        );

        OutputShapValuesMulti(shapValuesForBlock, out);

        processDocumentsProfile.FinishIterationBlock(end - start);
        auto profileResults = processDocumentsProfile.GetProfileResults();
        documentsLogger.Log(profileResults);
    }
}
