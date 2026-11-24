// Media Foundation for the whole host: started once, on first use, and never shut down while the process runs.
#pragma once

#include <string>

namespace kvhost {

// Starts Media Foundation (MFStartup, MFSTARTUP_LITE) the first time it is called and keeps it running for the life
// of the process; later calls only report that it is up. Thread-safe. False (err) when MFStartup failed; a later
// call tries again. Every MF user in the host goes through this, and nothing in the host calls MFShutdown: see
// mf_platform.cpp for why.
bool ensureMediaFoundation(std::string* err = nullptr);

} // namespace kvhost
