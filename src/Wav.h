#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Mono float audio. Files are written 32-bit float; read accepts 16-bit PCM and float.
struct Audio
{
	int sampleRate = 48000;
	std::vector<float> s;
	long long onset = -1;   // sample of note-on when the producer knows it; -1 = detect from the signal
	long long noteOff = -1; // sample of note-off when the producer knows it; -1 = unknown
};

inline bool writeWav(const std::string& path, const Audio& a)
{
	FILE* f = fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const uint32_t n = static_cast<uint32_t>(a.s.size() * 4), rate = a.sampleRate;
	const uint16_t fmt = 3, ch = 1, bits = 32, align = 4;
	const uint32_t riff = 36 + n, fmtSize = 16, byteRate = rate * 4;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmtSize, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
	fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&n, 4, 1, f); fwrite(a.s.data(), 4, a.s.size(), f);
	fclose(f);
	return true;
}

inline bool readWav(const std::string& path, Audio& a)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::vector<uint8_t> b;
	uint8_t tmp[65536];
	for (size_t n; (n = fread(tmp, 1, sizeof tmp, f)) > 0;)
		b.insert(b.end(), tmp, tmp + n);
	fclose(f);
	if (b.size() < 12 || memcmp(b.data(), "RIFF", 4))
		return false;
	uint16_t fmt = 0, ch = 1, bits = 16;
	for (size_t p = 12; p + 8 <= b.size();)
	{
		uint32_t sz;
		memcpy(&sz, &b[p + 4], 4);
		const uint8_t* d = &b[p + 8];
		if (!memcmp(&b[p], "fmt ", 4))
		{
			memcpy(&fmt, d, 2); memcpy(&ch, d + 2, 2);
			uint32_t r; memcpy(&r, d + 4, 4); a.sampleRate = static_cast<int>(r);
			memcpy(&bits, d + 14, 2);
		}
		else if (!memcmp(&b[p], "data", 4))
		{
			sz = static_cast<uint32_t>(std::min<size_t>(sz, b.size() - p - 8));
			const size_t frames = sz / (ch * bits / 8);
			a.s.assign(frames, 0.f);
			for (size_t i = 0; i < frames; ++i)
			{
				float sum = 0;
				for (int c = 0; c < ch; ++c)
				{
					if (bits == 16) { int16_t v; memcpy(&v, d + (i * ch + c) * 2, 2); sum += v / 32768.f; }
					else if (fmt == 3 && bits == 32) { float v; memcpy(&v, d + (i * ch + c) * 4, 4); sum += v; }
				}
				a.s[i] = sum / ch;
			}
			return true;
		}
		p += 8 + sz + (sz & 1);
	}
	return false;
}
