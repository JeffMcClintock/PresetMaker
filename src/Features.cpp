#include "Features.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <format>
#include "Metrics.h" // fft

namespace
{
std::vector<double> smooth(const std::vector<double>& x, int half)
{
	std::vector<double> s(x.size());
	const long long n = (long long)x.size();
	for (long long i = 0; i < n; ++i)
	{
		const long long a = std::max(0LL, i - half), b = std::min(n, i + half + 1);
		double acc = 0;
		for (long long k = a; k < b; ++k) acc += x[k];
		s[i] = acc / (b - a);
	}
	return s;
}

// |log((a+floor)/(b+floor))| scaled so a ratio of `full` scores 1
double logRatio(double a, double b, double floor, double full)
{
	return std::min(1.0, std::fabs(std::log((a + floor) / (b + floor))) / std::log(full));
}
} // namespace

EnvelopeFeatures extractFeatures(const std::vector<double>& e, double dt, long long noteOff)
{
	EnvelopeFeatures f;
	const long long n = (long long)e.size();
	if (n < 4) return f;
	// the held part ends at note-off, or where the sound has fallen 40 dB below its loudest when that is unknown
	long long held = noteOff > 0 && noteOff < n ? noteOff : n;
	if (noteOff <= 0 || noteOff >= n)
	{
		const double top = *std::max_element(e.begin(), e.end());
		while (held > 1 && e[held - 1] < top - 40) --held;
	}

	// attack from the raw level; the rest from a 50 ms smoothing, so tremolo does not read as decay
	long long rawPeak = 0;
	for (long long i = 1; i < held; ++i)
		if (e[i] > e[rawPeak]) rawPeak = i;
	long long atk = 0;
	while (atk < rawPeak && e[atk] < e[rawPeak] - 2) ++atk;
	f.attackSec = atk * dt;

	const std::vector<double> s = smooth(e, std::max(1, int(0.025 / dt)));
	long long peakAt = 0;
	for (long long i = 1; i < held; ++i)
		if (s[i] > s[peakAt]) peakAt = i;
	const double peak = s[peakAt];

	{
		const long long from = held - std::max<long long>(1, held / 3);
		std::vector<double> v(s.begin() + from, s.begin() + held);
		std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
		f.sustainDb = std::min(0.0, v[v.size() / 2] - peak);
	}

	long long dec = peakAt;
	while (dec < held && s[dec] > peak + f.sustainDb + 1) ++dec;
	f.decaySec = (dec - peakAt) * dt;

	if (noteOff > 0 && noteOff < n)
	{
		const double at = s[noteOff];
		long long r = noteOff;
		while (r < n && s[r] > at - 20) ++r;
		f.releaseSec = (r - noteOff) * dt;
	}

	// tremolo: the held part once the decay is over, linear trend removed, Hann window, zero-padded FFT
	const long long t0 = std::max(dec, (long long)(0.25 / dt)), m = held - t0;
	if (m * dt >= 0.5)
	{
		double sx = 0, sy = 0, sxx = 0, sxy = 0;
		for (long long i = 0; i < m; ++i) { sx += i; sy += e[t0 + i]; sxx += double(i) * i; sxy += i * e[t0 + i]; }
		const double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx + 1e-9), icpt = (sy - slope * sx) / m;
		size_t N = 1;
		while (N < size_t(4 * m)) N <<= 1;
		std::vector<std::complex<float>> buf(N);
		double wsum = 0;
		for (long long i = 0; i < m; ++i)
		{
			const double w = 0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * i / m);
			buf[i] = float((e[t0 + i] - (icpt + slope * i)) * w);
			wsum += w;
		}
		fft(buf);
		const double df = 1.0 / (N * dt);
		size_t best = 0;
		double bestMag = 0;
		const double fMin = std::max(0.5, 2.0 / (m * dt)); // at least two full cycles in the window
		for (size_t k = size_t(std::ceil(fMin / df)); k <= size_t(15.0 / df) && k < N / 2; ++k)
			if (std::abs(buf[k]) > bestMag) { bestMag = std::abs(buf[k]); best = k; }
		const double depth = 2.0 * (2.0 * bestMag / wsum); // sinusoid amplitude 2|X|/sum(w); peak-to-peak is twice that
		if (depth >= 0.3) { f.tremoloHz = best * df; f.tremoloDb = depth; }
	}
	return f;
}

double featureDistance(const EnvelopeFeatures& a, const EnvelopeFeatures& b)
{
	double d = 0;
	int k = 0;
	d += logRatio(a.attackSec, b.attackSec, 0.005, 4); ++k;
	d += logRatio(a.decaySec, b.decaySec, 0.02, 4); ++k;
	d += std::min(1.0, std::fabs(a.sustainDb - b.sustainDb) / 20.0); ++k;
	if (a.releaseSec >= 0 && b.releaseSec >= 0) { d += logRatio(a.releaseSec, b.releaseSec, 0.01, 4); ++k; }
	d += std::min(1.0, std::fabs(a.tremoloDb - b.tremoloDb) / 3.0); ++k;
	if (a.tremoloHz > 0 && b.tremoloHz > 0) { d += logRatio(a.tremoloHz, b.tremoloHz, 0, 2); ++k; }
	return d / k;
}

std::string describe(const EnvelopeFeatures& f)
{
	std::string s = std::format("attack {:.0f} ms, decay {:.0f} ms, sustain {:+.1f} dB", f.attackSec * 1e3, f.decaySec * 1e3, f.sustainDb);
	if (f.releaseSec >= 0) s += std::format(", release {:.0f} ms", f.releaseSec * 1e3);
	s += f.tremoloHz > 0 ? std::format(", tremolo {:.1f} Hz {:.1f} dB", f.tremoloHz, f.tremoloDb) : ", no tremolo";
	return s;
}
