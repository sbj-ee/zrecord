#pragma once

#include <QStringList>

namespace zrecord {

// What the updater needs from the application window (MainWindow), so the
// update flow is testable without the whole app.
class UpdateHost
{
public:
    virtual ~UpdateHost() = default;
    // True when the open document has changes that are not saved.
    virtual bool hasUnsavedChanges() const = 0;
    // Command-line arguments that reopen what is open now (the document path).
    virtual QStringList restartArguments() const = 0;
    // Closes the window the way the user would (this may ask to save, and the
    // user may cancel). True when it closed.
    virtual bool closeForRestart() = 0;
};

} // namespace zrecord
