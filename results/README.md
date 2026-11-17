# Recorded results

The paper reference is `present/candidate`, the mixed-strength DP tree with nine critical leaf repairs, compared with `present/stock`. `present/unrepaired` supplies the intermediate state and reconstruction command. `present/planning` contains stock-seeded grouping, target and calibration inputs. All data are preserved copies; new analyses go to `runs/`.

Main results: weighted spread reduction 16.4217764%; window-average FVR total power 20.896761585 to 21.643542569 mW (+3.573668%); full-run aggregate glitching power 30.050 to 22.920 uW (-23.7271%); FVR peak |t| 2.895734 to 2.581247 at 512 traces/class. Both full-sample null controls remain below 4.5; prefixes cross and remain recorded. Lower peak t is descriptive and does not establish replicated security improvement.

Total cell area 40018.902 to 40063.590 um² (+0.1117%). Clock-cell area including dummy loads 760.228 to 804.916 um² (+5.88%). The historical driver-only stock clock area is 701.974 um²; do not mix accounting conventions. Glitch power is averaged over the full run; total power is averaged over 80 ns measurement windows. They cannot be subtracted as if measured over the same interval.

`comparators/` is a separate exploratory tuned-Triton/iCTS comparison. It is not part of the PRESENT paper campaign. Its independent-PrimeTime provenance correction is retained: failed Nov. 16 checkout did not constitute a fresh timing pass. Three earlier power runs completed; additional runs remain license-blocked; tested iCTS points did not satisfy hold timing. See its ASSESSMENT and status files. Historical summaries are evidence snapshots, not claims of completed SOTA signoff.
