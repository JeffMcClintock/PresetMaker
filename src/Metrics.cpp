#include "Metrics.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace
{
constexpr int kBands = 40;
constexpr double kPi = 3.14159265358979323846;
constexpr int kResN[2] = {1024, 4096}, kResHop[2] = {128, 512};

double erbRate(double f) { return 21.4 * std::log10(4.37e-3 * f + 1.0); }
double erbInv(double e) { return (std::pow(10.0, e / 21.4) - 1.0) / 4.37e-3; }

// Terhardt approximation of the threshold in quiet, dB SPL.
double thresholdQuiet(double hz)
{
	const double k = std::clamp(hz, 20.0, 20000.0) / 1000.0;
	return 3.64 * std::pow(k, -0.8) - 6.5 * std::exp(-0.6 * (k - 3.3) * (k - 3.3)) + 1e-3 * std::pow(k, 4.0);
}

void fft(std::vector<std::complex<float>>& a)
{
	const size_t n = a.size();
	for (size_t i = 1, j = 0; i < n; ++i)
	{
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) std::swap(a[i], a[j]);
	}
	for (size_t len = 2; len <= n; len <<= 1)
	{
		const float ang = -2.f * static_cast<float>(kPi) / len;
		const std::complex<float> wl(std::cos(ang), std::sin(ang));
		for (size_t i = 0; i < n; i += len)
		{
			std::complex<float> w(1.f);
			for (size_t k = 0; k < len / 2; ++k, w *= wl)
			{
				const auto u = a[i + k], v = a[i + k + len / 2] * w;
				a[i + k] = u + v;
				a[i + k + len / 2] = u - v;
			}
		}
	}
}

struct BandMap
{
	std::vector<double> centre;
	std::vector<double> thr;           // threshold per band, dB SPL
	std::vector<std::vector<std::pair<int, float>>> bins; // per band: (bin, weight)
};

BandMap makeBands(int n, int sr)
{
	BandMap m;
	const double fMax = std::min(16000.0, 0.45 * sr), eLo = erbRate(40), eHi = erbRate(fMax);
	const double binHz = double(sr) / n;
	for (int b = 0; b < kBands; ++b)
	{
		const double lo = erbInv(eLo + (eHi - eLo) * b / kBands), hi = erbInv(eLo + (eHi - eLo) * (b + 1) / kBands);
		m.centre.push_back(0.5 * (lo + hi));
		m.thr.push_back(thresholdQuiet(m.centre.back()));
		std::vector<std::pair<int, float>> v;
		for (int k = 1; k < n / 2; ++k)
			if (k * binHz >= lo && k * binHz < hi)
				v.push_back({k, 1.f});
		if (v.empty()) // band narrower than a bin: borrow the nearest, scaled by width ratio
			v.push_back({std::clamp(int(std::lround(m.centre.back() / binHz)), 1, n / 2 - 1), float((hi - lo) / binHz)});
		m.bins.push_back(std::move(v));
	}
	return m;
}

// Schroeder spreading function: excitation (dB) a masker throws onto a band dz Bark away.
double spreadDb(double dz) { return 15.81 + 7.5 * (dz + 0.474) - 17.5 * std::sqrt(1.0 + (dz + 0.474) * (dz + 0.474)); }

// Masking: replace each band's power by the excitation spread onto it from every band. The kernel
// sums to 1 so a flat spectrum keeps its level; a band is ~1 ERB, about 0.6 Bark in the midrange.
void applyMasking(std::vector<float>& power, int frames, double amount)
{
	if (amount <= 0) return;
	std::vector<double> k(2 * kBands - 1);
	double sum = 0;
	for (int d = 1 - kBands; d < kBands; ++d)
		sum += k[d + kBands - 1] = std::pow(10.0, spreadDb(0.6 * d / amount) / 10.0);
	std::vector<double> e(kBands);
	for (int fr = 0; fr < frames; ++fr)
	{
		float* p = &power[size_t(fr) * kBands];
		for (int b = 0; b < kBands; ++b)
		{
			double acc = 0;
			for (int m = 0; m < kBands; ++m) acc += p[m] * k[b - m + kBands - 1];
			e[b] = acc / sum;
		}
		for (int b = 0; b < kBands; ++b) p[b] = float(e[b]);
	}
}

