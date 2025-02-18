#include "onnx_helpers.h"

#include <catboost/libs/model/model_build_helper.h>
#include <catboost/libs/cat_feature/cat_feature.h>
#include <catboost/libs/helpers/exception.h>
#include <catboost/libs/options/enum_helpers.h>
#include <catboost/libs/options/json_helper.h>
#include <catboost/libs/options/loss_description.h>
#include <catboost/libs/options/multiclass_label_options.h>

#include <library/svnversion/svnversion.h>

#include <contrib/libs/onnx/onnx/common/constants.h>
#include <contrib/libs/protobuf/repeated_field.h>

#include <util/generic/array_ref.h>
#include <util/generic/mapfindptr.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/generic/xrange.h>
#include <util/string/join.h>
#include <util/system/yassert.h>

#include <numeric>


using ENanValueTreatment = TFloatFeature::ENanValueTreatment;
using EType = NCB::NOnnx::TOnnxNode::EType;


void NCB::NOnnx::InitMetadata(
    const TFullModel& model,
    const NJson::TJsonValue& userParameters,
    onnx::ModelProto* onnxModel) {

    onnxModel->set_ir_version(onnx::IR_VERSION);

    onnx::OperatorSetIdProto* opset = onnxModel->add_opset_import();
    opset->set_domain(onnx::AI_ONNX_ML_DOMAIN);
    opset->set_version(2);

    onnxModel->set_producer_name("CatBoost");
    onnxModel->set_producer_version(PROGRAM_VERSION);

    if (userParameters.Has("onnx_domain")) {
        onnxModel->set_domain(userParameters["onnx_domain"].GetStringSafe());
    }
    if (userParameters.Has("onnx_model_version")) {
        onnxModel->set_model_version(userParameters["onnx_model_version"].GetIntegerSafe());
    }
    if (userParameters.Has("onnx_doc_string")) {
        onnxModel->set_doc_string(userParameters["onnx_doc_string"].GetStringSafe());
    }

    for (const auto& [key, value] : model.ModelInfo) {
        CB_ENSURE_INTERNAL(
            key != "cat_features",
            "model metadata contains 'cat_features' key, but it is reserved for categorical features indices"
        );

        onnx::StringStringEntryProto* metadata_prop = onnxModel->add_metadata_props();
        metadata_prop->set_key(key);
        metadata_prop->set_value(value);
    }

    // If categorical features are present save cat_features to metadata_props as well
    if (!model.ObliviousTrees->CatFeatures.empty()) {
        TVector<int> catFeaturesIndices;
        for (const auto& catFeature : model.ObliviousTrees->CatFeatures) {
            catFeaturesIndices.push_back(catFeature.Position.FlatIndex);
        }

        onnx::StringStringEntryProto* catFeaturesProp = onnxModel->add_metadata_props();
        catFeaturesProp->set_key("cat_features");
        catFeaturesProp->set_value(JoinSeq(",", catFeaturesIndices));
    }
}


static bool IsClassifierModel(const TFullModel& model) {
    if (model.ObliviousTrees->ApproxDimension > 1) { // multiclass
        return true;
    }

    if (const auto* modelInfoParams = MapFindPtr(model.ModelInfo, "params")) {
        NJson::TJsonValue paramsJson = ReadTJsonValue(*modelInfoParams);

        if (paramsJson.Has("loss_function")) {
            NCatboostOptions::TLossDescription modelLossDescription;
            modelLossDescription.Load(paramsJson["loss_function"]);

            if (IsClassificationObjective(modelLossDescription.LossFunction)) {
                return true;
            }
        }
    }

    return false;
}


