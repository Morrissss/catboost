#pragma once

#include "fwd.h"

#include "features.h"

#include <util/generic/array_ref.h>
#include <util/generic/maybe.h>
#include <util/generic/ptr.h>

namespace NCB {  // split due to CUDA-compiler inability to parse nested namespace definitions
    namespace NModelEvaluation {
        class TFeatureLayout {
        public:
            TMaybe<TVector<ui32>> FloatFeatureIndexes;
            TMaybe<TVector<ui32>> CatFeatureIndexes;
            TMaybe<TVector<ui32>> FlatIndexes;
        public:
            inline TFeaturePosition AdjustFeature(const TFloatFeature& feature) const {
                TFeaturePosition position = feature.Position;
                if (FloatFeatureIndexes.Defined()) {
                    position.Index = FloatFeatureIndexes->at(position.Index);
                }
                if (FlatIndexes.Defined()) {
                    position.FlatIndex = FlatIndexes->at(position.FlatIndex);
                }
                return position;
            }

            inline TFeaturePosition AdjustFeature(const TCatFeature& feature) const {
                TFeaturePosition position = feature.Position;
                if (CatFeatureIndexes.Defined()) {
                    position.Index = CatFeatureIndexes->at(position.Index);
                }
                if (FlatIndexes.Defined()) {
                    position.FlatIndex = FlatIndexes->at(position.FlatIndex);
                }
                return position;
            }
        };

        class IModelEvaluator {
        public:
            virtual ~IModelEvaluator() = default;

            virtual void SetPredictionType(EPredictionType type) = 0;
            virtual EPredictionType GetPredictionType() const = 0;

            virtual TModelEvaluatorPtr Clone() const = 0;

            virtual i32 GetApproxDimension() const = 0;
            virtual size_t GetTreeCount() const = 0;

            virtual void SetFeatureLayout(const TFeatureLayout& featureLayout) = 0;

            virtual void SetProperty(const TStringBuf propName, const TStringBuf propValue) = 0;

            // TODO(kirillovs): maybe write special class for results (on gpu it'll hold floats in possibly managed memory)
            TVector<double> CreateVectorForPredictions(size_t docCount) const {
                switch (GetPredictionType())
                {
                case EPredictionType::RawFormulaVal:
                case EPredictionType::Probability:
                    return TVector<double>(docCount * GetApproxDimension());
                case EPredictionType::Class:
                    return TVector<double>(docCount);
                default:
                    Y_UNREACHABLE();
                }
            }

            virtual void CalcFlatTransposed(
                TConstArrayRef<TConstArrayRef<float>> transposedFeatures,
                size_t treeStart,
                size_t treeEnd,
                TArrayRef<double> results,
                const TFeatureLayout* featureInfo = nullptr
            ) const = 0;

            void CalcFlatTransposed(
                TConstArrayRef<TConstArrayRef<float>> transposedFeatures,
                TArrayRef<double> results,
                const TFeatureLayout* featureInfo = nullptr
            ) const {
                CalcFlatTransposed(transposedFeatures, 0, GetTreeCount(), results, featureInfo);
            }

            void CalcFlatTransposed(
                TConstArrayRef<TVector<float>> transposedFeatures,
                TArrayRef<double> results,
                const TFeatureLayout* featureInfo = nullptr
            ) const {
                TVector<TConstArrayRef<float>> featureRefs{transposedFeatures.begin(), transposedFeatures.end()};
                CalcFlatTransposed(featureRefs, 0, GetTreeCount(), results, featureInfo);
            }

            virtual void CalcFlat(
                TConstArrayRef<TConstArrayRef<float>> features,
                size_t treeStart,
                size_t treeEnd,
                TArrayRef<double> results,
                const TFeatureLayout* featureInfo = nullptr
            ) const = 0;

            void CalcFlat(
                TConstArrayRef<TConstArrayRef<float>> features,
                TArrayRef<double> results,
                const TFeatureLayout* featureInfo = nullptr
            ) const {
                CalcFlat(features, 0, GetTreeCount(), results, featureInfo);
            }

