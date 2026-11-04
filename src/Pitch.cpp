#include "Pitch.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <set>
#include "Metrics.h"

PitchEstimate estimatePitch(const Audio& a)
{
	// 0.2 s in (past the attack) where the note is long enough, else from the start
	const double sr = a.sampleRate;
	size_t start = static_cast<size_t>(0.2 * sr);
	if (a.s.size() < start + 8192) start = 0;
	size_t n = 1;
	while (n * 2 <= std::min<size_t>(65536, a.s.size() - start)) n *= 2;
	if (n < 4096) return {};

	std::vector<std::complex<float>> buf(n);
	float peak = 0;
	for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(a.s[start + i]));
	if (peak < 1e-5f) return {};
	for (size_t i = 0; i < n; ++i)
	{
		const float w = 0.5f - 0.5f * std::cos(2 * 3.14159265f * i / n);
		buf[i] = a.s[start + i] * w;
	}
	fft(buf);

	// local maxima between 25 Hz and 6 kHz, interpolated, the three loudest at least 3% apart
	struct Partial { double power, hz; };
	std::vector<Partial> all;
	const size_t lo = static_cast<size_t>(25 * n / sr), hi = std::min<size_t>(static_cast<size_t>(6000 * n / sr), n / 2 - 1);
	auto P = [&](size_t i) { return static_cast<double>(std::norm(buf[i])) + 1e-30; };
	for (size_t i = std::max<size_t>(lo, 1); i < hi; ++i)
		if (P(i) > P(i - 1) && P(i) >= P(i + 1))
		{
			const double l = std::log(P(i - 1)), c = std::log(P(i)), r = std::log(P(i + 1));
			const double d = 0.5 * (l - r) / (l - 2 * c + r);
			all.push_back({ P(i), (i + d) * sr / n });
		}
	std::sort(all.begin(), all.end(), [](auto& x, auto& y) { return x.power > y.power; });
	std::vector<Partial> top;
	for (auto& p : all)
	{
		if (std::all_of(top.begin(), top.end(), [&](auto& q) { return std::fabs(p.hz - q.hz) > p.hz * 0.03; })) top.push_back(p);
		if (top.size() == 3) break;
	}
	if (top.empty()) return {};

	double total = 0;
	for (auto& p : top) total += std::sqrt(p.power);
	std::set<double> cands;
	for (auto& p : top)
		for (int k = 1; k <= 8; ++k)
			if (p.hz / k >= 25) cands.insert(p.hz / k);
	struct Cand { double score, hz; };
	std::vector<Cand> scored;
	for (double c : cands)
	{
		double e = 0;
		for (auto& p : top)
		{
			const int h = static_cast<int>(std::lround(p.hz / c));
			if (h >= 1 && std::fabs(p.hz / h - c) / c < 0.012) e += std::sqrt(p.power);
		}
		scored.push_back({ e / total, c });
	}
	double best = 0;
	for (auto& s : scored) best = std::max(best, s.score);
	PitchEstimate r;
	r.confidence = best;
	for (auto& s : scored)
		if (s.score >= best * 0.92) r.hz = std::max(r.hz, s.hz); // the highest fundamental that explains them
	return r;
}
