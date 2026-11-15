#pragma once

#include <QString>

#include "Project.h"
#include "RemovalCache.h"
#include "RemovalCacheKey.h"
#include "ModelRegistry.h"

namespace VideoEditor {

// Export preflight for AI removal layers. It is fail-closed: a missing,
// expired or mismatched repair cache never reaches the renderer, so export can
// never silently encode the original frame in place of a required repair.
//
// The result types live in core/Removal.h so media (ExportPipeline) can use
// them without depending on the AI library.
class RemovalPreflight {
public:
    // Shared derivation of every field that changes generated pixels. Task 8
    // must use this (or an identical construction) so the key written by
    // generation matches the key checked here.
    static RemovalKeyInput keyInputFor(const Project &project, const Clip &clip,
                                       const RemovalLayer &layer,
                                       const ModelRegistry &models);

    // modelsAlreadyVerified skips featureEnabled()'s model-file hashing when
    // AiFeatureGate::enabled() already proved the files for this session.
    static RemovalPreflightResult check(const Project &project,
                                        const RemovalCache &cache,
                                        const ModelRegistry &models,
                                        bool modelsAlreadyVerified = false);

    // Null-registry path: the model manifest could not be loaded, so there is
    // no registry to inspect. Still honours track/clip/layer enablement and
    // source-range overlap; returns ok when no enabled removal layer is
    // actually in effect.
    static RemovalPreflightResult checkWithoutModels(const Project &project,
                                                     const QString &reason);
};

} // namespace VideoEditor
