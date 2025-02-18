#include "model_exporter.h"

#include "coreml_helpers.h"
#include "cpp_exporter.h"
#include "json_model_helpers.h"
#include "onnx_helpers.h"
#include "pmml_helpers.h"
#include "python_exporter.h"

#include <catboost/libs/options/output_file_options.h>

#include <library/json/json_reader.h>

#include <contrib/libs/coreml/TreeEnsemble.pb.h>
#include <contrib/libs/coreml/Model.pb.h>

#include <util/string/builder.h>

namespace NCB {
    ICatboostModelExporter* CreateCatboostModelExporter(const TString& modelFile, const EModelType format, const TString& userParametersJson, bool addFileFormatExtension) {
        switch (format) {
            case EModelType::Cpp:
                return new TCatboostModelToCppConverter(modelFile, addFileFormatExtension, userParametersJson);
            case EModelType::Python:
                return new TCatboostModelToPythonConverter(modelFile, addFileFormatExtension, userParametersJson);
            default:
                TStringBuilder err;
                err << "CreateCatboostModelExporter doesn't support " << format << ".";
                CB_ENSURE(false, err);
        }
    }

    void OutputModelCoreML(
        const TFullModel& model,
        const TString& modelFile,
        const NJson::TJsonValue& userParameters,
        const THashMap<ui32, TString>* catFeaturesHashToString) {

        CoreML::Specification::Model treeModel;
        treeModel.set_specificationversion(1);

        auto regressor = treeModel.mutable_treeensembleregressor();
        auto ensemble = regressor->mutable_treeensemble();

        NCB::NCoreML::TPerTypeFeatureIdxToInputIndex perTypeFeatureIdxToInputIndex;
        bool createPipelineModel = model.HasCategoricalFeatures();

        TString data;
        if (createPipelineModel) {
            CoreML::Specification::Model pipelineModel;
            pipelineModel.set_specificationversion(1);

            auto* container = pipelineModel.mutable_pipeline()->mutable_models();
            NCB::NCoreML::ConfigureCategoricalMappings(model, catFeaturesHashToString, container);

            auto* contained = container->Add();
            auto treeDescription = treeModel.mutable_description();
            NCB::NCoreML::ConfigureTreeModelIO(model, userParameters, regressor, treeDescription, &perTypeFeatureIdxToInputIndex);

            NCB::NCoreML::ConfigureTrees(model, perTypeFeatureIdxToInputIndex, ensemble);

            *contained = treeModel;

            auto pipelineDescription = pipelineModel.mutable_description();
            NCB::NCoreML::ConfigureMetadata(model, userParameters, pipelineDescription);
            NCB::NCoreML::ConfigurePipelineModelIO(model, pipelineDescription);

            pipelineModel.SerializeToString(&data);
        } else {
            auto description = treeModel.mutable_description();
            NCB::NCoreML::ConfigureMetadata(model, userParameters, description);
            NCB::NCoreML::ConfigureTreeModelIO(model, userParameters, regressor, description, &perTypeFeatureIdxToInputIndex);

            NCB::NCoreML::ConfigureTrees(model, perTypeFeatureIdxToInputIndex, ensemble);

            treeModel.SerializeToString(&data);
        }

        TOFStream out(modelFile);
        out.Write(data);
    }

    void OutputModelOnnx(
        const TFullModel& model,
        const TString& modelFile,
        const NJson::TJsonValue& userParameters) {

        /* TODO(akhropov): the problem with OneHotFeatures is that raw 'float' values
        * could be interpreted as nans so that equality comparison won't work for such splits
        */
        CB_ENSURE(
            !model.HasCategoricalFeatures(),
            "ONNX-ML format export does yet not support categorical features"
        );

        onnx::ModelProto outModel;

        NCB::NOnnx::InitMetadata(model, userParameters, &outModel);

        TMaybe<TString> graphName;
        if (userParameters.Has("onnx_graph_name")) {
            graphName = userParameters["onnx_graph_name"].GetStringSafe();
        }

        NCB::NOnnx::ConvertTreeToOnnxGraph(model, graphName, outModel.mutable_graph());

        TString data;
        outModel.SerializeToString(&data);

        TOFStream out(modelFile);
        out.Write(data);
    }

    void ExportModel(
        const TFullModel& model,
        const TString& modelFile,
        const EModelType format,
        const TString& userParametersJson,
        bool addFileFormatExtension,
        const TVector<TString>* featureId,
        const THashMap<ui32, TString>* catFeaturesHashToString
    ) {

        //TODO(eermishkina): support non symmetric trees
        CB_ENSURE(model.IsOblivious() || format == EModelType::CatboostBinary, "Can save non symmetric trees only in cbm format");
        const auto modelFileName = NCatboostOptions::AddExtension(format, modelFile, addFileFormatExtension);
        switch (format) {
            case EModelType::CatboostBinary:
                CB_ENSURE(
                    userParametersJson.empty(),
                    "JSON user params for CatBoost model export are not supported"
                );
                OutputModel(model, modelFileName);
                break;
            case EModelType::AppleCoreML:
                {
                    TStringInput is(userParametersJson);
                    NJson::TJsonValue params;
                    NJson::ReadJsonTree(&is, &params);

                    OutputModelCoreML(model, modelFileName, params, catFeaturesHashToString);
                }
                break;
            case EModelType::Json:
                {
                    CB_ENSURE(
                        userParametersJson.empty(),
                        "JSON user params for CatBoost model export are not supported"
                    );

                    OutputModelJson(model, modelFileName, featureId, catFeaturesHashToString);
                }
                break;
            case EModelType::Onnx:
                {
                    TStringInput is(userParametersJson);
                    NJson::TJsonValue params;
                    NJson::ReadJsonTree(&is, &params);

                    OutputModelOnnx(model, modelFileName, params);
                }
                break;
            case EModelType::Pmml:
                {
                    TStringInput is(userParametersJson);
                    NJson::TJsonValue params;
                    NJson::ReadJsonTree(&is, &params);

                    NCB::NPmml::OutputModel(model, modelFileName, params, catFeaturesHashToString);
                }
                break;
            default:
                TIntrusivePtr<NCB::ICatboostModelExporter> modelExporter
                    = NCB::CreateCatboostModelExporter(
                        modelFile,
                        format,
                        userParametersJson,
                        addFileFormatExtension
                    );
                if (!modelExporter) {
                    TStringBuilder err;
                    err << "Export to " << format << " format is not supported";
                    CB_ENSURE(false, err.c_str());
                }
                modelExporter->Write(model, catFeaturesHashToString);
                break;
        }
    }
}