// only one of classLabelsInt64 or classLabelsString returned nonempty
static void GetClassLabels(
    const TFullModel& model,
    TVector<i64>* classLabelsInt64,
    TVector<TString>* classLabelsString) {

    classLabelsInt64->clear();
    classLabelsString->clear();

    if (model.ObliviousTrees->ApproxDimension > 1) {  // is multiclass?
        if (model.ModelInfo.contains("multiclass_params")) {
            const auto& multiclassParamsJsonAsString = model.ModelInfo.at("multiclass_params");
            TMulticlassLabelOptions multiclassOptions;
            multiclassOptions.Load(ReadTJsonValue(multiclassParamsJsonAsString));
            if (multiclassOptions.ClassNames.IsSet() && !multiclassOptions.ClassNames.Get().empty()) {
                *classLabelsString = multiclassOptions.ClassNames.Get();
                return;
            }
            if (multiclassOptions.ClassToLabel.IsSet() && !multiclassOptions.ClassToLabel.Get().empty()) {
                const auto& classLabelsFloat = multiclassOptions.ClassToLabel.Get();
                classLabelsInt64->assign(classLabelsFloat.begin(), classLabelsFloat.end());
                return;
            }
        }
        classLabelsInt64->resize(model.ObliviousTrees->ApproxDimension);
        std::iota(classLabelsInt64->begin(), classLabelsInt64->end(), 0);
    } else { // binclass
        if (const auto* modelInfoParams = MapFindPtr(model.ModelInfo, "params")) {
            NJson::TJsonValue paramsJson = ReadTJsonValue(*modelInfoParams);

            if (paramsJson.Has("data_processing_options")) {
                const NJson::TJsonValue& dataProcessingOptions = paramsJson["data_processing_options"];
                if (dataProcessingOptions.Has("class_names")) {
                    auto classNames = dataProcessingOptions["class_names"].GetArraySafe();
                    if (!classNames.empty()) {
                        for (const auto& token : classNames) {
                            classLabelsString->push_back(token.GetStringSafe());
                        }
                        return;
                    }
                }
            }
        }
        classLabelsInt64->push_back(0);
        classLabelsInt64->push_back(1);
    }
}


static void InitValueInfo(
    const TString& name,
    google::protobuf::int32 elemType,
    TMaybe<google::protobuf::int64> secondDim,
    onnx::ValueInfoProto* valueInfo) {

    valueInfo->set_name(name);

    onnx::TypeProto* featuresType = valueInfo->mutable_type();
    onnx::TypeProto_Tensor* tensorType = featuresType->mutable_tensor_type();
    tensorType->set_elem_type(elemType);
    onnx::TensorShapeProto* tensorShape = tensorType->mutable_shape();
    tensorShape->add_dim()->set_dim_param("N");
    if (secondDim) {
        tensorShape->add_dim()->set_dim_value(*secondDim);
    }
}


inline void SetAttributeValue(float f, onnx::AttributeProto* attribute) {
    attribute->set_type(onnx::AttributeProto_AttributeType_FLOAT);
    attribute->set_f(f);
}

inline void SetAttributeValue(google::protobuf::int64 i, onnx::AttributeProto* attribute) {
    attribute->set_type(onnx::AttributeProto_AttributeType_INT);
    attribute->set_i(i);
}

inline void SetAttributeValue(const TString& s, onnx::AttributeProto* attribute) {
    attribute->set_type(onnx::AttributeProto_AttributeType_STRING);
    attribute->set_s(s);
}

inline void SetAttributeValue(TConstArrayRef<google::protobuf::int64> ints, onnx::AttributeProto* attribute) {
    attribute->set_type(onnx::AttributeProto_AttributeType_INTS);
    for (auto i : ints) {
        attribute->add_ints(i);
    }
}

inline void SetAttributeValue(TConstArrayRef<TString> strings, onnx::AttributeProto* attribute) {
    attribute->set_type(onnx::AttributeProto_AttributeType_STRINGS);
    for (const auto& s : strings) {
        attribute->add_strings(s);
    }
}


template <class T>
static void AddAttribute(
    const TString& name,
    const T& value,
    onnx::NodeProto* node) {

    onnx::AttributeProto* attribute = node->add_attribute();
    attribute->set_name(name);
    SetAttributeValue(value, attribute);
}


static void AddClassLabelsAttribute(
    const TVector<i64>& classLabelsInt64,
    const TVector<TString>& classLabelsString,
    onnx::NodeProto* node) {

    if (!classLabelsInt64.empty()) {
        AddAttribute("classlabels_int64s", classLabelsInt64, node);
    } else {
        AddAttribute("classlabels_strings", classLabelsString, node);
    }
}

