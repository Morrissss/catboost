#include "scoring.h"

#include "calc_score_cache.h"
#include "fold.h"
#include "index_calcer.h"

#include <catboost/libs/algo_helpers/online_predictor.h>
#include "pairwise_scoring.h"
#include "split.h"
#include "monotonic_constraint_utils.h"
#include "tensor_search_helpers.h"

#include <catboost/libs/data_new/objects.h>
#include <catboost/libs/data_types/pair.h>
#include <catboost/libs/helpers/map_merge.h>
#include <catboost/libs/index_range/index_range.h>
#include <catboost/libs/options/catboost_options.h>

#include <library/threading/local_executor/local_executor.h>
#include <library/dot_product/dot_product.h>

#include <util/generic/array_ref.h>

#include <type_traits>


using namespace NCB;


namespace {

    // Statistics (sums for score calculation) are stored in an array.
    // This class helps navigating in this array.
    struct TStatsIndexer {
    public:
        const int BucketCount;

    public:
        explicit TStatsIndexer(int bucketCount)
            : BucketCount(bucketCount) {
        }

        int CalcSize(int depth) const {
            return (1U << depth) * BucketCount;
        }

        int GetIndex(int leafIndex, int bucketIndex) const {
            return BucketCount * leafIndex + bucketIndex;
        }
    };


    template <class T>
    class TDataRefOptionalHolder {
    public:
        TDataRefOptionalHolder() = default;

        // Buf not used, init from external data
        explicit TDataRefOptionalHolder(TArrayRef<T> extData)
            : Data(extData)
        {}

        // noninitializing
        explicit TDataRefOptionalHolder(size_t size)
        {
            Buf.yresize(size);
            Data = TArrayRef<T>(Buf);
        }

        bool NonInited() const {
            return Data.data() == nullptr;
        }

        TArrayRef<T> GetData() {
            return Data;
        }

        TConstArrayRef<T> GetData() const {
            return Data;
        }

    private:
        TArrayRef<T> Data;
        TVector<T> Buf;
    };

    using TBucketStatsRefOptionalHolder = TDataRefOptionalHolder<TBucketStats>;
}


/* A helper function that returns calculated ctr values for this projection
   (== feature or feature combination) from cache.
*/
inline static const TOnlineCTR& GetCtr(
    const std::tuple<const TOnlineCTRHash&,
    const TOnlineCTRHash&>& allCtrs,
    const TProjection& proj
) {
    static const constexpr size_t OnlineSingleCtrsIndex = 0;
    static const constexpr size_t OnlineCTRIndex = 1;
    return proj.HasSingleFeature() ?
          std::get<OnlineSingleCtrsIndex>(allCtrs).at(proj)
        : std::get<OnlineCTRIndex>(allCtrs).at(proj);
}


// Helper function for calculating index of leaf for each document given a new split.
// Calculates indices when a permutation is given.
template <typename TBucketIndexType, typename TFullIndexType>
inline static void SetSingleIndex(
    const TCalcScoreFold& fold,
    const TStatsIndexer& indexer,
    TBucketIndexType* bucketIndex,
    const ui32* bucketIndexing, // can be nullptr for simple case, use bucketBeginOffset instead then
    const int bucketBeginOffset,
    const int permBlockSize,
    NCB::TIndexRange<int> docIndexRange, // aligned by permutation blocks in docPermutation
    TVector<TFullIndexType>* singleIdx // already of proper size
) {
    const int docCount = fold.GetDocCount();
    const TIndexType* indices = GetDataPtr(fold.Indices);
    const TArrayRef<TFullIndexType> singleIdxRef(*singleIdx);

    if (bucketIndexing == nullptr) {
        for (int doc : docIndexRange.Iter()) {
            singleIdxRef[doc] = indexer.GetIndex(indices[doc], bucketIndex[bucketBeginOffset + doc]);
        }
    } else if (permBlockSize > 1) {
        const int blockCount = (docCount + permBlockSize - 1) / permBlockSize;
        Y_ASSERT(
            (static_cast<int>(bucketIndexing[0]) / permBlockSize + 1 == blockCount)
            || (static_cast<int>(bucketIndexing[0]) + permBlockSize - 1
            == static_cast<int>(bucketIndexing[permBlockSize - 1]))
        );
        int blockStart = docIndexRange.Begin;
        while (blockStart < docIndexRange.End) {
            const int blockIdx = static_cast<int>(bucketIndexing[blockStart]) / permBlockSize;
            const int nextBlockStart = Min(
                blockStart + (blockIdx + 1 == blockCount ? docCount - blockIdx * permBlockSize : permBlockSize),
                docIndexRange.End
            );
            const int originalBlockIdx = static_cast<int>(bucketIndexing[blockStart]);
            for (int doc = blockStart; doc < nextBlockStart; ++doc) {
                const int originalDocIdx = originalBlockIdx + doc - blockStart;
                singleIdxRef[doc] = indexer.GetIndex(indices[doc], bucketIndex[originalDocIdx]);
            }
            blockStart = nextBlockStart;
        }
    } else {
        for (int doc : docIndexRange.Iter()) {
            const ui32 originalDocIdx = bucketIndexing[doc];
            singleIdxRef[doc] = indexer.GetIndex(indices[doc], bucketIndex[originalDocIdx]);
        }
    }
}


template <class T, EFeatureValuesType FeatureValuesType, typename TFullIndexType>
inline static void BuildSingleIndex(
    const TCalcScoreFold& fold,
    const TTypedFeatureValuesHolder<T, FeatureValuesType>& column,
    const TStatsIndexer& indexer,
    NCB::TIndexRange<int> docIndexRange,
    TVector<TFullIndexType>* singleIdx // already of proper size
) {
    if (const auto* denseColumnData
            = dynamic_cast<const TCompressedValuesHolderImpl<T, FeatureValuesType>*>(&column))
    {
        const bool simpleIndexing = fold.NonCtrDataPermutationBlockSize == fold.GetDocCount();
        const ui32* docInDataProviderIndexing =
            simpleIndexing ?
            nullptr
            : fold.LearnPermutationFeaturesSubset.Get<TIndexedSubset<ui32>>().data();
        const int docInDataProviderBeginOffset = simpleIndexing ? fold.FeaturesSubsetBegin : 0;

        const TCompressedArray& compressedArray = *denseColumnData->GetCompressedData().GetSrc();

        DispatchBitsPerKeyToDataType(
            compressedArray,
            "BuildSingleIndex",
            [&] (const auto* histogram) {
                SetSingleIndex(
                    fold,
                    indexer,
                    histogram,
                    docInDataProviderIndexing,
                    docInDataProviderBeginOffset,
                    fold.NonCtrDataPermutationBlockSize,
                    docIndexRange,
                    singleIdx
                );
            }
        );
    } else {
        Y_FAIL("BuildSingleIndex: unexpected column type");
    }
}


