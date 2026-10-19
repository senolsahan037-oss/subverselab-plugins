// sensecli in.wav [--seconds 10] : runs SourceSense (plugin-common) over the first N seconds and prints one line of CSV:
//   class,confidence,ready,centroid,flatness,lowShare,onsetRate,crest,harmonicity,width,duty,activeSec,score x7 (vocal..fullmix)
#include "../../subverselab-plugin-common/SourceSense/SourceSense.h"
#include "wavio.h"
#include <string>

int main (int argc, char** argv)
{
    if (argc < 2) { std::fprintf (stderr, "usage: sensecli in.wav [--seconds 10]\n"); return 2; }
    double secs = 10.0;
    for (int i = 2; i < argc; ++i) { const std::string a (argv[i]); if (a == "--seconds" && i + 1 < argc) secs = std::atof (argv[++i]); else { std::fprintf (stderr, "unknown argument %s\n", a.c_str()); return 2; } }
    Wav w = readWav (argv[1]);
    static svl::sourcesense::SourceSense ss; ss.prepare ((double) w.sr);
    const size_t n = std::min (w.l.size(), (size_t) (secs * w.sr));
    for (size_t pos = 0; pos < n; pos += 512) ss.process (w.l.data() + pos, w.r.data() + pos, (int) std::min ((size_t) 512, n - pos));
    const auto r = ss.result();
    std::printf ("%s,%.4f,%d,%.1f,%.5f,%.4f,%.3f,%.2f,%.4f,%.4f,%.4f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", svl::sourcesense::className (r.cls), r.confidence, r.ready ? 1 : 0, r.f.centroidHz, r.f.flatness,
                 r.f.lowShare, r.f.onsetRate, r.f.crestDb, r.f.harmonicity, r.f.width, r.f.duty, r.f.activeSec,
                 r.score[0], r.score[1], r.score[2], r.score[3], r.score[4], r.score[5], r.score[6]);
    return 0;
}
