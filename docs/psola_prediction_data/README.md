# Prediction telemetry data

Generated from the COM11 B4D.12 recorder-on capture at 44,100 Hz, 64-frame
blocks: one-minute vocal replay plus two ten-second transition fixtures. The
recorder retained every MC+2/MC+3+ event and every eighth MC+1 event, up to
512 MC+1 events per case. No event was dropped in these windows.

The raw serial capture remains locally at
`artifacts/alpha01d/psola_prediction_raw_final.txt`; the paired recorder-off
capture is `artifacts/alpha01d/psola_prediction_recorder_off_raw.txt`.
Regenerate the tracked CSVs with:

```text
python scripts/analyze_psola_prediction.py artifacts/alpha01d/psola_prediction_raw_final.txt docs/psola_prediction_data
python scripts/analyze_psola_prediction_slack.py artifacts/alpha01d/psola_prediction_raw_final.txt docs/psola_prediction_data/psola_prediction_slack.csv
python scripts/analyze_psola_b1_baseline.py artifacts/alpha01d/psola_prediction_raw_final.txt docs/psola_prediction_data/psola_b1_baseline.csv
```

`psola_prediction_detail.csv` uses status values 0=no candidate, 1=predicted,
2=not predictable, 3=ambiguous. `mark_available` and `model_available` use
0=no, 1=yes, 2=no coherent prior snapshot, 3=not applicable. Accuracy counts
use all captured grains as denominators. The rounded destination lead and
microsecond DSP times are device telemetry; exact destination/source bit
comparisons use the raw double bit patterns.

`psola_b1_baseline.csv` joins the Experiment A first-grain observations to
their N-1 control snapshots and, where retained by the bounded forensic
recorder, grain cycle breakdown. Its recency rank is the difference between
publication serials; it is a candidate-policy *upper bound*, not a measured
prepared-cache hit. The capture did not retain exact publication blocks or
all 16 model references, so it cannot establish exact model age or lead time.