            void CalcFlat(
                TConstArrayRef<TVector<float>> features,
                TArrayRef<double> results,
                const TFeatureLayout* featureInfo = nullptr
            ) const {
                TVector<TConstArrayRef<float>> featureRefs{features.begin(), features.end()};
                CalcFlat(featureRefs, 0, GetTreeCount(), results, featureInfo);
            }

            virtual void CalcFlatSingle(
                TConstArrayRef<float> features,
                size_t treeStart,
                size_t treeEnd,
                TArrayRef<double> results,
                const TFeatureLayout* featureInfo = nullptr
            ) const = 0;

            void CalcFlatSingle(
                TConstArrayRef<float> features,
                TArrayRef<double> results,
                const TFeatureLayout* featureInfo = nullptr
            ) const {
                CalcFlatSingle(features, 0, GetTreeCount(), results, featureInfo);
            }

            virtual void Calc(
                TConstArrayRef<TConstArrayRef<float>> floatFeatures,
                TConstArrayRef<TConstArrayRef<int>> catFeatures,
                size_t treeStart,
                size_t treeEnd,
                TArrayRef<double> results,
                const TFeatureLayout* featureInfo = nullptr
            ) const = 0;

            virtual void Calc(
                TConstArrayRef<TConstArrayRef<float>> floatFeatures,
                TConstArrayRef<TConstArrayRef<TStringBuf>> catFeatures,
                size_t treeStart,
                size_t treeEnd,
                TArrayRef<double> results,
                const TFeatureLayout* featureInfo = nullptr
            ) const = 0;

            template <typename TCatFeatureType>
            void Calc(
                TConstArrayRef<TConstArrayRef<float>> floatFeatures,
                TConstArrayRef<TConstArrayRef<TCatFeatureType>> catFeatures,
                TArrayRef<double> results,
                const TFeatureLayout* featureInfo = nullptr
            ) const {
                Calc(floatFeatures, catFeatures, 0, GetTreeCount(), results, featureInfo);
            }

            void Calc(
                TConstArrayRef<TVector<float>> floatFeatures,
                TConstArrayRef<TVector<TString>> catFeatures,
                TArrayRef<double> results,
                const TFeatureLayout* featureInfo = nullptr
            ) const {
                TVector<TConstArrayRef<float>> floatRefs(floatFeatures.begin(), floatFeatures.end());
                TVector<TConstArrayRef<TStringBuf>> catFeatureStringRefs(Reserve(catFeatures.size()));
                for (const auto& objCatFeature : catFeatures) {
                    catFeatureStringRefs.emplace_back(TVector<TStringBuf>{objCatFeature.begin(), objCatFeature.end()});
                }
                Calc<TStringBuf>(floatRefs, catFeatureStringRefs, results, featureInfo);
            }

            virtual void Calc(
                const IQuantizedData* quantizedFeatures,
                size_t treeStart,
                size_t treeEnd,
                TArrayRef<double> results
            ) const = 0;

            virtual void CalcLeafIndexesSingle(
                TConstArrayRef<float> floatFeatures,
                TConstArrayRef<TStringBuf> catFeatures,
                size_t treeStart,
                size_t treeEnd,
                TArrayRef<TCalcerIndexType> indexes,
                const TFeatureLayout* featureInfo = nullptr
            ) const = 0;

            virtual void CalcLeafIndexes(
                TConstArrayRef<TConstArrayRef<float>> floatFeatures,
                TConstArrayRef<TConstArrayRef<TStringBuf>> catFeatures,
                size_t treeStart,
                size_t treeEnd,
                TArrayRef<TCalcerIndexType> indexes,
                const TFeatureLayout* featureInfo = nullptr
            ) const = 0;

            virtual void CalcLeafIndexes(
                const IQuantizedData* quantizedFeatures,
                size_t treeStart,
                size_t treeEnd,
                TArrayRef<TCalcerIndexType> indexes
            ) const = 0;
        };

        TModelEvaluatorPtr CreateCpuEvaluator(const TFullModel& model);

        bool CudaEvaluationPossible(const TFullModel& model);
        TModelEvaluatorPtr CreateGpuEvaluator(const TFullModel& model);
    }
}
