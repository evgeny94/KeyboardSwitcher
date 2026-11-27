"""Regression test: a 1-2 char trailing fragment must not cancel suppression.

Bug (2026-11-27): correct Hebrew "אין לי מ" scored evidence 2/3 because the
1-char trailing "מ" counted as unattested, so suppression never fired and the
LSTM switched it to gibberish English "thi kh n".

Run:  python model/test_triage_short_tail.py
"""
from triage import LexicalTriage, TriageParams


def main() -> None:
    t = LexicalTriage(params=TriageParams(w_fuzzy=0, w_trigram=0, w_lsh=0),
                      enable_fuzzy=False, enable_lsh=False)
    ev = t.evidence
    cases = [
        # (text, lang, expected evidence, why)
        ("אין לי מ", "he", 1.0, "bug repro: 2 attested words + 1-char tail"),
        ("כן נו א", "he", 1.0, "bug repro: 2 attested words + 1-char tail"),
        ("אין לי מש", "he", 1.0, "2-char tail is also neutral"),
        ("עם א", "he", 0.5, "only 1 prior word: tail still counts"),
        ("this is t", "en", 1.0, "same rule for English"),
        ("это все п", "ru", 1.0, "same rule for Russian"),
    ]
    failed = 0
    for text, lang, want, why in cases:
        got = ev(text, lang)
        ok = abs(got - want) < 1e-6
        failed += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {text!r:>14} {lang} ev={got:.3f} want={want:.3f}  {why}")
    # A tail is only neutral when every prior word is attested.
    got = ev("אין קקקקקק מ", "he")
    ok = got < 1.0
    failed += not ok
    print(f"{'ok  ' if ok else 'FAIL'} unattested prior word keeps the tail counted (ev={got:.3f})")
    assert failed == 0, f"{failed} case(s) failed"
    print("all passed")


if __name__ == "__main__":
    main()