// Calculate index of leaf for each document given a new split ensemble.
template <typename TFullIndexType>
inline static void BuildSingleIndex(
    const TCalcScoreFold& fold,
    const TQuantizedForCPUObjectsDataProvider& objectsDataProvider,
    const std::tuple<const TOnlineCTRHash&, const TOnlineCTRHash&>& allCtrs,
    const TSplitEnsemble& splitEnsemble,
    const TStatsIndexer& indexer,
    NCB::TIndexRange<int> docIndexRange,
    TVector<TFullIndexType>* singleIdx // already of proper size
) {
    if (splitEnsemble.IsSplitOfType(ESplitType::OnlineCtr)) {
        const TCtr& ctr = splitEnsemble.SplitCandidate.Ctr;
        const bool simpleIndexing = fold.CtrDataPermutationBlockSize == fold.GetDocCount();
        const ui32* docInFoldIndexing = simpleIndexing ? nullptr : GetDataPtr(fold.IndexInFold);
        SetSingleIndex(
            fold,
            indexer,
            GetCtr(allCtrs, ctr.Projection).Feature[ctr.CtrIdx][ctr.TargetBorderIdx][ctr.PriorIdx].data(),
            docInFoldIndexing,
            0,
            fold.CtrDataPermutationBlockSize,
            docIndexRange,
            singleIdx
        );
    } else {
        auto buildSingleIndexFunc = [&] (const auto& column) {
            BuildSingleIndex(
                fold,
                column,
                indexer,
                docIndexRange,
                singleIdx
            );
        };

        switch (splitEnsemble.Type) {
            case ESplitEnsembleType::OneFeature:
                {
                    const auto& splitCandidate = splitEnsemble.SplitCandidate;
                    if (splitCandidate.Type == ESplitType::FloatFeature) {
                        buildSingleIndexFunc(
                            **objectsDataProvider.GetNonPackedFloatFeature((ui32)splitCandidate.FeatureIdx)
                        );
                    } else {
                        Y_ASSERT(splitCandidate.Type == ESplitType::OneHotFeature);
                        buildSingleIndexFunc(
                           **objectsDataProvider.GetNonPackedCatFeature((ui32)splitCandidate.FeatureIdx)
                        );
                    }
                }
                break;
            case ESplitEnsembleType::BinarySplits:
                buildSingleIndexFunc(
                    objectsDataProvider.GetBinaryFeaturesPack(splitEnsemble.BinarySplitsPackRef.PackIdx)
                );
                break;
            case ESplitEnsembleType::ExclusiveBundle:
                buildSingleIndexFunc(
                    objectsDataProvider.GetExclusiveFeaturesBundle(
                        splitEnsemble.ExclusiveFeaturesBundleRef.BundleIdx
                    )
                );
        }
    }
}


// Update bootstraped sums on docIndexRange in a bucket
template <typename TFullIndexType>
inline static void UpdateWeighted(
    const TVector<TFullIndexType>& singleIdx,
    const double* weightedDer,
    const float* sampleWeights,
    NCB::TIndexRange<int> docIndexRange,
    TBucketStats* stats
) {
    for (int doc : docIndexRange.Iter()) {
        TBucketStats& leafStats = stats[singleIdx[doc]];
        leafStats.SumWeightedDelta += weightedDer[doc];
        leafStats.SumWeight += sampleWeights[doc];
    }
}


// Update not bootstraped sums on docIndexRange in a bucket
template <typename TFullIndexType>
inline static void UpdateDeltaCount(
    const TVector<TFullIndexType>& singleIdx,
    const double* derivatives,
    const float* learnWeights,
    NCB::TIndexRange<int> docIndexRange,
    TBucketStats* stats
) {
    if (learnWeights == nullptr) {
        for (int doc : docIndexRange.Iter()) {
            TBucketStats& leafStats = stats[singleIdx[doc]];
            leafStats.SumDelta += derivatives[doc];
            leafStats.Count += 1;
        }
    } else {
        for (int doc : docIndexRange.Iter()) {
            TBucketStats& leafStats = stats[singleIdx[doc]];
            leafStats.SumDelta += derivatives[doc];
            leafStats.Count += learnWeights[doc];
        }
    }
}


template <typename TFullIndexType>
inline static void CalcStatsKernel(
    bool isCaching,
    const TVector<TFullIndexType>& singleIdx,
    const TCalcScoreFold& fold,
    bool isPlainMode,
    const TStatsIndexer& indexer,
    int depth,
    const TCalcScoreFold::TBodyTail& bt,
    int dim,
    NCB::TIndexRange<int> docIndexRange,
    TBucketStats* stats
) {
    Y_ASSERT(!isCaching || depth > 0);
    if (isCaching) {
        Fill(
            stats + indexer.CalcSize(depth - 1),
            stats + indexer.CalcSize(depth),
            TBucketStats{0, 0, 0, 0}
        );
    } else {
        Fill(stats, stats + indexer.CalcSize(depth), TBucketStats{0, 0, 0, 0});
    }

    if (bt.TailFinish > docIndexRange.Begin) {
        const bool hasPairwiseWeights = !bt.PairwiseWeights.empty();
        const float* weightsData = hasPairwiseWeights ?
            GetDataPtr(bt.PairwiseWeights) : GetDataPtr(fold.LearnWeights);
        const float* sampleWeightsData = hasPairwiseWeights ?
            GetDataPtr(bt.SamplePairwiseWeights) : GetDataPtr(fold.SampleWeights);

        int tailFinishInRange = Min((int)bt.TailFinish, docIndexRange.End);

        if (isPlainMode) {
            UpdateWeighted(
                singleIdx,
                GetDataPtr(bt.SampleWeightedDerivatives[dim]),
                sampleWeightsData,
                NCB::TIndexRange<int>(docIndexRange.Begin, tailFinishInRange),
                stats
            );
        } else {
            if (bt.BodyFinish > docIndexRange.Begin) {
                UpdateDeltaCount(
                    singleIdx,
                    GetDataPtr(bt.WeightedDerivatives[dim]),
                    weightsData,
                    NCB::TIndexRange<int>(docIndexRange.Begin, Min((int)bt.BodyFinish, docIndexRange.End)),
                    stats
                );
            }
            if (tailFinishInRange > bt.BodyFinish) {
                UpdateWeighted(
                    singleIdx,
                    GetDataPtr(bt.SampleWeightedDerivatives[dim]),
                    sampleWeightsData,
                    NCB::TIndexRange<int>(Max((int)bt.BodyFinish, docIndexRange.Begin), tailFinishInRange),
                    stats
                );
            }
        }
    }
}

