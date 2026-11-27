# Future work: retrain the language model

Status: **planned, not started** (recorded 2026-11-27).

## Why

Correct Hebrew such as `אין לי מ` was auto-switched to `thi kh n`.
The LSTM scored that gibberish as English with ~0.97 confidence.
The dictionary-triage fix (short trailing fragment is neutral, suppress
floor 0.97) hides most of these cases, but it does not fix the cause,
which lives in the model.

## Root cause in the training data (`model/train_model.py`, `build_samples`)

1. **Contradictory labels.** `emit_prefixes` labels every prefix (length
   2 or more) of a real word with that word's language. So `thi` (from
   *this*) and `kh` (from *khaki*) are taught as **English**. The same
   strings also show up as cross-layout garbage of Hebrew (`אין`→`thi`,
   `לי`→`kh`) labelled **N/A**. The English prefixes outnumber them, so the net
   learns "English". Calibration cannot fix this, because the classes truly
   overlap on short prefixes.
2. **Phrase negatives have no prefixes**, and only one other layout is
   garbled per phrase. The app scores phrases keystroke by keystroke, so most
   of what it sees is a prefix of a phrase.
3. **Phrases are random bags of words** (`rng.sample(pool, k)`). The model
   learns "does each word look real", not "is this plausible text".
4. The model answers "which language is this string?" when the real question
   is "which of these two readings did the user intend?" (the text as typed
   on the current layout vs. the converted text).

## Proposed approach (cheapest first)

1. **Consistent labels.** Find strings that are both a prefix or word in
   language A and cross-layout garbage of language B. Give them soft targets
   weighted by corpus frequency, or drop them, instead of conflicting hard
   labels.
2. **Prefixes for phrase negatives, both other layouts.**
3. **Real word sequences** from a corpus (e.g. OpenSubtitles, Wikipedia
   sentences) in place of random word bags.
4. **Temperature-scale** on a held-out set of cross-layout conversions, so the
   thresholds in `cpp/src/Config.cpp` mean calibrated probabilities.
5. *(ambitious)* **Pairwise model.** Input = (typed, converted) and output =
   intended reading. Or per-language generative char-LMs plus a
   length-normalised log-likelihood ratio. Gibberish then scores low in every
   language instead of high in one. References:
   [Wikimedia wrong-keyboard notes](https://www.mediawiki.org/wiki/User:TJones_(WMF)/Notes/Typing_on_the_Wrong_Keyboard_Russian_and_English),
   [EMNLP 2014 char n-gram layout detection](https://mirror.aclweb.org/emnlp2014/papers/pdf/EMNLP2014068.pdf).

## Acceptance criteria

- Offline: single-word harness (`model/evaluate_transitions.py`), FP must
  stay at 0 and TP must not drop.
- Phrase stress set: Hebrew typed correctly and cut mid-last-word must give
  fewer false switches than the triage-only fix. The baseline is in the
  2026-11-27 bug-1 experiment: C++-faithful mirror, 12/518 at floor 0 and
  8/518 at floor 0.97.
- Re-tune `Config.cpp` per-pair params after retraining. Consider relaxing
  `TriageSuppressFloor` if the model alone is now robust.
- Export with `model/convert_to_onnx.py`. Keep the ONNX input/output
  contract (classes N/A, en, he, ru) or update `Languages.cpp`.
