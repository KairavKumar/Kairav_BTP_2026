# Verification evidence

`import_manifest.json` records every imported file's source and SHA-256. `build/source_hash_verification.json` matches all 17 hashes from the preserved measured build. CostAwareTreeBuilder.cpp comes from the historical universal_power_20261031/CostAwareTreeBuilder.before.cpp, not the later working version.

REPLAY_CHECKS.json, recomputed_TVLA.json and replayed_LP.json record portable-entry-point checks. Native reloading and reconstruction with the recorded nine-leaf repair reproduced cost, setup, hold and area using the preserved executable. A clean dependency rebuild was not executed.
