#include "apply.h"
#include "index_calcer.h"
#include "features_data_helpers.h"

#include <catboost/libs/data_new/data_provider.h>
#include <catboost/libs/data_new/model_dataset_compatibility.h>
#include <catboost/libs/eval_result/eval_helpers.h>
#include <catboost/libs/helpers/exception.h>
#include <catboost/libs/logging/logging.h>
#include <catboost/libs/model/cpu/evaluator.h>

#include <util/generic/array_ref.h>
#include <util/generic/cast.h>
#include <util/generic/utility.h>

#include <cmath>


using namespace NCB;
using NPar::TLocalExecutor;


TLocalExecutor::TExecRangeParams GetBlockParams(int executorThreadCount, int docCount, int begin, int end) {
    const int threadCount = executorThreadCount + 1; // one for current thread

    // for 1 iteration it will be 7k docs, for 10k iterations it will be 100 docs.
    const int minBlockSize = ceil(10000.0 / sqrt(end - begin + 1));
    const int effectiveBlockCount = Min(threadCount, (docCount + minBlockSize - 1) / minBlockSize);

    TLocalExecutor::TExecRangeParams blockParams(0, docCount);
    blockParams.SetBlockCount(effectiveBlockCount);
    return blockParams;
};

namespace {
    class IQuantizedBlockVisitor {
    public:
        virtual ~IQuantizedBlockVisitor() = default;

        virtual void Do(
            const NModelEvaluation::IQuantizedData& quantizedBlock,
            ui32 objectBlockStart,
            ui32 objectBlockEnd) = 0;
    };
}


static void BlockedEvaluation(
    const TFullModel& model,
    const TObjectsDataProvider& objectsData,
    ui32 objectBlockStart,
    ui32 objectBlockEnd,
    ui32 subBlockSize,
    IQuantizedBlockVisitor* visitor
) {
    THolder<IFeaturesBlockIterator> featuresBlockIterator
        = CreateFeaturesBlockIterator(model, objectsData, objectBlockStart, objectBlockEnd);

    for (; objectBlockStart < objectBlockEnd; objectBlockStart += subBlockSize) {
        const ui32 currentBlockSize = Min(objectBlockEnd - objectBlockStart, subBlockSize);

        featuresBlockIterator->NextBlock((size_t)currentBlockSize);

        auto quantizedBlock = MakeQuantizedFeaturesForEvaluator(
            model,
            *featuresBlockIterator,
            objectBlockStart,
            objectBlockStart + currentBlockSize);

        visitor->Do(*quantizedBlock, objectBlockStart, objectBlockStart + currentBlockSize);
    }
}


namespace {
    class TApplyVisitor final : public IQuantizedBlockVisitor {
    public:
        TApplyVisitor(
            const TFullModel& model,
            int treeBegin,
            int treeEnd,
            TArrayRef<double> approxesFlat)
            : ModelEvaluator(model.GetCurrentEvaluator())
            , ApproxDimension(model.GetDimensionsCount())
            , TreeBegin(treeBegin)
            , TreeEnd(treeEnd)
            , ApproxesFlat(approxesFlat)
        {}

        void Do(
            const NModelEvaluation::IQuantizedData& quantizedBlock,
            ui32 objectBlockStart,
            ui32 objectBlockEnd) override
        {
            ModelEvaluator->Calc(
                &quantizedBlock,
                TreeBegin,
                TreeEnd,
                MakeArrayRef(
                    ApproxesFlat.data() + objectBlockStart * ApproxDimension,
                    (objectBlockEnd - objectBlockStart) * ApproxDimension));
        }

    private:
        NModelEvaluation::TConstModelEvaluatorPtr ModelEvaluator;
        size_t ApproxDimension;
        int TreeBegin;
        int TreeEnd;
        TArrayRef<double> ApproxesFlat;
    };
}