Scorer::Feat analyse(const std::vector<float>& x, int n, int hop, int sr, size_t paddedLen, double mask)
{
	Scorer::Feat f;
	f.n = n; f.hop = hop; f.bands = kBands;
	f.frames = int((paddedLen > size_t(n) ? paddedLen - n : 0) / hop) + 1;
	f.power.assign(size_t(f.frames) * kBands, 0.f);
	const BandMap bm = makeBands(n, sr);
	std::vector<float> win(n);
	double wsum = 0;
	for (int i = 0; i < n; ++i) { win[i] = 0.5f - 0.5f * std::cos(2 * float(kPi) * i / n); wsum += win[i]; }
	const float norm = float(2.0 / wsum);
	std::vector<std::complex<float>> buf(n);
	for (int fr = 0; fr < f.frames; ++fr)
	{
		const size_t s = size_t(fr) * hop;
		for (int i = 0; i < n; ++i)
			buf[i] = (s + i < x.size() ? x[s + i] : 0.f) * win[i];
		fft(buf);
		for (int b = 0; b < kBands; ++b)
		{
			double p = 0;
			for (auto& [k, wgt] : bm.bins[b])
				p += std::norm(buf[k] * norm) * wgt * 0.5; // sine at full scale = 1.0 power
			f.power[size_t(fr) * kBands + b] = float(p);
		}
	}
	applyMasking(f.power, f.frames, mask);
	return f;
}

// dB SPL level above threshold -> compressive specific loudness
inline double loudness(double powerLin, double offDb, double thrDb)
{
	const double excess = 10.0 * std::log10(powerLin + 1e-14) + offDb - thrDb;
	return excess <= 0 ? 0.0 : std::pow(10.0, 0.023 * excess) - 1.0;
}

// Start the signal 2 ms before note-on. A producer that knows note-on says so (a.onset); only an
// unlabelled WAV falls back to detecting it, which is a threshold and so a discontinuity: a slow
// attack would otherwise be trimmed to where it becomes audible, hiding the attack itself.
std::vector<float> trimOnset(const Audio& a)
{
	const auto& x = a.s;
	const size_t pre = size_t(0.002 * a.sampleRate);
	size_t i = 0;
	if (a.onset >= 0)
		i = size_t(a.onset);
	else
	{
		float pk = 0;
		for (float v : x) pk = std::max(pk, std::fabs(v));
		if (pk <= 0) return x;
		while (i < x.size() && std::fabs(x[i]) < pk * 1e-3f) ++i;
	}
	i = i > pre ? i - pre : 0;
	return std::vector<float>(x.begin() + std::min(i, x.size()), x.end());
}
} // namespace

double Score::similarity() const { return 100.0 * std::exp(-3.0 * total); }

Scorer::Scorer(const Audio& ref, ScoreWeights w) : w_(w), sr_(ref.sampleRate)
{
	ref_.sampleRate = sr_;
	ref_.s = trimOnset(ref);
	len_ = ref_.s.size();
	for (float v : ref_.s) refSumSq_ += double(v) * v;
	for (int r = 0; r < 2; ++r)
		refFeat_.push_back(analyse(ref_.s, kResN[r], kResHop[r], sr_, len_, w_.maskSpread));

	double peak = 1e-14;
	const Feat& f = refFeat_[0];
	for (int fr = 0; fr < f.frames; ++fr)
	{
		double s = 0;
		for (int b = 0; b < kBands; ++b) s += f.power[size_t(fr) * kBands + b];
		peak = std::max(peak, s);
	}
	offDb_ = w_.peakSpl - 10.0 * std::log10(peak);
}

