#pragma once

#include <QString>

namespace VideoEditor {

// Maps an engine/manager error code to an actionable Chinese message. Unknown
// codes fall back to the engine detail when one was supplied, then to a generic
// retry message.
QString removalErrorMessage(const QString &code, const QString &detail);

} // namespace VideoEditor
