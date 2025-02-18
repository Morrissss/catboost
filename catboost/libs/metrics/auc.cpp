#include "auc.h"

#include <catboost/libs/helpers/parallel_sort/parallel_sort.h>

#include <util/generic/algorithm.h>
#include <util/generic/array_ref.h>
#include <util/generic/vector.h>

using NMetrics::TSample;
using NCB::TMergeData;

static double MergeAndCountInversions(
    const TMergeData& mergeOperation,
    const TVector<TSample>& input,
    TVector<TSample>* aux
) {
    double result = 0;
    ui32 leftIndex = mergeOperation.Left1;
    ui32 rightIndex = mergeOperation.Left2;
    ui32 outputIndex = mergeOperation.OutputIndex;
    auto& output = *aux;
    double accumulatedWeight = 0;
    while (leftIndex < mergeOperation.Right1 && rightIndex < mergeOperation.Right2) {
        if (input[rightIndex].Target < input[leftIndex].Target) {
            accumulatedWeight += input[rightIndex].Weight;
            output[outputIndex] = input[rightIndex];
            ++outputIndex;
            ++rightIndex;
        } else {
            result += input[leftIndex].Weight * accumulatedWeight;
            output[outputIndex] = input[leftIndex];
            ++outputIndex;
            ++leftIndex;
        }
    }
    if (leftIndex < mergeOperation.Right1) {
        for (ui32 i = leftIndex; i < mergeOperation.Right1; ++i) {
            result += input[i].Weight * accumulatedWeight;
        }
        std::copy(input.begin() + leftIndex, input.begin() + mergeOperation.Right1, output.begin() + outputIndex);
    }
    if (rightIndex < mergeOperation.Right2) {
        std::copy(input.begin() + rightIndex, input.begin() + mergeOperation.Right2, output.begin() + outputIndex);
    }
    return result;
}

static double SortAndCountInversions(
    ui32 lo,
    ui32 hi,
    TVector<TSample>* samples,
    TVector<TSample>* aux
) {
    if (lo + 1 >= hi) return 0;
    ui32 mid = lo + (hi - lo) / 2;
    auto leftCount = SortAndCountInversions(lo, mid, samples, aux);
    auto rightCount = SortAndCountInversions(mid, hi, samples, aux);
    auto mergeCount = MergeAndCountInversions({lo, mid, mid, hi, lo}, *samples, aux);
    std::copy(aux->begin() + lo, aux->begin() + hi, samples->begin() + lo);
    return leftCount + rightCount + mergeCount;
}

static bool CompareSamplesByPrediction(const TSample& left, const TSample& right) {
    return std::tie(left.Prediction, left.Target) < std::tie(right.Prediction, right.Target);
}

static bool CompareSamplesByTarget(const TSample& left, const TSample& right) {
    return left.Target < right.Target;
}

static double ParallelSortAndCountInversions(
    TVector<TSample>* samples,
    TVector<TSample>* aux,
    NPar::TLocalExecutor* localExecutor
) {
    if (samples->size() <= 1u) {
        return 0;
    }
    const ui32 threadCount = Min((ui32)localExecutor->GetThreadCount() + 1u, (ui32)samples->size());
    TVector<ui32> blockSizes;
    NCB::EquallyDivide(samples->size(), threadCount, &blockSizes);
    TVector<ui32> startPositions(threadCount);
    ui32 position = 0;
    for (ui32 i = 0; i < threadCount; ++i) {
        startPositions[i] = position;
        position += blockSizes[i];
    }
    TVector<double> threadResults(threadCount, 0);
    NPar::ParallelFor(
        *localExecutor,
        0,
        threadCount,
        [&](int blockId) {
            int left = startPositions[blockId];
            int right = left + blockSizes[blockId];
            threadResults[blockId] += SortAndCountInversions(left, right, samples, aux);
        }
    );
    double result = 0;
    while (blockSizes.size() > 1u) {
        const ui32 currentMergesCount = blockSizes.size() / 2u;
        TVector<ui32> threadsPerMergeCount;
        NCB::EquallyDivide(threadCount, currentMergesCount, &threadsPerMergeCount);
        TVector<TMergeData> mergeData;
        for (ui32 i = 0; i < currentMergesCount; ++i) {
            TMergeData currentMerge = {
                startPositions[2 * i],
                startPositions[2 * i + 1],
                startPositions[2 * i + 1],
                (2 * i + 2 == startPositions.size() ? static_cast<ui32>(samples->size()) : startPositions[2 * i + 2]),
                startPositions[2 * i]
            };
            DivideMergeIntoParallelMerges(currentMerge, CompareSamplesByTarget, *samples, &mergeData, &threadsPerMergeCount[i]);
        }
        TVector<double> leftWeightsSum(mergeData.size(), 0);
        TVector<double> rightWeightsSum(mergeData.size(), 0);
        NPar::ParallelFor(
            *localExecutor,
            0,
            mergeData.size(),
            [&](int blockId) {
                threadResults[blockId] += MergeAndCountInversions(mergeData[blockId], *samples, aux);
                for (ui32 i = mergeData[blockId].Left1; i < mergeData[blockId].Right1; ++i) {
                    leftWeightsSum[blockId] += (*samples)[i].Weight;
                }
                for (ui32 i = mergeData[blockId].Left2; i < mergeData[blockId].Right2; ++i) {
                    rightWeightsSum[blockId] += (*samples)[i].Weight;
                }
            }
        );
        ui32 position = 0;
        for (ui32 i = 0; i < currentMergesCount; ++i) {
            for (ui32 j = position; j + 1 < position + threadsPerMergeCount[i]; ++j) {
                rightWeightsSum[j + 1] += rightWeightsSum[j];
                result += rightWeightsSum[j] * leftWeightsSum[j + 1];
            }
            position += threadsPerMergeCount[i];
        }
        NPar::ParallelFor(
            *localExecutor,
            0,
            mergeData.size(),
            [&](int blockId) {
                int startPosition = mergeData[blockId].OutputIndex;
                int endPosition = startPosition + mergeData[blockId].GetSize();
                std::copy(aux->begin() + startPosition, aux->begin() + endPosition, samples->begin() + startPosition);
            }
        );
        const ui32 newSize = (blockSizes.size() + 1) / 2u;
        TVector<ui32> newBlockSizes, newStartPositions;
        newBlockSizes.reserve(newSize);
        newStartPositions.reserve(newSize);
        for (ui32 i = 0; i + 1 < blockSizes.size(); i += 2) {
            newBlockSizes.emplace_back(blockSizes[i] + blockSizes[i + 1]);
            newStartPositions.emplace_back(startPositions[i]);
        }
        if (2 * newSize != blockSizes.size()) {
            newBlockSizes.emplace_back(blockSizes.back());
            newStartPositions.emplace_back(startPositions.back());
        }
        blockSizes = newBlockSizes;
        startPositions = newStartPositions;
    }
    for (const double& threadResult : threadResults) {
        result += threadResult;
    }
    return result;
}

