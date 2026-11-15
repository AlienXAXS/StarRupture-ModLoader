#include "UI/image_resample.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ImageResample
{
	namespace
	{
		constexpr double kPi     = 3.14159265358979323846;
		constexpr double kRadius = 3.0;

		double Sinc(double x)
		{
			if (std::fabs(x) < 1e-9)
				return 1.0;
			const double a = kPi * x;
			return std::sin(a) / a;
		}

		double Lanczos(double x)
		{
			return std::fabs(x) < kRadius ? Sinc(x) * Sinc(x / kRadius) : 0.0;
		}

		// The source pixels one output pixel is made of: first index and normalised weights.
		struct Taps
		{
			int                 first = 0;
			std::vector<double> weight;
		};

		// The taps of every output pixel along one axis. The filter is widened by the shrink factor;
		// a size that is not smaller is not widened. Source pixels past either end repeat the edge.
		std::vector<Taps> MakeTaps(uint32_t in, uint32_t out)
		{
			const double scale = static_cast<double>(in) / out;
			const double width = (std::max)(scale, 1.0);
			std::vector<Taps> taps(out);
			for (uint32_t x = 0; x < out; ++x)
			{
				const double centre = (x + 0.5) * scale;
				const int first = static_cast<int>(std::floor(centre - kRadius * width));
				const int last  = static_cast<int>(std::ceil(centre + kRadius * width));
				Taps& t = taps[x];
				t.first = first;
				double total = 0.0;
				for (int j = first; j <= last; ++j)
				{
					const double v = Lanczos((j + 0.5 - centre) / width);
					t.weight.push_back(v);
					total += v;
				}
				for (double& v : t.weight)
					v /= total;
			}
			return taps;
		}

		int Clamped(int index, uint32_t count)
		{
			return (std::min)((std::max)(index, 0), static_cast<int>(count) - 1);
		}
	}

	void Resample(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t outWidth, uint32_t outHeight,
		std::vector<uint8_t>& out)
	{
		out.clear();
		if (!rgba || width == 0 || height == 0 || outWidth == 0 || outHeight == 0)
			return;
		if (outWidth == width && outHeight == height)
		{
			out.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
			return;
		}

		// Colour times alpha, so clear pixels weigh nothing.
		std::vector<float> pre(static_cast<size_t>(width) * height * 4);
		for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
		{
			const float a = rgba[i * 4 + 3] / 255.0f;
			for (int ch = 0; ch < 3; ++ch)
				pre[i * 4 + ch] = rgba[i * 4 + ch] / 255.0f * a;
			pre[i * 4 + 3] = a;
		}

		// Across, then down.
		const std::vector<Taps> across = MakeTaps(width, outWidth);
		std::vector<float> mid(static_cast<size_t>(outWidth) * height * 4);
		for (uint32_t x = 0; x < outWidth; ++x)
		{
			const Taps& t = across[x];
			for (uint32_t y = 0; y < height; ++y)
				for (int ch = 0; ch < 4; ++ch)
				{
					double sum = 0.0;
					for (size_t k = 0; k < t.weight.size(); ++k)
						sum += t.weight[k] * pre[(static_cast<size_t>(y) * width + Clamped(t.first + static_cast<int>(k), width)) * 4 + ch];
					mid[(static_cast<size_t>(y) * outWidth + x) * 4 + ch] = static_cast<float>(sum);
				}
		}

		const std::vector<Taps> down = MakeTaps(height, outHeight);
		out.resize(static_cast<size_t>(outWidth) * outHeight * 4);
		for (uint32_t y = 0; y < outHeight; ++y)
		{
			const Taps& t = down[y];
			for (uint32_t x = 0; x < outWidth; ++x)
			{
				double v[4];
				for (int ch = 0; ch < 4; ++ch)
				{
					double sum = 0.0;
					for (size_t k = 0; k < t.weight.size(); ++k)
						sum += t.weight[k] * mid[(static_cast<size_t>(Clamped(t.first + static_cast<int>(k), height)) * outWidth + x) * 4 + ch];
					v[ch] = sum;
				}

				// Back to straight alpha.
				const double a = (std::min)((std::max)(v[3], 0.0), 1.0);
				uint8_t* px = &out[(static_cast<size_t>(y) * outWidth + x) * 4];
				for (int ch = 0; ch < 3; ++ch)
					px[ch] = a > 0.002 ? static_cast<uint8_t>(std::lround((std::min)((std::max)(v[ch] / a, 0.0), 1.0) * 255.0)) : 0;
				px[3] = static_cast<uint8_t>(std::lround(a * 255.0));
			}
		}
	}
}