inline static void FixUpStats(
    int depth,
    const TStatsIndexer& indexer,
    bool selectedSplitValue,
    TBucketStats* stats
) {
    const int halfOfStats = indexer.CalcSize(depth - 1);
    if (selectedSplitValue == true) {
        for (int statIdx = 0; statIdx < halfOfStats; ++statIdx) {
            stats[statIdx].Remove(stats[statIdx + halfOfStats]);
        }
    } else {
        for (int statIdx = 0; statIdx < halfOfStats; ++statIdx) {
            stats[statIdx].Remove(stats[statIdx + halfOfStats]);
            DoSwap(stats[statIdx], stats[statIdx + halfOfStats]);
        }
    }
}


template <typename TFullIndexType, typename TIsCaching>
static void CalcStatsImpl(
    const TCalcScoreFold& fold,
    const TQuantizedForCPUObjectsDataProvider& objectsDataProvider,
    const TFlatPairsInfo& pairs,
    const std::tuple<const TOnlineCTRHash&, const TOnlineCTRHash&>& allCtrs,
    const TSplitEnsemble& splitEnsemble,
    const TStatsIndexer& indexer,
    const TIsCaching& /*isCaching*/,
    bool /*isPlainMode*/,
    ui32 oneHotMaxSize,
    int depth,
    int /*splitStatsCount*/,
    NPar::TLocalExecutor* localExecutor,
    TPairwiseStats* stats
) {
    const int approxDimension = fold.GetApproxDimension();
    const int leafCount = 1 << depth;

    Y_ASSERT(approxDimension == 1 && fold.GetBodyTailCount() == 1);

    const int docCount = fold.GetDocCount();
    auto weightedDerivativesData = MakeArrayRef(
        fold.BodyTailArr[0].WeightedDerivatives[0].data(),
        docCount
    );
    const auto blockCount = fold.GetCalcStatsIndexRanges().RangesCount();
    const auto docPart = CeilDiv(docCount, blockCount);

    const auto pairCount = pairs.ysize();
    const auto pairPart = CeilDiv(pairCount, blockCount);

    NCB::MapMerge(
        localExecutor,
        fold.GetCalcStatsIndexRanges(),
        /*mapFunc*/[&](NCB::TIndexRange<int> partIndexRange, TPairwiseStats* output) {
            Y_ASSERT(!partIndexRange.Empty());

            auto docIndexRange = NCB::TIndexRange<int>(
                Min(docCount, docPart * partIndexRange.Begin),
                Min(docCount, docPart * partIndexRange.End)
            );

            auto pairIndexRange = NCB::TIndexRange<int>(
                Min(pairCount, pairPart * partIndexRange.Begin),
                Min(pairCount, pairPart * partIndexRange.End)
            );

            auto computePairwiseStats = [&] (
                const auto& column,
                TMaybe<const TExclusiveFeaturesBundle*> exclusiveFeaturesBundle = Nothing()
            ) {
                ComputePairwiseStats(
                    fold,
                    weightedDerivativesData,
                    pairs,
                    leafCount,
                    indexer.BucketCount,
                    oneHotMaxSize,
                    exclusiveFeaturesBundle,
                    column,
                    docIndexRange,
                    pairIndexRange,
                    output);
            };

            switch (splitEnsemble.Type) {
                case ESplitEnsembleType::OneFeature:
                    {
                        const auto& splitCandidate = splitEnsemble.SplitCandidate;
                        output->SplitEnsembleSpec = TSplitEnsembleSpec::OneSplit(splitCandidate.Type);

                        switch (splitCandidate.Type) {
                            case ESplitType::OnlineCtr:
                                {
                                    const TCtr& ctr = splitCandidate.Ctr;
                                    TConstArrayRef<ui8> buckets =
                                        GetCtr(allCtrs, ctr.Projection)
                                            .Feature[ctr.CtrIdx][ctr.TargetBorderIdx][ctr.PriorIdx];

                                    ComputePairwiseStats<ui8>(
                                        ESplitEnsembleType::OneFeature,
                                        weightedDerivativesData,
                                        pairs,
                                        leafCount,
                                        indexer.BucketCount,
                                        oneHotMaxSize,
                                        fold.Indices,
                                        /*exclusiveFeaturesBundle*/ Nothing(),
                                        docIndexRange,
                                        pairIndexRange,
                                        [buckets](ui32 docIdx) { return buckets[docIdx]; },
                                        output);
                                }
                                break;
                            case ESplitType::FloatFeature:
                                computePairwiseStats(
                                    **objectsDataProvider.GetNonPackedFloatFeature(
                                        (ui32)splitCandidate.FeatureIdx
                                    ));
                                break;
                            case ESplitType::OneHotFeature:
                                computePairwiseStats(
                                    **objectsDataProvider.GetNonPackedCatFeature(
                                        (ui32)splitCandidate.FeatureIdx
                                    ));
                                break;
                        }
                    }
                    break;
                case ESplitEnsembleType::BinarySplits:
                    output->SplitEnsembleSpec = TSplitEnsembleSpec::BinarySplitsPack();
                    computePairwiseStats(
                        objectsDataProvider.GetBinaryFeaturesPack(splitEnsemble.BinarySplitsPackRef.PackIdx)
                    );
                    break;
                case ESplitEnsembleType::ExclusiveBundle:
                    const auto bundleIdx = splitEnsemble.ExclusiveFeaturesBundleRef.BundleIdx;
                    const auto* bundleMetaData =
                        &(objectsDataProvider.GetExclusiveFeatureBundlesMetaData()[bundleIdx]);
                    output->SplitEnsembleSpec = TSplitEnsembleSpec::ExclusiveFeatureBundle(*bundleMetaData);

                    computePairwiseStats(
                        objectsDataProvider.GetExclusiveFeaturesBundle(bundleIdx),
                        bundleMetaData);
                    break;
            }
        },
        /*mergeFunc*/[&](TPairwiseStats* output, TVector<TPairwiseStats>&& addVector) {
            for (const auto& addItem : addVector) {
                output->Add(addItem);
            }
        },
        stats
    );
}


