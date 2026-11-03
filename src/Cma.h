#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <random>
#include <vector>

// Separable CMA-ES (diagonal covariance) on the unit cube. Minimises f. Keeps the best point seen.
class SepCma
{
public:
	SepCma(std::vector<double> start, double sigma, unsigned seed = 1)
		: n_(start.size()), m_(std::move(start)), sigma_(sigma), rng_(seed)
	{
		lambda_ = 4 + int(3 * std::log(double(n_)));
		const int mu = lambda_ / 2;
		w_.resize(mu);
		for (int i = 0; i < mu; ++i) w_[i] = std::log(mu + 0.5) - std::log(i + 1.0);
		const double s = std::accumulate(w_.begin(), w_.end(), 0.0);
		double s2 = 0;
		for (auto& w : w_) { w /= s; s2 += w * w; }
		mueff_ = 1.0 / s2;
		const double n = double(n_);
		cs_ = (mueff_ + 2) / (n + mueff_ + 5);
		ds_ = 1 + 2 * std::max(0.0, std::sqrt((mueff_ - 1) / (n + 1)) - 1) + cs_;
		cc_ = (4 + mueff_ / n) / (n + 4 + 2 * mueff_ / n);
		c1_ = 2.0 / ((n + 1.3) * (n + 1.3) + mueff_) * (n + 2) / 3;
		cmu_ = std::min(1 - c1_, 2 * (mueff_ - 2 + 1 / mueff_) / ((n + 2) * (n + 2) + mueff_)) * (n + 2) / 3;
		chiN_ = std::sqrt(n) * (1 - 1 / (4 * n) + 1 / (21 * n * n));
		C_.assign(n_, 1.0); pc_.assign(n_, 0.0); ps_.assign(n_, 0.0);
		best_ = m_;
	}

	// One generation. Returns number of evaluations used.
	int step(const std::function<double(const std::vector<double>&)>& f)
	{
		std::normal_distribution<double> nd;
		struct Ind { std::vector<double> x, y, z; double f; };
		std::vector<Ind> pop(lambda_);
		for (auto& p : pop)
		{
			p.x.resize(n_); p.y.resize(n_); p.z.resize(n_);
			for (size_t i = 0; i < n_; ++i)
			{
				p.z[i] = nd(rng_);
				p.y[i] = std::sqrt(C_[i]) * p.z[i];
				p.x[i] = std::clamp(m_[i] + sigma_ * p.y[i], 0.0, 1.0);
				// Measure the step that was actually taken, or a mean pinned to a bound drags the
				// evolution paths one way every generation and sigma inflates to its cap.
				p.y[i] = (p.x[i] - m_[i]) / sigma_;
				p.z[i] = p.y[i] / std::sqrt(C_[i]);
			}
			p.f = f(p.x);
			if (p.f < bestF_) { bestF_ = p.f; best_ = p.x; }
		}
		std::sort(pop.begin(), pop.end(), [](const Ind& a, const Ind& b) { return a.f < b.f; });

		std::vector<double> ym(n_, 0.0), zm(n_, 0.0);
		for (size_t k = 0; k < w_.size(); ++k)
			for (size_t i = 0; i < n_; ++i) { ym[i] += w_[k] * pop[k].y[i]; zm[i] += w_[k] * pop[k].z[i]; }
		for (size_t i = 0; i < n_; ++i) m_[i] = std::clamp(m_[i] + sigma_ * ym[i], 0.0, 1.0);

		double psn = 0;
		for (size_t i = 0; i < n_; ++i)
		{
			ps_[i] = (1 - cs_) * ps_[i] + std::sqrt(cs_ * (2 - cs_) * mueff_) * zm[i];
			psn += ps_[i] * ps_[i];
		}
		psn = std::sqrt(psn);
		++gen_;
		const bool hsig = psn / std::sqrt(1 - std::pow(1 - cs_, 2.0 * gen_)) / chiN_ < 1.4 + 2.0 / (n_ + 1);
		for (size_t i = 0; i < n_; ++i)
		{
			pc_[i] = (1 - cc_) * pc_[i] + (hsig ? std::sqrt(cc_ * (2 - cc_) * mueff_) * ym[i] : 0.0);
			double rankMu = 0;
			for (size_t k = 0; k < w_.size(); ++k) rankMu += w_[k] * pop[k].y[i] * pop[k].y[i];
			C_[i] = (1 - c1_ - cmu_) * C_[i] + c1_ * (pc_[i] * pc_[i] + (hsig ? 0 : cc_ * (2 - cc_)) * C_[i]) + cmu_ * rankMu;
			C_[i] = std::max(C_[i], 1e-8);
		}
		sigma_ = std::clamp(sigma_ * std::exp(cs_ / ds_ * (psn / chiN_ - 1)), 1e-4, 0.5);
		return lambda_;
	}

	const std::vector<double>& best() const { return best_; }
	double bestF() const { return bestF_; }
	double sigma() const { return sigma_; }

private:
	size_t n_;
	std::vector<double> m_;
	double sigma_;
	std::mt19937 rng_;
	int lambda_ = 0, gen_ = 0;
	std::vector<double> w_, C_, pc_, ps_, best_;
	double mueff_ = 0, cs_ = 0, ds_ = 0, cc_ = 0, c1_ = 0, cmu_ = 0, chiN_ = 0, bestF_ = 1e30;
};