TVector<TVector<double>> ApplyModelMulti(
    const TFullModel& model,
    const TObjectsDataProvider& objectsData,
    const EPredictionType predictionType,
    int begin, /*= 0*/
    int end,   /*= 0*/
    TLocalExecutor* executor)
{
    const int docCount = SafeIntegerCast<int>(objectsData.GetObjectCount());
    const int approxesDimension = model.GetDimensionsCount();
    TVector<double> approxesFlat(docCount * approxesDimension);
    if (docCount > 0) {
        end = end == 0 ? model.GetTreeCount() : Min<int>(end, model.GetTreeCount());
        const int executorThreadCount = executor ? executor->GetThreadCount() : 0;
        auto blockParams = GetBlockParams(executorThreadCount, docCount, begin, end);

        TApplyVisitor visitor(model, begin, end, approxesFlat);

        const ui32 subBlockSize = ui32(NModelEvaluation::FORMULA_EVALUATION_BLOCK_SIZE * 64);

        const auto applyOnBlock = [&](int blockId) {
            const int blockFirstIdx = blockParams.FirstId + blockId * blockParams.GetBlockSize();
            const int blockLastIdx = Min(blockParams.LastId, blockFirstIdx + blockParams.GetBlockSize());

            BlockedEvaluation(model, objectsData, (ui32)blockFirstIdx, (ui32)blockLastIdx, subBlockSize, &visitor);
        };
        if (executor) {
            executor->ExecRangeWithThrow(applyOnBlock, 0, blockParams.GetBlockCount(), TLocalExecutor::WAIT_COMPLETE);
        } else {
            applyOnBlock(0);
        }
    }

    TVector<TVector<double>> approxes(approxesDimension);
    if (approxesDimension == 1) { //shortcut
        approxes[0].swap(approxesFlat);
    } else {
        for (int dim = 0; dim < approxesDimension; ++dim) {
            approxes[dim].yresize(docCount);
            for (int doc = 0; doc < docCount; ++doc) {
                approxes[dim][doc] = approxesFlat[approxesDimension * doc + dim];
            };
        }
    }

    if (predictionType == EPredictionType::InternalRawFormulaVal) {
        //shortcut
        return approxes;
    } else {
        return PrepareEvalForInternalApprox(predictionType, model, approxes, executor);
    }
}

TVector<TVector<double>> ApplyModelMulti(
    const TFullModel& model,
    const TObjectsDataProvider& objectsData,
    bool verbose,
    const EPredictionType predictionType,
    int begin,
    int end,
    int threadCount)
{
    TSetLoggingVerboseOrSilent inThisScope(verbose);

    NPar::TLocalExecutor executor;
    executor.RunAdditionalThreads(threadCount - 1);
    const auto& result = ApplyModelMulti(model, objectsData, predictionType, begin, end, &executor);
    return result;
}

TVector<TVector<double>> ApplyModelMulti(
    const TFullModel& model,
    const TDataProvider& data,
    bool verbose,
    const EPredictionType predictionType,
    int begin,
    int end,
    int threadCount)
{
    auto approxes = ApplyModelMulti(model, *data.ObjectsData, verbose, predictionType, begin, end, threadCount);
    if (const auto& baseline = data.RawTargetData.GetBaseline()) {
        for (size_t i = 0; i < approxes.size(); ++i) {
            for (size_t j = 0; j < approxes[0].size(); ++j) {
                approxes[i][j] += (*baseline)[i][j];
            }
        }
    }
    return approxes;
}

void TModelCalcerOnPool::ApplyModelMulti(
    const EPredictionType predictionType,
    int begin,
    int end,
    TVector<double>* flatApproxBuffer,
    TVector<TVector<double>>* approx)
{
    const ui32 docCount = ObjectsData->GetObjectCount();
    auto approxDimension = Model->GetDimensionsCount();
    TVector<double>& approxFlat = *flatApproxBuffer;
    approxFlat.resize(static_cast<unsigned long>(docCount * approxDimension)); // TODO(annaveronika): yresize?

    if (end == 0) {
        end = Model->GetTreeCount();
    } else {
        end = Min<int>(end, Model->GetTreeCount());
    }

    Executor->ExecRangeWithThrow(
        [&, this](int blockId) {
            const int blockFirstId = BlockParams.FirstId + blockId * BlockParams.GetBlockSize();
            const int blockLastId = Min(BlockParams.LastId, blockFirstId + BlockParams.GetBlockSize());
            TArrayRef<double> resultRef(
                approxFlat.data() + blockFirstId * approxDimension,
                (blockLastId - blockFirstId) * approxDimension);
            ModelEvaluator->Calc(QuantizedDataForThreads[blockId].Get(), begin, end, resultRef);
        },
        0,
        BlockParams.GetBlockCount(),
        NPar::TLocalExecutor::WAIT_COMPLETE);

    approx->resize(approxDimension);

    if (approxDimension == 1) { //shortcut
        (*approx)[0].swap(approxFlat);
    } else {
        for (auto& approxProjection : *approx) {
            approxProjection.clear();
            approxProjection.resize(docCount);
        }
        for (ui32 dim = 0; dim < approxDimension; ++dim) {
            for (ui32 doc = 0; doc < docCount; ++doc) {
                (*approx)[dim][doc] = approxFlat[approxDimension * doc + dim];
            };
        }
    }

    if (predictionType == EPredictionType::InternalRawFormulaVal) {
        //shortcut
        return;
    } else {
        (*approx) = PrepareEvalForInternalApprox(predictionType, *Model, *approx, Executor);
    }
    flatApproxBuffer->clear();
}