template <typename TFullIndexType, typename TIsCaching>
static void CalcStatsImpl(
    const TCalcScoreFold& fold,
    const TQuantizedForCPUObjectsDataProvider& objectsDataProvider,
    const TFlatPairsInfo& /*pairs*/,
    const std::tuple<const TOnlineCTRHash&, const TOnlineCTRHash&>& allCtrs,
    const TSplitEnsemble& splitEnsemble,
    const TStatsIndexer& indexer,
    const TIsCaching& isCaching,
    bool isPlainMode,
    ui32 /*oneHotMaxSize*/,
    int depth,
    int splitStatsCount,
    NPar::TLocalExecutor* localExecutor,
    TBucketStatsRefOptionalHolder* stats
) {
    Y_ASSERT(!isCaching || depth > 0);

    const int docCount = fold.GetDocCount();

    TVector<TFullIndexType> singleIdx;
    singleIdx.yresize(docCount);

    const int statsCount = fold.GetBodyTailCount() * fold.GetApproxDimension() * splitStatsCount;
    const int filledSplitStatsCount = indexer.CalcSize(depth);

    // bodyFunc must accept (bodyTailIdx, dim, bucketStatsArrayBegin) params
    auto forEachBodyTailAndApproxDimension = [&](auto bodyFunc) {
        const int approxDimension = fold.GetApproxDimension();
        for (int bodyTailIdx : xrange(fold.GetBodyTailCount())) {
            for (int dim : xrange(approxDimension)) {
                bodyFunc(bodyTailIdx, dim, (bodyTailIdx * approxDimension + dim) * splitStatsCount);
            }
        }
    };

    NCB::MapMerge(
        localExecutor,
        fold.GetCalcStatsIndexRanges(),
        /*mapFunc*/[&](NCB::TIndexRange<int> indexRange, TBucketStatsRefOptionalHolder* output) {
            NCB::TIndexRange<int> docIndexRange = fold.HasQueryInfo() ?
                NCB::TIndexRange<int>(
                    fold.LearnQueriesInfo[indexRange.Begin].Begin,
                    (indexRange.End == 0) ? 0 : fold.LearnQueriesInfo[indexRange.End - 1].End
                )
                : indexRange;

            BuildSingleIndex(
                fold,
                objectsDataProvider,
                allCtrs,
                splitEnsemble,
                indexer,
                docIndexRange,
                &singleIdx
            );

            if (output->NonInited()) {
                (*output) = TBucketStatsRefOptionalHolder(statsCount);
            } else {
                Y_ASSERT(docIndexRange.Begin == 0);
            }

            forEachBodyTailAndApproxDimension(
                [&](int bodyTailIdx, int dim, int bucketStatsArrayBegin) {
                    TBucketStats* statsSubset = output->GetData().data() + bucketStatsArrayBegin;
                    CalcStatsKernel(
                        isCaching && (indexRange.Begin == 0),
                        singleIdx,
                        fold,
                        isPlainMode,
                        indexer,
                        depth,
                        fold.BodyTailArr[bodyTailIdx],
                        dim,
                        docIndexRange,
                        statsSubset
                    );
                }
            );
        },
        /*mergeFunc*/[&](
            TBucketStatsRefOptionalHolder* output,
            TVector<TBucketStatsRefOptionalHolder>&& addVector
        ) {
            forEachBodyTailAndApproxDimension(
                [&](int /*bodyTailIdx*/, int /*dim*/, int bucketStatsArrayBegin) {
                    TBucketStats* outputStatsSubset =
                        output->GetData().data() + bucketStatsArrayBegin;

                    for (const auto& addItem : addVector) {
                        const TBucketStats* addStatsSubset =
                            addItem.GetData().data() + bucketStatsArrayBegin;
                        for (size_t i : xrange(filledSplitStatsCount)) {
                            (outputStatsSubset + i)->Add(*(addStatsSubset + i));
                        }
                    }
                }
            );
        },
        stats
    );

    if (isCaching) {
        forEachBodyTailAndApproxDimension(
            [&](int /*bodyTailIdx*/, int /*dim*/, int bucketStatsArrayBegin) {
                TBucketStats* statsSubset = stats->GetData().data() + bucketStatsArrayBegin;
                FixUpStats(depth, indexer, fold.SmallestSplitSideValue, statsSubset);
            }
        );
    }
}


inline void UpdateSplitScore(
    bool isPlainMode,
    const TBucketStats& trueStats,
    const TBucketStats& falseStats,
    int splitIdx,
    IPointwiseScoreCalcer* scoreCalcer
) {
    if (isPlainMode) {
        scoreCalcer->AddLeafPlain(splitIdx, falseStats, trueStats);
    } else {
        scoreCalcer->AddLeafOrdered(splitIdx, falseStats, trueStats);
    }
}


/* This function calculates resulting sums for each split given statistics that are calculated for each bucket
 * of the histogram.
 */