double CalcAUC(TVector<TSample>* samples, NPar::TLocalExecutor* localExecutor, double* outWeightSum, double* outPairWeightSum) {
    TVector<TSample> aux(samples->begin(), samples->end());

    NCB::ParallelMergeSort(CompareSamplesByPrediction, samples, localExecutor, &aux);

    double deltaSum = 0;
    double accumulatedEqualPredictionsWeight = 0;
    double accumulatedEqualPairsWeight = 0;
    double equalPredictionsPairWeightSum = 0;
    double equalPairsPairWeightSum = 0;

    for (size_t i = 0; i < samples->size(); ++i) {
        equalPredictionsPairWeightSum += accumulatedEqualPredictionsWeight * (*samples)[i].Weight;
        equalPairsPairWeightSum += accumulatedEqualPairsWeight * (*samples)[i].Weight;
        accumulatedEqualPredictionsWeight += (*samples)[i].Weight;
        accumulatedEqualPairsWeight += (*samples)[i].Weight;
        if (i + 1 == samples->size() || (*samples)[i].Prediction != (*samples)[i + 1].Prediction) {
            deltaSum += equalPredictionsPairWeightSum;
            deltaSum -= equalPairsPairWeightSum;
            equalPredictionsPairWeightSum = 0;
            accumulatedEqualPredictionsWeight = 0;
            equalPairsPairWeightSum = 0;
            accumulatedEqualPairsWeight = 0;
        } else if ((*samples)[i].Target != (*samples)[i + 1].Target) {
            deltaSum -= equalPairsPairWeightSum;
            equalPairsPairWeightSum = 0;
            accumulatedEqualPairsWeight = 0;
        }
    }

    auto optimisticAUC = ParallelSortAndCountInversions(samples, &aux, localExecutor);

    double weightSum = 0;
    double pairWeightSum = 0;
    double accumulatedWeight = 0;
    for (ui32 i = 0; i < samples->size(); ++i) {
        auto& sample = (*samples)[i];
        if (i > 0 && (*samples)[i - 1].Target != sample.Target) {
            accumulatedWeight = weightSum;
        }
        weightSum += sample.Weight;
        pairWeightSum += accumulatedWeight * sample.Weight;
    }
    if (outWeightSum != nullptr) {
        *outWeightSum = weightSum;
    }
    if (outPairWeightSum != nullptr) {
        *outPairWeightSum = pairWeightSum;
    }
    if (pairWeightSum == 0) {
        return 0;
    }

    return 1 - ((2 * optimisticAUC + deltaSum) / (2.0 * pairWeightSum));
}

double CalcAUC(TVector<TSample>* samples, double* outWeightSum, double* outPairWeightSum, int threadCount) {
    NPar::TLocalExecutor localExecutor;
    localExecutor.RunAdditionalThreads(threadCount - 1);
    return CalcAUC(samples, &localExecutor, outWeightSum, outPairWeightSum);
}