TModelCalcerOnPool::TModelCalcerOnPool(
    const TFullModel& model,
    TObjectsDataProviderPtr objectsData,
    NPar::TLocalExecutor* executor)
    : Model(&model)
    , ModelEvaluator(model.GetCurrentEvaluator())
    , ObjectsData(objectsData)
    , Executor(executor)
    , BlockParams(0, SafeIntegerCast<int>(objectsData->GetObjectCount()))
{
    if (BlockParams.FirstId == BlockParams.LastId) {
        return;
    }
    const int threadCount = executor->GetThreadCount() + 1; // one for current thread
    BlockParams.SetBlockCount(threadCount);
    QuantizedDataForThreads.resize(BlockParams.GetBlockCount());

    executor->ExecRangeWithThrow(
        [this, objectsData](int blockId) {
            const int blockFirstIdx = BlockParams.FirstId + blockId * BlockParams.GetBlockSize();
            const int blockLastIdx = Min(BlockParams.LastId, blockFirstIdx + BlockParams.GetBlockSize());
            QuantizedDataForThreads[blockId] = MakeQuantizedFeaturesForEvaluator(*Model, *objectsData, blockFirstIdx, blockLastIdx);
        },
        0,
        BlockParams.GetBlockCount(),
        NPar::TLocalExecutor::WAIT_COMPLETE
    );
}

TLeafIndexCalcerOnPool::TLeafIndexCalcerOnPool(
    const TFullModel& model,
    NCB::TObjectsDataProviderPtr objectsData,
    int treeStart,
    int treeEnd)
    : Model(model)
    , ModelEvaluator(model.GetCurrentEvaluator())
    , FeaturesBlockIterator(CreateFeaturesBlockIterator(model, *objectsData, 0, objectsData->GetObjectCount()))
    , DocCount(objectsData->GetObjectCount())
    , TreeStart(treeStart)
    , TreeEnd(treeEnd)
    , CurrBatchStart(0)
    , CurrBatchSize(Min(DocCount, NModelEvaluation::FORMULA_EVALUATION_BLOCK_SIZE))
    , CurrDocIndex(0)
{
    CB_ENSURE(treeStart >= 0);
    CB_ENSURE(treeEnd >= 0);

    CurrentBatchLeafIndexes.yresize(CurrBatchSize * (TreeEnd - TreeStart));

    CalcNextBatch();
}

bool TLeafIndexCalcerOnPool::Next() {
    ++CurrDocIndex;
    if (CurrDocIndex < DocCount) {
        if (CurrDocIndex == CurrBatchStart + CurrBatchSize) {
            CurrBatchStart += CurrBatchSize;
            CurrBatchSize = Min(DocCount - CurrDocIndex, NModelEvaluation::FORMULA_EVALUATION_BLOCK_SIZE);
            CalcNextBatch();
        }
        return true;
    } else {
        return false;
    }
}

bool TLeafIndexCalcerOnPool::CanGet() const {
    return CurrDocIndex < DocCount;
}

TVector<NModelEvaluation::TCalcerIndexType> TLeafIndexCalcerOnPool::Get() const {
    const auto treeCount = TreeEnd - TreeStart;
    const auto docIndexInBatch = CurrDocIndex - CurrBatchStart;
    TVector<NModelEvaluation::TCalcerIndexType> result;
    result.reserve(treeCount);
    for (size_t treeNum = 0; treeNum < treeCount; ++treeNum) {
        result.push_back(CurrentBatchLeafIndexes[docIndexInBatch + treeNum * CurrBatchSize]);
    }
    return result;
}

void TLeafIndexCalcerOnPool::CalcNextBatch() {
    FeaturesBlockIterator->NextBlock(CurrBatchSize);

    auto quantizedBlock = MakeQuantizedFeaturesForEvaluator(
        Model,
        *FeaturesBlockIterator,
        CurrBatchStart,
        CurrBatchStart + CurrBatchSize);

    ModelEvaluator->CalcLeafIndexes(quantizedBlock.Get(), TreeStart, TreeEnd, CurrentBatchLeafIndexes);
}