template <typename TIsPlainMode, typename THaveMonotonicConstraints>
inline static void UpdateScores(
    const TBucketStats* stats,
    int leafCount,
    const TStatsIndexer& indexer,
    const TSplitEnsembleSpec& splitEnsembleSpec,
    double scaledL2Regularizer,
    TIsPlainMode isPlainMode,
    THaveMonotonicConstraints haveMonotonicConstraints,
    ui32 oneHotMaxSize,
    const TVector<int>& currTreeMonotonicConstraints,
    const TVector<int>& candidateSplitMonotonicConstraints,
    IPointwiseScoreCalcer* scoreCalcer
) {
    Y_ASSERT(haveMonotonicConstraints == !candidateSplitMonotonicConstraints.empty());
    // Used only if monotonic constraints are non trivial.
    TVector<TVector<double>> leafDeltas;
    TVector<TVector<double>> bodyLeafWeights;
    TVector<TVector<double>> tailLeafSumWeightedDers;
    TVector<TVector<double>> tailLeafWeights;
    TVector<int> leafsProcessed;
    if (haveMonotonicConstraints) {
        /* In this case updateSplitScoreClosure simply stores relevant statistics for every leaf and
         * every evaluated split. Then monotonization applies to leaf values and split score is calculated.
         * This implies unnecessary memory usage.
         */
        for (TVector<TVector<double>>* vec : {
            &leafDeltas, &bodyLeafWeights, &tailLeafSumWeightedDers, &tailLeafWeights
        }) {
            vec->resize(scoreCalcer->GetSplitsCount());
            for (auto& perLeafStats : *vec) {
                perLeafStats.resize(2 * leafCount);
            }
        }
        leafsProcessed.resize(scoreCalcer->GetSplitsCount());
    }
    const auto updateSplitScoreClosure = [&] (
        const TBucketStats& trueStats,
        const TBucketStats& falseStats,
        int splitIdx
    ) {
        if (!haveMonotonicConstraints) {
            UpdateSplitScore(
                isPlainMode,
                trueStats,
                falseStats,
                splitIdx,
                scoreCalcer
            );
        } else {
            auto currLeafId = leafsProcessed[splitIdx];
            Y_ASSERT(currLeafId < leafCount);
            for (const auto leafStats : {&falseStats, &trueStats}) {
                double bodyLeafWeight = 0.0;
                if (isPlainMode) {
                    bodyLeafWeight = leafStats->SumWeight;
                    leafDeltas[splitIdx][currLeafId] = CalcAverage(
                        leafStats->SumWeightedDelta,
                        bodyLeafWeight,
                        scaledL2Regularizer
                    );
                } else {
                    // compute leaf value using statistics of current BodyTail body:
                    bodyLeafWeight = leafStats->Count;
                    leafDeltas[splitIdx][currLeafId] = CalcAverage(
                        leafStats->SumDelta,
                        bodyLeafWeight,
                        scaledL2Regularizer
                    );
                }
                /* Note that the following lines perform a reduction from isotonic regression with
                 * l2-regularization to usual isotonic regression with properly modified weights and values.
                 */
                bodyLeafWeights[splitIdx][currLeafId] = bodyLeafWeight + scaledL2Regularizer;
                tailLeafWeights[splitIdx][currLeafId] = leafStats->SumWeight;
                tailLeafSumWeightedDers[splitIdx][currLeafId] = leafStats->SumWeightedDelta;
                currLeafId += leafCount;
            }
            leafsProcessed[splitIdx] += 1;
        }
    };

    // used only if splitEnsembleSpec.Type == ESplitEnsembleType::ExclusiveBundle
    const auto& exclusiveFeaturesBundle = splitEnsembleSpec.ExclusiveFeaturesBundle;

    // allocate one time for all leaves
    TVector<TBucketStats> bundlePartsStats;

    // used only if splitEnsembleSpec.Type == ESplitEnsembleType::ExclusiveBundle
    TVector<bool> useBundlePartForCalcScores; // [bundlePartIdx]

    if (splitEnsembleSpec.Type == ESplitEnsembleType::ExclusiveBundle) {
        bundlePartsStats.resize(exclusiveFeaturesBundle.Parts.size());

        for (const auto& bundlePart : exclusiveFeaturesBundle.Parts) {
            useBundlePartForCalcScores.push_back(UseForCalcScores(bundlePart, oneHotMaxSize));
        }
    }


    for (int leaf = 0; leaf < leafCount; ++leaf) {
        switch (splitEnsembleSpec.Type) {
            case ESplitEnsembleType::OneFeature:
                {
                    auto splitType = splitEnsembleSpec.OneSplitType;

                    TBucketStats allStats{0, 0, 0, 0};

                    for (int bucketIdx = 0; bucketIdx < indexer.BucketCount; ++bucketIdx) {
                        const TBucketStats& leafStats = stats[indexer.GetIndex(leaf, bucketIdx)];
                        allStats.Add(leafStats);
                    }

                    TBucketStats trueStats{0, 0, 0, 0};
                    TBucketStats falseStats{0, 0, 0, 0};
                    if (splitType == ESplitType::OnlineCtr || splitType == ESplitType::FloatFeature) {
                        trueStats = allStats;
                        for (int splitIdx = 0; splitIdx < indexer.BucketCount - 1; ++splitIdx) {
                            falseStats.Add(stats[indexer.GetIndex(leaf, splitIdx)]);
                            trueStats.Remove(stats[indexer.GetIndex(leaf, splitIdx)]);

                            updateSplitScoreClosure(trueStats, falseStats, splitIdx);
                        }
                    } else {
                        Y_ASSERT(splitType == ESplitType::OneHotFeature);
                        falseStats = allStats;
                        for (int bucketIdx = 0; bucketIdx < indexer.BucketCount; ++bucketIdx) {
                            if (bucketIdx > 0) {
                                falseStats.Add(stats[indexer.GetIndex(leaf, bucketIdx - 1)]);
                            }
                            falseStats.Remove(stats[indexer.GetIndex(leaf, bucketIdx)]);

                            updateSplitScoreClosure(
                                /*trueStats*/ stats[indexer.GetIndex(leaf, bucketIdx)],
                                falseStats,
                                bucketIdx
                            );
                        }
                    }
                }
                break;
            case ESplitEnsembleType::BinarySplits:
                {
                    int binaryFeaturesCount = (int)GetValueBitCount(indexer.BucketCount - 1);
                    for (int binFeatureIdx = 0; binFeatureIdx < binaryFeaturesCount; ++binFeatureIdx) {
                        TBucketStats trueStats{0, 0, 0, 0};
                        TBucketStats falseStats{0, 0, 0, 0};

                        for (int bucketIdx = 0; bucketIdx < indexer.BucketCount; ++bucketIdx) {
                            auto& dstStats = ((bucketIdx >> binFeatureIdx) & 1) ? trueStats : falseStats;
                            dstStats.Add(stats[indexer.GetIndex(leaf, bucketIdx)]);
                        }

                        updateSplitScoreClosure(trueStats, falseStats, binFeatureIdx);
                    }
                }
                break;
            case ESplitEnsembleType::ExclusiveBundle:
                {
                    TBucketStats allStats = stats[indexer.GetIndex(leaf, indexer.BucketCount - 1)];

                    for (auto bundlePartIdx : xrange(exclusiveFeaturesBundle.Parts.size())) {
                        auto& bundlePartStats = bundlePartsStats[bundlePartIdx];
                        bundlePartStats = TBucketStats{0, 0, 0, 0};

                        for (auto bucketIdx : exclusiveFeaturesBundle.Parts[bundlePartIdx].Bounds.Iter()) {
                            const TBucketStats& leafStats = stats[indexer.GetIndex(leaf, bucketIdx)];
                            bundlePartStats.Add(leafStats);
                        }
                        allStats.Add(bundlePartStats);
                    }

                    ui32 binsBegin = 0;
                    for (auto bundlePartIdx : xrange(exclusiveFeaturesBundle.Parts.size())) {
                        if (!useBundlePartForCalcScores[bundlePartIdx]) {
                            continue;
                        }

                        const auto& bundlePart = exclusiveFeaturesBundle.Parts[bundlePartIdx];
                        auto binBounds = bundlePart.Bounds;

                        if (bundlePart.FeatureType == EFeatureType::Float) {
                            TBucketStats falseStats = allStats;
                            TBucketStats trueStats = bundlePartsStats[bundlePartIdx];
                            falseStats.Remove(bundlePartsStats[bundlePartIdx]);

                            for (ui32 splitIdx = 0; splitIdx < binBounds.GetSize(); ++splitIdx) {
                                if (splitIdx != 0) {
                                    const auto& statsPart
                                        = stats[indexer.GetIndex(leaf, binBounds.Begin + splitIdx - 1)];
                                    falseStats.Add(statsPart);
                                    trueStats.Remove(statsPart);
                                }

                                updateSplitScoreClosure(trueStats, falseStats, binsBegin + splitIdx);
                            }
                            binsBegin += binBounds.GetSize();
                        } else {
                            Y_ASSERT(bundlePart.FeatureType == EFeatureType::Categorical);
                            Y_ASSERT((binBounds.GetSize() + 1) <= oneHotMaxSize);

                            /* for binary features split on value 0 is the same as split on value 1
                             * so don't double the calculations,
                             * it also maintains compatibility with binary packed binary categorical features
                             * where value 1 is always assumed
                             */
                            if (binBounds.GetSize() > 1) {
                                TBucketStats trueStats = allStats;
                                trueStats.Remove(bundlePartsStats[bundlePartIdx]);

                                updateSplitScoreClosure(
                                    trueStats,
                                    /*falseStats*/ bundlePartsStats[bundlePartIdx],
                                    binsBegin
                                );
                            }

                            for (ui32 binIdx = 0; binIdx < binBounds.GetSize(); ++binIdx) {
                                const auto& statsPart
                                    = stats[indexer.GetIndex(leaf, binBounds.Begin + binIdx)];

                                TBucketStats falseStats = allStats;
                                falseStats.Remove(statsPart);

                                updateSplitScoreClosure(
                                    /*trueStats*/ statsPart,
                                    falseStats,
                                    binsBegin + binIdx + 1
                                );
                            }

                            binsBegin += binBounds.GetSize() + 1;
                        }
                    }
                }
                break;
        }
    }

    if (haveMonotonicConstraints) {
        Y_ASSERT(AllOf(leafDeltas, [=] (const auto& vec) {
            return vec.size() == SafeIntegerCast<size_t>(2 * leafCount);
        }));
        const THashSet<int> possibleNewSplitConstraints(
            candidateSplitMonotonicConstraints.begin(), candidateSplitMonotonicConstraints.end()
        );
        THashMap<int, TVector<TVector<ui32>>> possibleLeafIndexOrders;
        auto monotonicConstraints = currTreeMonotonicConstraints;
        for (int newSplitMonotonicConstraint : possibleNewSplitConstraints) {
            monotonicConstraints.push_back(newSplitMonotonicConstraint);
            possibleLeafIndexOrders[newSplitMonotonicConstraint] = BuildMonotonicLinearOrdersOnLeafs(
                monotonicConstraints
            );
            monotonicConstraints.pop_back();
        }
        for (int splitIdx : xrange(scoreCalcer->GetSplitsCount())) {
            const auto& indexOrder = possibleLeafIndexOrders[candidateSplitMonotonicConstraints[splitIdx]];
            for (const auto& monotonicSubtreeIndexOrder : indexOrder) {
                CalcOneDimensionalIsotonicRegression(
                    leafDeltas[splitIdx],
                    bodyLeafWeights[splitIdx],
                    monotonicSubtreeIndexOrder,
                    &leafDeltas[splitIdx]
                );
            }
            for (int leafIndex = 0; leafIndex < 2 * leafCount; ++leafIndex) {
                const double leafDelta = leafDeltas[splitIdx][leafIndex];
                const TBucketStats leafStats {
                    tailLeafSumWeightedDers[splitIdx][leafIndex],
                    tailLeafWeights[splitIdx][leafIndex],
                    0, // it is unused in following call
                    0  // it is unused in following call
                };
                scoreCalcer->AddLeaf(splitIdx, leafDelta, leafStats);
            }
        }
    }
}


