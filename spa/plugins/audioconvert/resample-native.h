/* Spa
 *
 * Copyright © 2019 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <math.h>

typedef void (*resample_func_t)(struct resample *r,
        const void * SPA_RESTRICT src[], uint32_t *in_len,
        void * SPA_RESTRICT dst[], uint32_t offs, uint32_t *out_len);

struct native_data {
	double rate;
	uint32_t n_taps;
	uint32_t n_phases;
	uint32_t oversample;
	uint32_t in_rate;
	uint32_t out_rate;
	uint32_t index;
	uint32_t phase;
	uint32_t inc;
	uint32_t frac;
	uint32_t filter_stride;
	uint32_t filter_stride_os;
	uint32_t hist;
	float **history;
	resample_func_t func;
	float *filter;
	float *hist_mem;
};

#include "resample-native-impl.h"
#include "resample-native-c.h"
#if defined (__SSE__)
#include "resample-native-sse.h"
#endif
#if defined (__SSSE3__)
#include "resample-native-ssse3.h"
#endif

struct quality {
	uint32_t n_taps;
	double cutoff;
};

#define DEFAULT_QUALITY	4

static const struct quality blackman_qualities[] = {
	{ 8, 0.5, },
	{ 16, 0.6, },
	{ 24, 0.72, },
	{ 32, 0.8, },
	{ 48, 0.85, },                  /* default */
	{ 64, 0.90, },
	{ 80, 0.92, },
	{ 96, 0.933, },
	{ 128, 0.950, },
	{ 144, 0.955, },
	{ 160, 0.960, }
};

static inline double sinc(double x)
{
	if (x < 1e-6) return 1.0;
	x *= M_PI;
	return sin(x) / x;
}

static inline double blackman(double x, double n_taps)
{
	double w = 2.0 * x * M_PI / n_taps + M_PI;
	return 0.3635819 - 0.4891775 * cos(w) +
		0.1365995 * cos(2 * w) - 0.0106411 * cos(3 * w);
}

static int build_filter(float *taps, uint32_t stride, uint32_t n_taps, uint32_t n_phases, double cutoff)
{
	uint32_t i, j, n_taps12 = n_taps/2;

	for (i = 0; i <= n_phases; i++) {
		double t = (double) i / (double) n_phases;
		for (j = 0; j < n_taps12; j++, t += 1.0) {
			/* exploit symmetry in filter taps */
			taps[(n_phases - i) * stride + n_taps12 + j] =
			taps[i * stride + (n_taps12 - j - 1)] =
				cutoff * sinc(t * cutoff) * blackman(t, n_taps);
		}
	}
	return 0;
}

static void impl_native_free(struct resample *r)
{
	free(r->data);
	r->data = NULL;
}

static inline uint32_t calc_gcd(uint32_t a, uint32_t b)
{
	while (b != 0) {
		uint32_t temp = a;
		a = b;
		b = temp % b;
	}
	return a;
}

static void impl_native_update_rate(struct resample *r, double rate)
{
	struct native_data *data = r->data;
	uint32_t in_rate, out_rate, phase, gcd;

	in_rate = r->i_rate * rate;
	out_rate = r->o_rate;
	phase = data->phase;

	gcd = calc_gcd(in_rate, out_rate);
	gcd = calc_gcd(gcd, phase);

	data->rate = rate;
	data->phase = phase / gcd;
	data->in_rate = in_rate / gcd;
	data->out_rate = out_rate / gcd;

	data->inc = data->in_rate / data->out_rate;
	data->frac = data->in_rate % data->out_rate;

	data->func = rate == 1.0 ? do_resample_full_c : do_resample_inter_c;
#if defined (__SSE__)
	if (r->cpu_flags & SPA_CPU_FLAG_SSE)
		data->func = rate == 1.0 ? do_resample_full_sse : do_resample_inter_sse;
#endif
#if defined (__SSSE3__)
	if (r->cpu_flags & SPA_CPU_FLAG_SSSE3)
		data->func = rate == 1.0 ? do_resample_full_ssse3 : do_resample_inter_ssse3;
#endif
}