Score Scorer::compare(const Audio& candIn) const
{
	Score sc;
	std::vector<float> cx = trimOnset(candIn);
	cx.resize(len_, 0.f); // a short render is padded with silence; a long one is cut

	double sumSq = 0;
	for (float v : cx) sumSq += double(v) * v;
	if (sumSq < 1e-18)
		return sc; // silent candidate: worst score

	double dist[2] = {}, deltaDist[2] = {}, attackDist = 0, envDist = 0;
	double gainDb = 0;

	for (int r = 0; r < 2; ++r)
	{
		const Feat cf = analyse(cx, kResN[r], kResHop[r], sr_, len_, w_.maskSpread);
		const Feat& rf = refFeat_[r];
		const BandMap bm = makeBands(kResN[r], sr_);

		// frame weights: an attack-emphasis distribution and a uniform one, each summing to 1
		std::vector<double> aw(rf.frames);
		double awSum = 0;
		for (int fr = 0; fr < rf.frames; ++fr)
		{
			const double t = (double(fr) * rf.hop + 0.5 * rf.n) / sr_;
			aw[fr] = std::exp(-t / w_.attackTauSec);
			awSum += aw[fr];
		}
		for (auto& v : aw) v /= awSum;
		const double uw = 1.0 / rf.frames, share = w_.attackShare;

		// reference loudness per frame/band, and its peak total
		std::vector<float> rl(rf.power.size());
		std::vector<double> rtot(rf.frames, 0.0);
		double peakTot = 1e-9;
		for (int fr = 0; fr < rf.frames; ++fr)
		{
			for (int b = 0; b < kBands; ++b)
			{
				const double l = loudness(rf.power[size_t(fr) * kBands + b], offDb_, bm.thr[b]);
				rl[size_t(fr) * kBands + b] = float(l);
				rtot[fr] += l;
			}
			peakTot = std::max(peakTot, rtot[fr]);
		}

		const double eps = 0.05 * peakTot; // floor on the reference mass so silent regions can't blow up the ratio
		const int lag = std::max(1, int(std::lround(w_.deltaLagSec * sr_ / rf.hop)));
		std::vector<float> clAll(rl.size());
		auto evalAt = [&](double g, double* attackOut, double* envOut, double* deltaOut) {
			double dA = 0, dU = 0, mA = 0, mU = 0, eA = 0, eU = 0, dAtk = 0, mAtk = 1e-9;
			for (int fr = 0; fr < rf.frames; ++fr)
			{
				double fd = 0, ctot = 0;
				for (int b = 0; b < kBands; ++b)
				{
					const double cl = loudness(cf.power[size_t(fr) * kBands + b], offDb_ + g, bm.thr[b]);
					clAll[size_t(fr) * kBands + b] = float(cl);
					fd += std::fabs(rl[size_t(fr) * kBands + b] - cl);
					ctot += cl;
				}
				const double ed = std::fabs(rtot[fr] - ctot);
				dA += aw[fr] * fd; mA += aw[fr] * rtot[fr]; eA += aw[fr] * ed;
				dU += uw * fd;     mU += uw * rtot[fr];     eU += uw * ed;
				if ((double(fr) * rf.hop + 0.5 * rf.n) / sr_ < 0.1) { dAtk += fd; mAtk += rtot[fr]; }
			}
			if (attackOut) *attackOut = dAtk / (mAtk + eps);
			if (envOut) *envOut = share * eA / (mA + eps) + (1 - share) * eU / (mU + eps);
			if (deltaOut) // per-band loudness change over the lag: scored against the reference's own amount of change
			{
				double xA = 0, xU = 0, nA = 0, nU = 0;
				for (int fr = 0; fr + lag < rf.frames; ++fr)
				{
					double xd = 0, nd = 0;
					for (int b = 0; b < kBands; ++b)
					{
						const size_t i0 = size_t(fr) * kBands + b, i1 = size_t(fr + lag) * kBands + b;
						const double dr = rl[i1] - rl[i0], dc = clAll[i1] - clAll[i0];
						xd += std::fabs(dr - dc);
						nd += std::fabs(dr);
					}
					xA += aw[fr] * xd; nA += aw[fr] * nd;
					xU += uw * xd;     nU += uw * nd;
				}
				*deltaOut = share * xA / (nA + eps) + (1 - share) * xU / (nU + eps);
			}
			return share * dA / (mA + eps) + (1 - share) * dU / (mU + eps);
		};

		if (r == 0)
		{
			// fit overall gain on the fine resolution: seeded by the RMS ratio (so there is no range
			// to fall off), a coarse grid around it, then a continuous golden-section refine
			const double g0 = 10.0 * std::log10((refSumSq_ + 1e-30) / (sumSq + 1e-30));
			double best = 1e30, bg = g0;
			for (double g = g0 - 9; g <= g0 + 9.01; g += 1.5)
			{
				const double d = evalAt(g, nullptr, nullptr, nullptr);
				if (d < best) { best = d; bg = g; }
			}
			{
				const double phi = 0.6180339887;
				double lo = bg - 1.5, hi = bg + 1.5;
				double a = hi - phi * (hi - lo), b = lo + phi * (hi - lo);
				double fa = evalAt(a, nullptr, nullptr, nullptr), fb = evalAt(b, nullptr, nullptr, nullptr);
				for (int it = 0; it < 8; ++it)
				{
					if (fa < fb) { hi = b; b = a; fb = fa; a = hi - phi * (hi - lo); fa = evalAt(a, nullptr, nullptr, nullptr); }
					else         { lo = a; a = b; fa = fb; b = lo + phi * (hi - lo); fb = evalAt(b, nullptr, nullptr, nullptr); }
				}
				const double gm = 0.5 * (lo + hi), fm = evalAt(gm, nullptr, nullptr, nullptr);
				if (fm < best) { best = fm; bg = gm; }
			}
			gainDb = bg;
			dist[0] = evalAt(gainDb, &attackDist, &envDist, &deltaDist[0]);
		}
		else
			dist[1] = evalAt(gainDb, nullptr, nullptr, &deltaDist[1]);
	}

	sc.fine = dist[0];
	sc.tonal = dist[1];
	sc.envelope = envDist;
	sc.delta = 0.5 * (deltaDist[0] + deltaDist[1]);
	sc.attack = attackDist;
	sc.gainDb = gainDb;
	sc.level = std::fabs(gainDb) / 40.0;
	sc.total = w_.wFine * sc.fine + w_.wTonal * sc.tonal + w_.wEnvelope * sc.envelope + w_.wDelta * sc.delta + w_.wLevel * sc.level;
	return sc;
}