static void CalculateNonPairwiseScore(
    const TCalcScoreFold& fold,
    const TFold& initialFold,
    const TSplitEnsembleSpec& splitEnsembleSpec,
    bool isPlainMode,
    const int leafCount,
    const float l2Regularizer,
    const ui32 oneHotMaxSize,
    const TStatsIndexer& indexer,
    const TBucketStats* splitStats,
    int splitStatsCount,
    const TVector<int>& currTreeMonotonicConstraints,
    const TVector<int>& candidateSplitMonotonicConstraints,
    IPointwiseScoreCalcer* scoreCalcer
) {
    const int approxDimension = fold.GetApproxDimension();
    const bool haveMonotonicConstraints = !candidateSplitMonotonicConstraints.empty();

    for (int bodyTailIdx = 0; bodyTailIdx < fold.GetBodyTailCount(); ++bodyTailIdx) {
        const double sumAllWeights = initialFold.BodyTailArr[bodyTailIdx].BodySumWeight;
        const int docCount = initialFold.BodyTailArr[bodyTailIdx].BodyFinish;
        const double scaledL2Regularizer = l2Regularizer * (sumAllWeights / docCount);
        scoreCalcer->SetL2Regularizer(scaledL2Regularizer);
        const auto updateScores = [&] (auto isPlainMode, auto haveMonotonicConstraints, const auto* stats) {
            UpdateScores(
                stats,
                leafCount,
                indexer,
                splitEnsembleSpec,
                scaledL2Regularizer,
                isPlainMode,
                haveMonotonicConstraints,
                oneHotMaxSize,
                currTreeMonotonicConstraints,
                candidateSplitMonotonicConstraints,
                scoreCalcer
            );
        };
        for (int dim = 0; dim < approxDimension; ++dim) {
            const TBucketStats* stats = splitStats
                + (bodyTailIdx * approxDimension + dim) * splitStatsCount;
            if (isPlainMode && haveMonotonicConstraints) {
                updateScores(std::true_type(), std::true_type(), stats);
            } else if (isPlainMode && !haveMonotonicConstraints) {
                updateScores(std::true_type(), std::false_type(), stats);
            } else if (!isPlainMode && haveMonotonicConstraints) {
                updateScores(std::false_type(), std::true_type(), stats);
            } else {
                updateScores(std::false_type(), std::false_type(), stats);
            }
        }
    }
}