static void impl_native_process(struct resample *r,
		const void * SPA_RESTRICT src[], uint32_t *in_len,
		void * SPA_RESTRICT dst[], uint32_t *out_len)
{
	struct native_data *data = r->data;
	uint32_t n_taps = data->n_taps;
	float **history = data->history;
	const float **s = (const float **)src;
	uint32_t c, refill, hist, in, out, remain;

	out = refill = in = 0;
	hist = data->hist;

	if (hist) {
		/* first work on the history if any. */
		if (hist < n_taps) {
			/* we need at least n_taps to completely process the
			 * history before we can work on the new input. When
			 * we have less, refill the history. */
			refill = SPA_MIN(*in_len, n_taps);
			for (c = 0; c < r->channels; c++)
				memcpy(&history[c][hist], s[c], refill * sizeof(float));

			if (hist + refill < n_taps) {
				/* not enough in the history, keep the input in
				 * the history and produce no output */
				data->hist = hist + refill;
				*in_len = refill;
				*out_len = 0;
				return;
			}
		}
		/* now we have at least n_taps of data in the history
		 * and we try to process it */
		in = hist + refill;
		out = *out_len;
		data->func(r, (const void**)history, &in, dst, 0, &out);
	}

	if (data->index >= hist) {
		/* we are past the history and can now work on the new
		 * input data */
		data->index -= hist;
		in = *in_len;
		data->func(r, src, &in, dst, out, out_len);

		remain = *in_len - in;
		if (remain < n_taps) {
			/* not enough input data remaining for more output,
			 * copy to history */
			for (c = 0; c < r->channels; c++)
				memcpy(history[c], &s[c][in], remain * sizeof(float));
		} else {
			/* we have enough input data remaining to produce
			 * more output ask to resubmit. else we copy the
			 * remainder to the history */
			remain = 0;
			*in_len = in;
		}
	} else {
		/* we are still working on the history */
		remain = hist - in;
		if (*in_len < n_taps) {
			/* not enough input data, add it to the history because
			 * resubmitting it is not going to make progress.
			 * We copied this into the history above. */
			remain += refill;
			*in_len = refill;
		} else {
			/* input has enough data to possibly produce more output
			 * from the history so ask to resubmit */
			*in_len = 0;
		}
		if (remain) {
			/* move history */
			for (c = 0; c < r->channels; c++)
				memmove(history[c], &history[c][in], remain * sizeof(float));
		}
	}
	data->hist = remain;
	data->index = 0;
	return;
}

static void impl_native_reset (struct resample *r)
{
	struct native_data *d = r->data;
	memset(d->hist_mem, 0, r->channels * sizeof(float) * d->n_taps * 2);
	d->hist = d->n_taps / 2;
	d->index = 0;
	d->phase = 0;
}

static uint32_t impl_native_delay (struct resample *r)
{
	struct native_data *d = r->data;
	return d->n_taps;
}

static int impl_native_init(struct resample *r)
{
	struct native_data *d;
	const struct quality *q = &blackman_qualities[DEFAULT_QUALITY];
	double scale;
	uint32_t c, n_taps, n_phases, filter_size, in_rate, out_rate, gcd, filter_stride;
	uint32_t history_stride, history_size, oversample;

	r->free = impl_native_free;
	r->update_rate = impl_native_update_rate;
	r->process = impl_native_process;
	r->reset = impl_native_reset;
	r->delay = impl_native_delay;

	gcd = calc_gcd(r->i_rate, r->o_rate);

	in_rate = r->i_rate / gcd;
	out_rate = r->o_rate / gcd;

	scale = SPA_MIN(q->cutoff * out_rate / in_rate, 1.0);
	/* multiple of 8 taps to ease simd optimizations */
	n_taps = SPA_ROUND_UP_N((uint32_t)ceil(q->n_taps / scale), 8);

	/* try to get at least 256 phases so that interpolation is
	 * accurate enough when activated */
	n_phases = out_rate;
	oversample = (255 + n_phases) / n_phases;
	n_phases *= oversample;

	filter_stride = SPA_ROUND_UP_N(n_taps * sizeof(float), 64);
	filter_size = filter_stride * (n_phases + 1);
	history_stride = SPA_ROUND_UP_N(2 * n_taps * sizeof(float), 64);
	history_size = r->channels * history_stride;

	d = malloc(sizeof(struct native_data) +
			filter_size +
			history_size +
			(r->channels * sizeof(float*)) +
			64);

	if (d == NULL)
		return -ENOMEM;

	r->data = d;
	d->n_taps = n_taps;
	d->n_phases = n_phases;
	d->oversample = oversample;
	d->in_rate = in_rate;
	d->out_rate = out_rate;
	d->filter = SPA_MEMBER(d, sizeof(struct native_data), float);
	d->filter = SPA_PTR_ALIGN(d->filter, 64, float);
	d->hist_mem = SPA_MEMBER(d->filter, filter_size, float);
	d->hist_mem = SPA_PTR_ALIGN(d->hist_mem, 64, float);
	d->history = SPA_MEMBER(d->hist_mem, history_size, float*);
	d->filter_stride = filter_stride / sizeof(float);
	d->filter_stride_os = d->filter_stride * oversample;
	for (c = 0; c < r->channels; c++)
		d->history[c] = SPA_MEMBER(d->hist_mem, c * history_stride, float);

	build_filter(d->filter, d->filter_stride, n_taps, n_phases, scale);

	impl_native_reset(r);
	impl_native_update_rate(r, 1.0);

	return 0;
}