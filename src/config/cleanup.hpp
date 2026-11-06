#pragma once

namespace cc::config {

/// What the pruning removed.
struct CleanupResult {
    /// The number of the files that were removed.
    int messages = 0;
    /// The number of the files that could not be removed.
    int errors = 0;
};

/// Removes the files that are older than `period_days`.
///
/// The reference prunes the transcripts of the sessions, the plans, the
/// copies of the files the sessions changed and the logs of the debugger;
/// it does it in the background when the session starts. A period of zero
/// days removes everything that was written before now.
///
/// The number of days is the one of the `cleanupPeriodDays` setting; the
/// reference keeps thirty days when the setting is not there.
CleanupResult prune_old_files(int period_days);

} // namespace cc::config