void CalcStatsAndScores(
    const TQuantizedForCPUObjectsDataProvider& objectsDataProvider,
    const std::tuple<const TOnlineCTRHash&, const TOnlineCTRHash&>& allCtrs,
    const TCalcScoreFold& fold,
    const TCalcScoreFold& prevLevelData,
    const TFold* initialFold,
    const TFlatPairsInfo& pairs,
    const NCatboostOptions::TCatBoostOptions& fitParams,
    const TCandidateInfo& candidateInfo,
    int depth,
    bool useTreeLevelCaching,
    const TVector<int>& currTreeMonotonicConstraints,
    const TVector<int>& monotonicConstraints,
    NPar::TLocalExecutor* localExecutor,
    TBucketStatsCache* statsFromPrevTree,
    TStats3D* stats3d,
    TPairwiseStats* pairwiseStats,
    IScoreCalcer* scoreCalcer
) {
    CB_ENSURE(
        stats3d || pairwiseStats || scoreCalcer,
        "stats3d, pairwiseStats, and scoreCalcer are empty - nothing to calculate"
    );
    CB_ENSURE(!scoreCalcer || initialFold, "initialFold must be non-nullptr for scores calculation");

    const auto& splitEnsemble = candidateInfo.SplitEnsemble;
    const bool isPairwiseScoring = IsPairwiseScoring(fitParams.LossFunctionDescription->GetLossFunction());

    const int bucketCount = GetBucketCount(
        splitEnsemble,
        *objectsDataProvider.GetQuantizedFeaturesInfo(),
        objectsDataProvider.GetPackedBinaryFeaturesSize(),
        objectsDataProvider.GetExclusiveFeatureBundlesMetaData()
    );
    const TStatsIndexer indexer(bucketCount);
    const int fullIndexBitCount = depth + GetValueBitCount(bucketCount - 1);
    const bool isPlainMode = IsPlainMode(fitParams.BoostingOptions->BoostingType);

    const float l2Regularizer = static_cast<const float>(fitParams.ObliviousTreeOptions->L2Reg);
    const ui32 oneHotMaxSize = fitParams.CatFeatureParams.Get().OneHotMaxSize.Get();

    decltype(auto) selectCalcStatsImpl = [&] (
        auto isCaching,
        const TCalcScoreFold& fold,
        int splitStatsCount,
        auto* stats
    ) {
        if (fullIndexBitCount <= 8) {
            CalcStatsImpl<ui8>(
                fold,
                objectsDataProvider,
                pairs,
                allCtrs,
                splitEnsemble,
                indexer,
                isCaching,
                isPlainMode,
                oneHotMaxSize,
                depth,
                splitStatsCount,
                localExecutor,
                stats
            );
        } else if (fullIndexBitCount <= 16) {
            CalcStatsImpl<ui16>(
                fold,
                objectsDataProvider,
                pairs,
                allCtrs,
                splitEnsemble,
                indexer,
                isCaching,
                isPlainMode,
                oneHotMaxSize,
                depth,
                splitStatsCount,
                localExecutor,
                stats
            );
        } else if (fullIndexBitCount <= 32) {
            CalcStatsImpl<ui32>(
                fold,
                objectsDataProvider,
                pairs,
                allCtrs,
                splitEnsemble,
                indexer,
                isCaching,
                isPlainMode,
                oneHotMaxSize,
                depth,
                splitStatsCount,
                localExecutor,
                stats
            );
        }
    };

    // Pairwise scoring doesn't use statistics from previous tree level
    if (isPairwiseScoring) {
        CB_ENSURE(!stats3d, "Pairwise scoring is incompatible with stats3d calculation");

        TPairwiseStats localPairwiseStats;
        if (pairwiseStats == nullptr) {
            pairwiseStats = &localPairwiseStats;
        }
        pairwiseStats->SplitEnsembleSpec = TSplitEnsembleSpec(
            splitEnsemble,
            objectsDataProvider.GetExclusiveFeatureBundlesMetaData()
        );

        selectCalcStatsImpl(/*isCaching*/ std::false_type(), fold, /*splitStatsCount*/0, pairwiseStats);

        if (scoreCalcer) {
            const float pairwiseBucketWeightPriorReg =
                static_cast<const float>(fitParams.ObliviousTreeOptions->PairwiseNonDiagReg);
            CalculatePairwiseScore(
                *pairwiseStats,
                bucketCount,
                l2Regularizer,
                pairwiseBucketWeightPriorReg,
                oneHotMaxSize,
                dynamic_cast<TPairwiseScoreCalcer*>(scoreCalcer)
            );
        }
    } else {
        CB_ENSURE(!pairwiseStats, "Per-object scoring is incompatible with pairwiseStats calculation");
        TBucketStatsRefOptionalHolder extOrInSplitStats;
        int splitStatsCount = 0;

        const auto& treeOptions = fitParams.ObliviousTreeOptions.Get();

        if (!useTreeLevelCaching) {
            splitStatsCount = indexer.CalcSize(depth);
            const int statsCount =
                fold.GetBodyTailCount() * fold.GetApproxDimension() * splitStatsCount;

            if (stats3d != nullptr) {
                stats3d->Stats.yresize(statsCount);
                stats3d->BucketCount = bucketCount;
                stats3d->MaxLeafCount = 1U << depth;
                stats3d->SplitEnsembleSpec = TSplitEnsembleSpec(
                    splitEnsemble,
                    objectsDataProvider.GetExclusiveFeatureBundlesMetaData()
                );

                extOrInSplitStats = TBucketStatsRefOptionalHolder(stats3d->Stats);
            }
            selectCalcStatsImpl(
                /*isCaching*/ std::false_type(),
                fold,
                splitStatsCount,
                &extOrInSplitStats
            );
        } else {
            splitStatsCount = indexer.CalcSize(treeOptions.MaxDepth);
            bool areStatsDirty;

            // thread-safe access
            TVector<TBucketStats, TPoolAllocator>& splitStatsFromCache =
                statsFromPrevTree->GetStats(splitEnsemble, splitStatsCount, &areStatsDirty);
            extOrInSplitStats = TBucketStatsRefOptionalHolder(splitStatsFromCache);
            if (depth == 0 || areStatsDirty) {
                selectCalcStatsImpl(
                    /*isCaching*/ std::false_type(),
                    fold,
                    splitStatsCount,
                    &extOrInSplitStats
                );
            } else {
                selectCalcStatsImpl(
                    /*isCaching*/ std::true_type(),
                    prevLevelData,
                    splitStatsCount,
                    &extOrInSplitStats
                );
            }
            if (stats3d) {
                TBucketStatsCache::GetStatsInUse(fold.GetBodyTailCount() * fold.GetApproxDimension(),
                    splitStatsCount,
                    indexer.CalcSize(depth),
                    splitStatsFromCache
                ).swap(stats3d->Stats);
                stats3d->BucketCount = bucketCount;
                stats3d->MaxLeafCount = 1U << depth;
                stats3d->SplitEnsembleSpec = TSplitEnsembleSpec(
                    splitEnsemble,
                    objectsDataProvider.GetExclusiveFeatureBundlesMetaData()
                );
            }
        }
        if (scoreCalcer) {
            const int leafCount = 1 << depth;
            TSplitEnsembleSpec splitEnsembleSpec(
                splitEnsemble,
                objectsDataProvider.GetExclusiveFeatureBundlesMetaData()
            );
            const int candidateSplitCount = CalcSplitsCount(
                splitEnsembleSpec, indexer.BucketCount, oneHotMaxSize
            );
            scoreCalcer->SetSplitsCount(candidateSplitCount);

            TVector<int> candidateSplitMonotonicConstraints;
            if (!monotonicConstraints.empty()) {
                candidateSplitMonotonicConstraints.resize(candidateSplitCount, 0);
                for (int splitIdx : xrange(candidateSplitCount)) {
                    const auto split = candidateInfo.GetSplit(
                        splitIdx, objectsDataProvider, oneHotMaxSize
                    );
                    if (split.Type == ESplitType::FloatFeature) {
                        Y_ASSERT(split.FeatureIdx >= 0);
                        candidateSplitMonotonicConstraints[splitIdx] =
                            monotonicConstraints[split.FeatureIdx];
                    }
                }
            }

            CalculateNonPairwiseScore(
                fold,
                *initialFold,
                splitEnsembleSpec,
                isPlainMode,
                leafCount,
                l2Regularizer,
                oneHotMaxSize,
                indexer,
                extOrInSplitStats.GetData().data(),
                splitStatsCount,
                currTreeMonotonicConstraints,
                candidateSplitMonotonicConstraints,
                dynamic_cast<IPointwiseScoreCalcer*>(scoreCalcer)
            );
        }
    }
}

