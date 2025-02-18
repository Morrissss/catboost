#include "feature_names_converter.h"

#include <catboost/libs/column_description/cd_parser.h>
#include <catboost/libs/helpers/exception.h>

#include <util/string/type.h>

static bool TryParseRange(const TString& ignoredFeatureDescription, ui32& left, ui32& right) {
    return StringSplitter(ignoredFeatureDescription).Split('-').TryCollectInto(&left, &right);
}

static bool IsNumberOrRange(const TString& ignoredFeatureDescription) {
    ui32 left, right;
    return IsNumber(ignoredFeatureDescription) || TryParseRange(ignoredFeatureDescription, left, right);
}

static void ConvertStringIndicesIntoIntegerIndices(NJson::TJsonValue* ignoredFeaturesJson) {
    NJson::TJsonValue ignoredFeaturesIndicesJson(NJson::JSON_ARRAY);
    for (NJson::TJsonValue ignoredFeature : ignoredFeaturesJson->GetArray()) {
        if (IsNumber(ignoredFeature.GetString())) {
            ignoredFeaturesIndicesJson.AppendValue(FromString<ui32>(ignoredFeature.GetString()));
        } else {
            ui32 left, right;
            CB_ENSURE_INTERNAL(TryParseRange(ignoredFeature.GetString(), left, right), "Bad feature range");
            for (ui32 index = left; index <= right; index++) {
                ignoredFeaturesIndicesJson.AppendValue(index);
            }
        }
    }
    ignoredFeaturesJson->Swap(ignoredFeaturesIndicesJson);
}

static void ConvertNamesIntoIndices(const TMap<TString, ui32>& indicesFromNames, NJson::TJsonValue* ignoredFeaturesJson) {
    NJson::TJsonValue ignoredFeaturesIndicesJson(NJson::JSON_ARRAY);
    for (NJson::TJsonValue ignoredFeature : ignoredFeaturesJson->GetArray()) {
        CB_ENSURE(indicesFromNames.contains(ignoredFeature.GetString()), "There is no feature with name '" + ignoredFeature.GetString() + "' in dataset");
        ignoredFeaturesIndicesJson.AppendValue(indicesFromNames.at(ignoredFeature.GetString()));
    }
    ignoredFeaturesJson->Swap(ignoredFeaturesIndicesJson);
}

static bool IsNumbersOrRangesConvert(const NJson::TJsonValue& ignoredFeaturesJson) {
    return AllOf(ignoredFeaturesJson.GetArray(), [](const NJson::TJsonValue& ignoredFeature) {
        return IsNumberOrRange(ignoredFeature.GetString());
    });
}

static bool IsNumbersConvert(const NJson::TJsonValue& ignoredFeaturesJson) {
    return AllOf(ignoredFeaturesJson.GetArray(), [](const NJson::TJsonValue& ignoredFeature) {
        return IsNumber(ignoredFeature.GetString());
    });
}

static void ConvertStringsArrayIntoIndicesArray(const NCatboostOptions::TPoolLoadParams& poolLoadParams, NJson::TJsonValue* ignoredFeaturesJson) {
    if (IsNumbersOrRangesConvert(*ignoredFeaturesJson)) {
        ConvertStringIndicesIntoIntegerIndices(ignoredFeaturesJson);
    } else {
        CB_ENSURE(!poolLoadParams.LearnSetPath.Scheme.Contains("quantized") || poolLoadParams.DsvPoolFormatParams.CdFilePath.Inited(),
                "quatized pool without CD file doesn't support ignoring features by names");
        const TVector<TColumn> columns = ReadCD(poolLoadParams.DsvPoolFormatParams.CdFilePath, TCdParserDefaults(EColumn::Num));
        ui32 currentId = 0;
        TMap<TString, ui32> indicesFromNames;
        for (const auto& column : columns) {
            if (IsFactorColumn(column.Type)) {
                if (!column.Id.empty()) {
                    indicesFromNames[column.Id] = currentId;
                }
                currentId++;
            }
        }
        ConvertNamesIntoIndices(indicesFromNames, ignoredFeaturesJson);
    }
}

static void ConvertStringsArrayIntoIndicesArray(const NCB::TDataMetaInfo& metaInfo, NJson::TJsonValue* ignoredFeaturesJson) {
    if (IsNumbersConvert(*ignoredFeaturesJson)) {
        ConvertStringIndicesIntoIntegerIndices(ignoredFeaturesJson);
    } else {
        TMap<TString, ui32> indicesFromNames;
        ui32 currentId = 0;
        auto featuresArray = metaInfo.FeaturesLayout.Get()->GetExternalFeaturesMetaInfo();
        for (const auto& feature : featuresArray) {
            if (!feature.Name.empty()) {
                indicesFromNames[feature.Name] = currentId;
            }
            currentId++;
        }
        ConvertNamesIntoIndices(indicesFromNames, ignoredFeaturesJson);
    }
}

void ConvertIgnoredFeaturesFromStringToIndices(const NCatboostOptions::TPoolLoadParams& poolLoadParams, NJson::TJsonValue* catBoostJsonOptions) {
    if (catBoostJsonOptions->Has("ignored_features")) {
        ConvertStringsArrayIntoIndicesArray(poolLoadParams, &(*catBoostJsonOptions)["ignored_features"]);
    }
}

void ConvertIgnoredFeaturesFromStringToIndices(const NCB::TDataMetaInfo& metaInfo, NJson::TJsonValue* catBoostJsonOptions) {
    if (catBoostJsonOptions->Has("ignored_features")) {
        ConvertStringsArrayIntoIndicesArray(metaInfo, &(*catBoostJsonOptions)["ignored_features"]);
    }
}