static void InitProbabilitiesOutput(
    const TString& name,
    google::protobuf::int32 mapKeysType,
    onnx::ValueInfoProto* output) {

    output->set_name(name);

    onnx::TypeProto* featuresType = output->mutable_type();
    onnx::TypeProto_Sequence* sequenceType = featuresType->mutable_sequence_type();
    onnx::TypeProto* sequenceElementType = sequenceType->mutable_elem_type();

    onnx::TypeProto_Map* mapElement = sequenceElementType->mutable_map_type();
    mapElement->set_key_type(mapKeysType);
    mapElement->mutable_value_type()->mutable_tensor_type()->set_elem_type(onnx::TensorProto_DataType_FLOAT);
}


struct TTreesAttributes {
    // TreeEnsembleClassifier only
    onnx::AttributeProto* class_ids;
    onnx::AttributeProto* class_nodeids;
    onnx::AttributeProto* class_treeids;
    onnx::AttributeProto* class_weights;

    // TreeEnsembleRegressor only
    onnx::AttributeProto* target_ids;
    onnx::AttributeProto* target_nodeids;
    onnx::AttributeProto* target_treeids;
    onnx::AttributeProto* target_weights;

    onnx::AttributeProto* nodes_falsenodeids;
    onnx::AttributeProto* nodes_featureids;
    onnx::AttributeProto* nodes_hitrates;
    onnx::AttributeProto* nodes_missing_value_tracks_true;
    onnx::AttributeProto* nodes_modes;
    onnx::AttributeProto* nodes_nodeids;
    onnx::AttributeProto* nodes_treeids;
    onnx::AttributeProto* nodes_truenodeids;
    onnx::AttributeProto* nodes_values;

public:
    TTreesAttributes(
        bool isClassifierModel,
        google::protobuf::RepeatedPtrField<onnx::AttributeProto>* treesNodeAttributes) {

#define GET_ATTR(attr, attr_type_suffix) \
        attr = treesNodeAttributes->Add(); \
        attr->set_name(#attr); \
        attr->set_type(onnx::AttributeProto_AttributeType_##attr_type_suffix);

        if (isClassifierModel) {
            GET_ATTR(class_ids, INTS);
            GET_ATTR(class_nodeids, INTS);
            GET_ATTR(class_treeids, INTS);
            GET_ATTR(class_weights, FLOATS);

            target_ids = nullptr;
            target_nodeids = nullptr;
            target_treeids = nullptr;
            target_weights = nullptr;
        } else {
            class_ids = nullptr;
            class_nodeids = nullptr;
            class_treeids = nullptr;
            class_weights = nullptr;

            GET_ATTR(target_ids, INTS);
            GET_ATTR(target_nodeids, INTS);
            GET_ATTR(target_treeids, INTS);
            GET_ATTR(target_weights, FLOATS);
        }

        GET_ATTR(nodes_falsenodeids, INTS);
        GET_ATTR(nodes_featureids, INTS);
        GET_ATTR(nodes_hitrates, FLOATS);
        GET_ATTR(nodes_missing_value_tracks_true, INTS);
        GET_ATTR(nodes_modes, STRINGS);
        GET_ATTR(nodes_nodeids, INTS);
        GET_ATTR(nodes_treeids, INTS);
        GET_ATTR(nodes_truenodeids, INTS);
        GET_ATTR(nodes_values, FLOATS);

#undef GET_ATTR
    }

    TTreesAttributes(
        const bool isClassifierModel,
        google::protobuf::RepeatedPtrField<onnx::AttributeProto>& attributes) {

#define SET_ATTR(attr, new_attr) \
        if (new_attr.name() == #attr) { \
            attr = &new_attr; \
        }
        if (isClassifierModel) {
            target_ids = nullptr;
            target_nodeids = nullptr;
            target_treeids = nullptr;
            target_weights = nullptr;
        } else {
            class_ids = nullptr;
            class_nodeids = nullptr;
            class_treeids = nullptr;
            class_weights = nullptr;
        }

        for (auto& attribute : attributes) {
            if (isClassifierModel) {
                SET_ATTR(class_ids, attribute);
                SET_ATTR(class_nodeids, attribute);
                SET_ATTR(class_treeids, attribute);
                SET_ATTR(class_weights, attribute);
            } else {
                SET_ATTR(target_ids, attribute);
                SET_ATTR(target_nodeids, attribute);
                SET_ATTR(target_treeids, attribute);
                SET_ATTR(target_weights, attribute);
            }

            SET_ATTR(nodes_falsenodeids, attribute);
            SET_ATTR(nodes_featureids, attribute);
            SET_ATTR(nodes_hitrates, attribute);
            SET_ATTR(nodes_missing_value_tracks_true, attribute);
            SET_ATTR(nodes_modes, attribute);
            SET_ATTR(nodes_nodeids, attribute);
            SET_ATTR(nodes_treeids, attribute);
            SET_ATTR(nodes_truenodeids, attribute);
            SET_ATTR(nodes_values, attribute);
        }
#undef SET_ATTR
    }
};


static void AddTree(
    const TObliviousTrees& trees,
    i64 treeIdx,
    bool isClassifierModel,
    TTreesAttributes* treesAttributes) {

    static const TString branchGTEMode = "BRANCH_GTE";
    static const TString leafMode = "LEAF";

    i64 nodeIdx = 0;

    // Process splits
    for (auto depth : xrange(trees.TreeSizes[treeIdx])) {
        const auto& split = trees.GetBinFeatures()[
            trees.TreeSplits[trees.TreeStartOffsets[treeIdx] + (trees.TreeSizes[treeIdx] - 1 - depth)]];

        int splitFlatFeatureIdx = 0;
        TString nodeMode;
        i64 missingValueTracksTrue = 0;
        float splitValue = 0.0f;

        if (split.Type == ESplitType::FloatFeature) {
            const auto& floatFeature = trees.FloatFeatures[split.FloatFeature.FloatFeature];
            splitFlatFeatureIdx = floatFeature.Position.FlatIndex;
            nodeMode = branchGTEMode;
            if (floatFeature.NanValueTreatment == TFloatFeature::ENanValueTreatment::AsTrue) {
                missingValueTracksTrue = 1;
            }
            splitValue = split.FloatFeature.Split;
        } else {
            CB_ENSURE_INTERNAL(
                false,
                "Categorical features splits are unsupported in ONNX-ML format export for now"
            );
        }

        i64 endNodeIdx = 2*nodeIdx + 1;
        for (; nodeIdx < endNodeIdx; ++nodeIdx) {
            treesAttributes->nodes_treeids->add_ints(treeIdx);
            treesAttributes->nodes_nodeids->add_ints(nodeIdx);

            treesAttributes->nodes_modes->add_strings(nodeMode);

            treesAttributes->nodes_featureids->add_ints((i64)splitFlatFeatureIdx);
            treesAttributes->nodes_values->add_floats(splitValue);
            treesAttributes->nodes_falsenodeids->add_ints(2*nodeIdx + 1);
            treesAttributes->nodes_truenodeids->add_ints(2*nodeIdx + 2);
            treesAttributes->nodes_missing_value_tracks_true->add_ints(missingValueTracksTrue);
            treesAttributes->nodes_hitrates->add_floats(1.0f);
        }
    }

    // Process leafs
    const double* leafValue = trees.LeafValues.begin() + trees.GetFirstLeafOffsets()[treeIdx];

    for (i64 endNodeIdx = 2*nodeIdx + 1; nodeIdx < endNodeIdx; ++nodeIdx) {
        treesAttributes->nodes_treeids->add_ints(treeIdx);
        treesAttributes->nodes_nodeids->add_ints(nodeIdx);

        treesAttributes->nodes_modes->add_strings(leafMode);

        // add dummy values because nodes_* must have equal length
        treesAttributes->nodes_featureids->add_ints(0);
        treesAttributes->nodes_values->add_floats(0.0f);
        treesAttributes->nodes_falsenodeids->add_ints(0);
        treesAttributes->nodes_truenodeids->add_ints(0);
        treesAttributes->nodes_missing_value_tracks_true->add_ints(0);
        treesAttributes->nodes_hitrates->add_floats(1.0f);


        if (isClassifierModel) {
            if (trees.ApproxDimension > 1) {
                for (auto approxIdx : xrange(trees.ApproxDimension)) {
                    treesAttributes->class_treeids->add_ints(treeIdx);
                    treesAttributes->class_nodeids->add_ints(nodeIdx);

                    treesAttributes->class_ids->add_ints(approxIdx);
                    treesAttributes->class_weights->add_floats((float)*leafValue);
                    ++leafValue;
                }
            } else {
                treesAttributes->class_treeids->add_ints(treeIdx);
                treesAttributes->class_nodeids->add_ints(nodeIdx);

                treesAttributes->class_ids->add_ints(1);
                treesAttributes->class_weights->add_floats((float)*leafValue);
                ++leafValue;
            }
        } else {
            Y_ASSERT(trees.ApproxDimension == 1);

            treesAttributes->target_treeids->add_ints(treeIdx);
            treesAttributes->target_nodeids->add_ints(nodeIdx);

            treesAttributes->target_ids->add_ints(0);
            treesAttributes->target_weights->add_floats((float)*leafValue);
            ++leafValue;
        }
    }
}


void NCB::NOnnx::ConvertTreeToOnnxGraph(
    const TFullModel& model,
    const TMaybe<TString>& onnxGraphName,
    onnx::GraphProto* onnxGraph) {

    const bool isClassifierModel = IsClassifierModel(model);

    const TObliviousTrees& trees = *model.ObliviousTrees;

    onnxGraph->set_name(onnxGraphName.GetOrElse("CatBoostModel"));

    InitValueInfo(
        "features",
        onnx::TensorProto_DataType_FLOAT,
        trees.GetFlatFeatureVectorExpectedSize(),
        onnxGraph->add_input());

    onnx::NodeProto* treesNode = onnxGraph->add_node();
    treesNode->set_domain(onnx::AI_ONNX_ML_DOMAIN);

    treesNode->add_input("features");
    if (isClassifierModel) {
        treesNode->set_op_type("TreeEnsembleClassifier");

        TVector<i64> classLabelsInt64;
        TVector<TString> classLabelsString;
        GetClassLabels(model, &classLabelsInt64, &classLabelsString);

        AddClassLabelsAttribute(classLabelsInt64, classLabelsString, treesNode);
        AddAttribute("post_transform", "SOFTMAX", treesNode);

        InitValueInfo(
            "label",
            classLabelsString.empty() ? onnx::TensorProto_DataType_INT64 : onnx::TensorProto_DataType_STRING,
            /*secondDim*/ Nothing(),
            onnxGraph->add_output()
        );
        treesNode->add_output("label");

        InitValueInfo(
            "probability_tensor",
            onnx::TensorProto_DataType_FLOAT,
            trees.ApproxDimension == 1 ? 2 : trees.ApproxDimension,
            onnxGraph->add_value_info()
        );
        treesNode->add_output("probability_tensor");


        onnx::NodeProto* zipMapNode = onnxGraph->add_node();
        zipMapNode->set_domain(onnx::AI_ONNX_ML_DOMAIN);
        zipMapNode->set_op_type("ZipMap");

        zipMapNode->add_input("probability_tensor");

        InitProbabilitiesOutput(
            "probabilities",
            classLabelsString.empty() ? onnx::TensorProto_DataType_INT64 : onnx::TensorProto_DataType_STRING,
            onnxGraph->add_output());

        zipMapNode->add_output("probabilities");

        AddClassLabelsAttribute(classLabelsInt64, classLabelsString, zipMapNode);
    } else {
        treesNode->set_op_type("TreeEnsembleRegressor");

        AddAttribute("post_transform", "NONE", treesNode);
        AddAttribute("n_targets", i64(1), treesNode);

        InitValueInfo(
            "predictions",
            onnx::TensorProto_DataType_FLOAT,
            /*secondDim*/ Nothing(),
            onnxGraph->add_output()
        );
        treesNode->add_output("predictions");
    }

    TTreesAttributes treesAttributes(isClassifierModel, treesNode->mutable_attribute());

    for (auto treeIdx : xrange(trees.GetTreeCount())) {
        AddTree(trees, treeIdx, isClassifierModel, &treesAttributes);
    }
}


static void ConfigureMetaInfo(const onnx::ModelProto& onnxModel, TFullModel* fullModel) {
    THashMap<TString, TString> modelInfo;

    for (int idx = 0; idx < onnxModel.metadata_props_size(); ++idx) {
        auto& property = onnxModel.metadata_props(idx);
        TString key, value;
        CB_ENSURE(property.has_key(), "Missing key in value info");
        key = property.key();
        if (property.has_value()) {
            value = property.value();
        }
        modelInfo[key] = value;
    }

    fullModel->ModelInfo = modelInfo;
}


static void PrepareTrees(
    const TTreesAttributes& treesAttributes,
    const bool isClassifierModel,
    TVector<THashMap<int, NCB::NOnnx::TOnnxNode>>* trees,
    int* approxDimension,
    TVector<TFloatFeature>* floatFeatures /* for adding nanModes and borders */
) {
    TVector<TSet<float>> floatFeatureBorders(floatFeatures->size());
    //consider all nodes
    for (auto idx = 0; idx < treesAttributes.nodes_treeids->ints_size() ;++idx) {
        NCB::NOnnx::TOnnxNode node;
        const size_t treeId = treesAttributes.nodes_treeids->ints(idx);
        const int nodeId = treesAttributes.nodes_nodeids->ints(idx);
        node.FalseNodeId = treesAttributes.nodes_falsenodeids->ints(idx);
        node.TrueNodeId = treesAttributes.nodes_truenodeids->ints(idx);

        if (treesAttributes.nodes_modes->strings(idx) == "LEAF") {
            node.Type = EType::Leaf;
        } else {
            node.Type = EType::Inner;

            TModelSplit split;
            split.Type = ESplitType::FloatFeature;
            split.FloatFeature.FloatFeature = treesAttributes.nodes_featureids->ints(idx);
            split.FloatFeature.Split = treesAttributes.nodes_values->floats(idx);

            //update floatFeatures
            if (treesAttributes.nodes_missing_value_tracks_true->ints(idx) == 1) {
                (*floatFeatures)[split.FloatFeature.FloatFeature].NanValueTreatment =
                ENanValueTreatment::AsTrue;
            }
            floatFeatureBorders[split.FloatFeature.FloatFeature].insert(split.FloatFeature.Split);

            node.SplitCondition = split;
        }

        //add node to tree
        if (treeId >= trees->size()) {
            trees->resize(treeId + 1);
        }
        (*trees)[treeId][nodeId] = node;
    }

    //to set borders to floatfeatures
    for (auto floatFeatureIdx : xrange(floatFeatures->size())) {
        (*floatFeatures)[floatFeatureIdx].Borders.assign(
            floatFeatureBorders[floatFeatureIdx].begin(),
            floatFeatureBorders[floatFeatureIdx].end());
    }

    //consider leaves
    auto treatLeafNode = [trees](
        onnx::AttributeProto* treeIds,
        onnx::AttributeProto* nodeIds,
        onnx::AttributeProto* values
    ) {
        for (auto idx = 0 ; idx < treeIds->ints_size(); ++idx) {
            const size_t treeId = treeIds->ints(idx);
            const int nodeId = nodeIds->ints(idx);
            const double value = values->floats(idx);
            CB_ENSURE(treeId < trees->size(), "Invalid class_nodeId " << treeId);
            (*trees)[treeId][nodeId].Values.emplace_back(value);
        }
    };
    if (isClassifierModel) {
        treatLeafNode(treesAttributes.class_treeids, treesAttributes.class_nodeids, treesAttributes.class_weights);
        //setup approxDimension
        const auto anyIdxNodeIdLeaf = treesAttributes.class_nodeids->ints(0);
        *approxDimension = static_cast<int>((*trees)[0][anyIdxNodeIdLeaf].Values.size());
    } else {
        treatLeafNode(treesAttributes.target_treeids, treesAttributes.target_nodeids, treesAttributes.target_weights);
    }
}


static THolder<TNonSymmetricTreeNode> BuildNonSymmetricTree(
    const THashMap<int, NCB::NOnnx::TOnnxNode>& tree,
    const int nodeId
) {
    THolder<TNonSymmetricTreeNode> head = MakeHolder<TNonSymmetricTreeNode>();
    const auto node = tree.at(nodeId);

    switch (node.Type) {
        case NCB::NOnnx::TOnnxNode::EType::Leaf: {
            if (node.Values.size() == 1) {
                head->Value = node.Values[0];
            } else {
                head->Value = node.Values;
            }
            return head;
        }
        case NCB::NOnnx::TOnnxNode::EType::Inner: {
            head->Value = TNonSymmetricTreeNode::TEmptyValue();
            head->SplitCondition = node.SplitCondition;
            CB_ENSURE(tree.contains(node.FalseNodeId), "unexpected false node id");
            CB_ENSURE(tree.contains(node.TrueNodeId), "unexpected true node id");
            head->Left = BuildNonSymmetricTree(tree, node.FalseNodeId);
            head->Right = BuildNonSymmetricTree(tree, node.TrueNodeId);
            return head;
        }
    }
}


static int GetFloatFeatureCount(const onnx::GraphProto& onnxGraph) {
    const auto valueInfo = onnxGraph.input()[0];
    CB_ENSURE(valueInfo.type().tensor_type().shape().dimSize() == 2,
        "Dimemsion must have format 'FloatTensorType'[0, featuresCount]");
    const int featuresCount = valueInfo.type().tensor_type().shape().dim(1).dim_value();
    CB_ENSURE(featuresCount >= 1, "Count of features must be greater than one");
    return featuresCount;
}

static void ConfigureSymmetricTrees(const onnx::GraphProto& onnxGraph, TFullModel* fullModel) {

    const auto& nodes = onnxGraph.node();
    //to treat first node
    const bool isClassifierModel = (nodes[0].op_type() == "TreeEnsembleClassifier");
    CB_ENSURE((nodes[0].op_type() == "TreeEnsembleClassifier") ||
              (nodes[0].op_type() == "TreeEnsembleRegressor"), "unexpected type task " << nodes[0].op_type());

    auto attributes = nodes[0].attribute();
    TTreesAttributes treesAttributes(isClassifierModel, attributes);

    TVector<TFloatFeature> floatFeatures;
    const int featuresCount = GetFloatFeatureCount(onnxGraph);
    //init floatFeatures
    for (auto idx = 0; idx < featuresCount; ++idx) {
        floatFeatures.emplace_back(TFloatFeature(false, idx, idx, TVector<float>(0) /*in PrepareTrees it will be update*/,""));
    }

    TVector<THashMap<int, NCB::NOnnx::TOnnxNode>> trees;
    int approxDimension = 1;
    PrepareTrees(treesAttributes, isClassifierModel, &trees, &approxDimension, &floatFeatures);

    TNonSymmetricTreeModelBuilder treeBuilder(floatFeatures, TVector<TCatFeature>(0), approxDimension);

    for (const auto& tree : trees) {
        treeBuilder.AddTree(BuildNonSymmetricTree(tree, 0));
    }

    treeBuilder.Build(fullModel->ObliviousTrees.GetMutable());

    fullModel->UpdateDynamicData();
}



void NCB::NOnnx::ConvertOnnxToCatboostModel(const onnx::ModelProto& onnxModel, TFullModel* fullModel) {
    //InitMetaData
    ConfigureMetaInfo(onnxModel, fullModel);

    onnx::GraphProto onnxGraph = onnxModel.graph();
    ConfigureSymmetricTrees(onnxGraph, fullModel);
}