TVector<double> GetScores(
    const TStats3D& stats3d,
    int depth,
    double sumAllWeights,
    int allDocCount,
    const NCatboostOptions::TCatBoostOptions& fitParams
) {
    const TVector<TBucketStats>& bucketStats = stats3d.Stats;
    const int splitStatsCount = stats3d.BucketCount * stats3d.MaxLeafCount;
    const int bucketCount = stats3d.BucketCount;
    const float l2Regularizer = static_cast<const float>(fitParams.ObliviousTreeOptions->L2Reg);
    const ui32 oneHotMaxSize = fitParams.CatFeatureParams.Get().OneHotMaxSize.Get();
    const int leafCount = 1 << depth;
    const TStatsIndexer indexer(bucketCount);

    THolder<IPointwiseScoreCalcer> scoreCalcer;
    switch (fitParams.ObliviousTreeOptions->ScoreFunction) {
        case EScoreFunction::Cosine:
            scoreCalcer = MakeHolder<TCosineScoreCalcer>();
            break;
        case EScoreFunction::L2:
            scoreCalcer = MakeHolder<TL2ScoreCalcer>();
            break;
        default:
            CB_ENSURE(false, "Error: score function for CPU should be Cosine or L2");
            break;
    }
    scoreCalcer->SetSplitsCount(CalcSplitsCount(stats3d.SplitEnsembleSpec, bucketCount, oneHotMaxSize));

    const double scaledL2Regularizer = l2Regularizer * (sumAllWeights / allDocCount);
    scoreCalcer->SetL2Regularizer(scaledL2Regularizer);
    for (int statsIdx = 0; statsIdx * splitStatsCount < bucketStats.ysize(); ++statsIdx) {
        const TBucketStats* stats = GetDataPtr(bucketStats) + statsIdx * splitStatsCount;
        UpdateScores(
            stats,
            leafCount,
            indexer,
            stats3d.SplitEnsembleSpec,
            scaledL2Regularizer,
            /*isPlainMode=*/std::true_type(),
            /*haveMonotonicConstraints*/std::false_type(),
            oneHotMaxSize,
            /*currTreeMonotonicConstraints*/{},
            /*candidateSplitMonotonicConstraints*/{},
            scoreCalcer.Get()
        );
    }
    return scoreCalcer->GetScores();
}