namespace {
    // don't use from different threads because of TransposedLeafIndicesBuffer
    class TLeafCalcerVisitor final : public IQuantizedBlockVisitor {
    public:
        TLeafCalcerVisitor(
            const TFullModel& model,
            int treeBegin,
            int treeEnd,
            TArrayRef<NModelEvaluation::TCalcerIndexType> leafIndices)
            : ModelEvaluator(model.GetCurrentEvaluator())
            , TreeBegin(treeBegin)
            , TreeEnd(treeEnd)
            , LeafIndices(leafIndices)
        {}

        void Do(
            const NModelEvaluation::IQuantizedData& quantizedBlock,
            ui32 objectBlockStart,
            ui32 objectBlockEnd) override
        {
            const int treeCount = TreeEnd - TreeBegin;
            const ui32 objectBlockSize = objectBlockEnd - objectBlockStart;
            const ui32 indexBlockSize = objectBlockSize * (ui32)treeCount;
            TransposedLeafIndicesBuffer.yresize(indexBlockSize);

            ModelEvaluator->CalcLeafIndexes(&quantizedBlock, TreeBegin, TreeEnd, TransposedLeafIndicesBuffer);

            NModelEvaluation::Transpose2DArray<NModelEvaluation::TCalcerIndexType>(
                 TransposedLeafIndicesBuffer,
                 treeCount,
                 objectBlockSize,
                 MakeArrayRef(LeafIndices.data() + objectBlockStart * treeCount, indexBlockSize)
             );
        }

    private:
        NModelEvaluation::TConstModelEvaluatorPtr ModelEvaluator;
        int TreeBegin;
        int TreeEnd;
        TVector<NModelEvaluation::TCalcerIndexType> TransposedLeafIndicesBuffer;
        TArrayRef<NModelEvaluation::TCalcerIndexType> LeafIndices;
    };
}


TVector<ui32> CalcLeafIndexesMulti(
    const TFullModel& model,
    NCB::TObjectsDataProviderPtr objectsData,
    int treeStart,
    int treeEnd,
    NPar::TLocalExecutor* executor /* = nullptr */)
{
    CB_ENSURE(treeStart >= 0);
    CB_ENSURE(treeEnd >= 0);
    CB_ENSURE(treeEnd >= treeStart);
    const int totalTreeCount = SafeIntegerCast<int>(model.GetTreeCount());
    treeEnd = treeEnd == 0 ? totalTreeCount : Min<int>(treeEnd, totalTreeCount);
    const size_t objCount = objectsData->GetObjectCount();
    TVector<ui32> result(objCount * (treeEnd - treeStart), 0);

    if (objCount > 0) {
        const int executorThreadCount = executor ? executor->GetThreadCount() : 0;
        auto blockParams = GetBlockParams(executorThreadCount, objCount, treeStart, treeEnd);

        const ui32 subBlockSize = ui32(NModelEvaluation::FORMULA_EVALUATION_BLOCK_SIZE * 64);

        const auto applyOnBlock = [&](int blockId) {
            const int blockFirstIdx = blockParams.FirstId + blockId * blockParams.GetBlockSize();
            const int blockLastIdx = Min(blockParams.LastId, blockFirstIdx + blockParams.GetBlockSize());

            TLeafCalcerVisitor visitor(model, treeStart, treeEnd, result);

            BlockedEvaluation(model, *objectsData, (ui32)blockFirstIdx, (ui32)blockLastIdx, subBlockSize, &visitor);
        };
        if (executor) {
            executor->ExecRangeWithThrow(applyOnBlock, 0, blockParams.GetBlockCount(), TLocalExecutor::WAIT_COMPLETE);
        } else {
            applyOnBlock(0);
        }
    }
    return result;
}

TVector<ui32> CalcLeafIndexesMulti(
    const TFullModel& model,
    NCB::TObjectsDataProviderPtr objectsData,
    bool verbose,
    int treeStart,
    int treeEnd,
    int threadCount)
{
    TSetLoggingVerboseOrSilent inThisScope(verbose);

    CB_ENSURE(threadCount > 0);
    NPar::TLocalExecutor executor;
    executor.RunAdditionalThreads(threadCount - 1);
    return CalcLeafIndexesMulti(model, objectsData, treeStart, treeEnd, &executor);
}
