#pragma once

#include "exclusive_feature_bundling.h"
#include "packed_binary_features.h"

#include <catboost/libs/options/enums.h>

#include <library/binsaver/bin_saver.h>

#include <util/generic/maybe.h>
#include <util/generic/vector.h>
#include <util/system/types.h>


namespace NCB {

    struct TFeaturesGroupPart {
        EFeatureType FeatureType;
        ui32 FeatureIdx; // per type

    public:
        explicit TFeaturesGroupPart(EFeatureType featureType = EFeatureType::Float, ui32 featureIndex = 0)
            : FeatureType(featureType)
            , FeatureIdx(featureIndex)
        {}

        bool operator==(const TFeaturesGroupPart& rhs) const {
            return (FeatureType == rhs.FeatureType) && (FeatureIdx == rhs.FeatureIdx);
        }

        SAVELOAD(FeatureType, FeatureIdx);
    };

    struct TFeaturesGroup {
        TVector<TFeaturesGroupPart> Parts;

    public:
        inline bool operator==(const TFeaturesGroup& rhs) const {
            return Parts == rhs.Parts;
        }

        inline ui32 GetSizeInBytes() const {
            return Parts.size();
        }

        inline void Add(const TFeaturesGroupPart& part) {
            Parts.emplace_back(part);
        }

        SAVELOAD(Parts);
    };

    struct TFeaturesGroupIndex {
        ui32 GroupIdx;
        ui32 InGroupIdx;
    };

    struct TFeaturesGroupingOptions {
        ui32 MaxFeaturesPerBundle = 4;
    };

    TVector<TFeaturesGroup> CreateFeatureGroups(
        const TFeaturesLayout& featuresLayout,
        const TQuantizedFeaturesInfo& quantizedFeaturesInfo,
        const TVector<TMaybe<TExclusiveBundleIndex>>& flatFeatureIndexToBundlePart,
        const TVector<TMaybe<TPackedBinaryIndex>>& flatFeatureIndexToPackedBinaryIndex,
        const TFeaturesGroupingOptions& options = {});
}
