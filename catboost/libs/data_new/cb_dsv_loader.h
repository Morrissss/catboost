#pragma once

#include "loader.h"

#include <catboost/libs/column_description/column.h>
#include <catboost/libs/data_util/line_data_reader.h>
#include <catboost/libs/helpers/exception.h>

#include <util/generic/ptr.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/generic/ylimits.h>
#include <util/system/types.h>


namespace NCB {

    // expose the declaration to allow to derive from it in other modules
    class TCBDsvDataLoader : public IRawObjectsOrderDatasetLoader
                           , protected TAsyncProcDataLoaderBase<TString>
    {
    public:
        using TBase = TAsyncProcDataLoaderBase<TString>;

    protected:
        decltype(auto) GetReadFunc() {
            return [this](TString* line) -> bool {
                return LineDataReader->ReadLine(line);
            };
        }

        decltype(auto) GetReadBaselineFunc() {
            return [this](TString *line) -> bool {
                return BaselineReader.ReadLine(line);
            };
        }

    public:
        explicit TCBDsvDataLoader(TDatasetLoaderPullArgs&& args);

        explicit TCBDsvDataLoader(TLineDataLoaderPushArgs&& args);

        ~TCBDsvDataLoader() {
            AsyncRowProcessor.FinishAsyncProcessing();
        }

        void Do(IRawObjectsOrderDataVisitor* visitor) override {
            TBase::Do(GetReadFunc(), GetReadBaselineFunc(), visitor);
        }

        bool DoBlock(IRawObjectsOrderDataVisitor* visitor) override {
            return TBase::DoBlock(GetReadFunc(), GetReadBaselineFunc(), visitor);
        }

        TVector<TColumn> CreateColumnsDescription(ui32 columnsCount);

        ui32 GetObjectCount() override {
            const ui64 dataLineCount = LineDataReader->GetDataLineCount();
            CB_ENSURE(
                dataLineCount <= Max<ui32>(), "CatBoost does not support datasets with more than "
                << Max<ui32>() << " objects"
            );
            // cast is safe - was checked above
            return (ui32)dataLineCount;
        }

        void StartBuilder(
            bool inBlock,
            ui32 objectCount,
            ui32 offset,
            IRawObjectsOrderDataVisitor* visitor
        ) override;

        void ProcessBlock(IRawObjectsOrderDataVisitor* visitor) override;

    protected:
        TVector<bool> FeatureIgnored; // init in process
        char FieldDelimiter;
        THolder<NCB::ILineDataReader> LineDataReader;
        TBaselineReader BaselineReader;
    };

}
