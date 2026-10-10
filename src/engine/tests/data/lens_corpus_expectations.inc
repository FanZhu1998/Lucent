// Corpus expectations of LensFixtures.TheCorpusMatchesAsDocumented (lens_correction_fixture_test.cpp):
// fixture id, the match kind of its automatic develop and the corrections applied ("d" distortion,
// "t" lateral CA, "v" vignetting), with the bundled lens database 1.0.0.
//
// Status: CONFIRMED. Predicted on 2026-10-10 by applying the matching and planning rules of
// lens_correction.cpp (match version 1), on top of a transcription of Lensfun 0.3.4's searches, to
// the probe metadata of every fixture (lucent-probe, LibRaw 0.22.2) and the bundled database; the
// first build's run of the test (W13 build-and-test phase, 2026-10-10) measured exactly this table
// for all 40 fixtures (ADR 0023, section 11). The test prints the table it measures. The two Nikon
// Z 8 High Efficiency NEFs match exactly from their probe, but this build's LibRaw cannot decode
// them, so they are never rendered (ExactMatchesRenderTheirCorrections notes and skips them).
{"adobe-dng-canon-eos-5d-mark-iii-linear-dng", LensMatchKind::Exact, "dtv"},
{"bq-aquaris-u-plus-dng", LensMatchKind::None, ""},
{"canon-eos-350d-cr2", LensMatchKind::None, ""},
{"canon-eos-5d-mark-iv-cr2-raw", LensMatchKind::Fuzzy, ""},
{"canon-eos-5d-mark-iv-cr2-sraw", LensMatchKind::Fuzzy, ""},
{"canon-eos-7d-mark-ii-dng-jxl", LensMatchKind::Fuzzy, ""},
{"canon-eos-r5-cr3-craw", LensMatchKind::Fuzzy, ""},
{"canon-eos-r5-cr3-craw-crop-dualpixel", LensMatchKind::Fuzzy, ""},
{"canon-eos-r5-cr3-raw", LensMatchKind::Fuzzy, ""},
{"canon-eos-r5-cr3-raw-crop", LensMatchKind::Fuzzy, ""},
{"dji-fc220-dng", LensMatchKind::None, ""},
{"dji-fc4382-dng", LensMatchKind::None, ""},
{"dji-fc7303-dng", LensMatchKind::None, ""},
{"fujifilm-gfx-50s-ii-raf-compressed", LensMatchKind::Exact, "dtv"},
{"fujifilm-x-t4-raf-compressed", LensMatchKind::Exact, "dt"},
{"fujifilm-x-t4-raf-lossless", LensMatchKind::Exact, "dt"},
{"hasselblad-l2d-20c-dng", LensMatchKind::Exact, ""},
{"hasselblad-x1d-ii-50c-3fr", LensMatchKind::None, ""},
{"insta360-one-x-dng", LensMatchKind::None, ""},
{"leica-m-monochrom-typ-246-dng", LensMatchKind::None, ""},
{"leica-m10-dng", LensMatchKind::Exact, "dt"},
{"nikon-z-6-nef-compressed", LensMatchKind::Exact, "dtv"},
{"nikon-z-6-nef-lossless", LensMatchKind::Exact, "dtv"},
{"nikon-z-8-nef-he", LensMatchKind::Exact, "dtv"},
{"nikon-z-8-nef-he-star", LensMatchKind::Exact, "dtv"},
{"olympus-xz-10-orf", LensMatchKind::None, ""},
{"om-system-om-1-orf", LensMatchKind::Fuzzy, ""},
{"oneplus-one-dng", LensMatchKind::None, ""},
{"panasonic-dc-s5m2-rw2", LensMatchKind::None, ""},
{"pentax-k-3-mark-iii-monochrome-dng", LensMatchKind::None, ""},
{"pentax-k-3-mark-iii-pef", LensMatchKind::None, ""},
{"ricoh-gxr-s10-dng", LensMatchKind::None, ""},
{"sony-dslr-a450-arw", LensMatchKind::None, ""},
{"sony-ilce-7m4-arw-compressed", LensMatchKind::Exact, "dt"},
{"sony-ilce-7m4-arw-compressed-apsc", LensMatchKind::Fuzzy, ""},
{"sony-ilce-7m4-arw-lossless-l", LensMatchKind::Exact, "dt"},
{"sony-ilce-7m4-arw-lossless-m", LensMatchKind::Exact, "dt"},
{"sony-ilce-7m4-arw-lossless-s", LensMatchKind::Exact, "dt"},
{"sony-ilce-7m4-arw-uncompressed", LensMatchKind::Exact, "dt"},
{"sony-ilce-7rm5-arw-compressed", LensMatchKind::Exact, "dt"},
