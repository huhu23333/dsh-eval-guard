#include <nn/nn.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using nn::DType;
using nn::Shape;
using nn::Tensor;

static int g_passed = 0;
static int g_failed = 0;

static void report(const std::string& name, bool ok) {
    if (ok) { ++g_passed; std::printf("TEST %s PASS\n", name.c_str()); }
    else { ++g_failed; std::printf("TEST %s FAIL\n", name.c_str()); }
}

static bool close(float a, float b, float tol = 1e-4f) {
    return std::fabs(a - b) <= tol * (1.0f + std::fabs(a) + std::fabs(b));
}

static std::vector<float> f32_data(const Tensor& t) {
    Tensor c = t.contiguous();
    std::vector<float> out(static_cast<size_t>(c.numel()));
    if (out.empty()) return out;
    std::memcpy(out.data(), c.data(), out.size() * sizeof(float));
    return out;
}

static Tensor make_f32(const Shape& shape, const std::vector<float>& values) {
    return Tensor::from_data(values.data(), shape, DType::F32);
}

static bool eq_data(const Tensor& t, const std::vector<float>& expected, float tol = 1e-4f) {
    const std::vector<float> got = f32_data(t);
    if (got.size() != expected.size()) return false;
    for (size_t i = 0; i < got.size(); ++i) if (!close(got[i], expected[i], tol)) return false;
    return true;
}

static bool shape_is(const Tensor& t, const Shape& shape) { return t.shape() == shape; }

static void test_dtype_metadata() {
    bool ok = nn::dtype_size(DType::F32) == 4 && nn::dtype_size(DType::F64) == 8 &&
              nn::dtype_size(DType::I8) == 1 && nn::dtype_size(DType::I16) == 2 &&
              nn::dtype_size(DType::I32) == 4 && nn::dtype_size(DType::I64) == 8 &&
              nn::dtype_size(DType::I128) == 16 && nn::dtype_size(DType::U128) == 16 &&
              nn::dtype_size(DType::BF16) == 2 && nn::dtype_size(DType::FP8_E5M2) == 1 &&
              nn::dtype_size(DType::MXFP4) == 1;
    report("dtype_metadata", ok);
}

static void test_casts_float() {
    Tensor a = make_f32({3}, {1.5f, -2.25f, 3.0f});
    Tensor d = a.to(DType::F64);
    Tensor back = d.to(DType::F32);
    report("cast_float", d.dtype() == DType::F64 && eq_data(back, {1.5f, -2.25f, 3.0f}));
}

static void test_casts_int() {
    Tensor a = make_f32({5}, {3.6f, -1.2f, 300.0f, -300.0f, 0.0f});
    Tensor i = a.to(DType::I32);
    Tensor narrow = a.to(DType::I8);
    int32_t iv[5]{};
    int8_t nv[5]{};
    std::memcpy(iv, i.contiguous().data(), sizeof(iv));
    std::memcpy(nv, narrow.contiguous().data(), sizeof(nv));
    bool ok = iv[0] == 4 && iv[1] == -1 && iv[2] == 300 && iv[3] == -300 && iv[4] == 0;
    ok = ok && nv[0] == 4 && nv[1] == -1 && nv[2] == 127 && nv[3] == -128 && nv[4] == 0;
    report("cast_int_saturate", ok);
}

static void test_integer_casts() {
    Tensor a = make_f32({4}, {0.0f, 1.0f, 2.0f, 3.0f});
    bool ok = true;
    for (DType d : {DType::I8, DType::I16, DType::I32, DType::I64, DType::I128,
                    DType::U8, DType::U16, DType::U32, DType::U64, DType::U128}) {
        Tensor back = a.to(d).to(DType::F32);
        ok = ok && eq_data(back, {0.0f, 1.0f, 2.0f, 3.0f});
    }
    report("integer_casts", ok);
}

static void test_low_precision_casts() {
    Tensor a = make_f32({4}, {1.5f, -2.0f, 0.5f, 0.0f});
    Tensor bf = a.to(DType::BF16).to(DType::F32);
    Tensor fp8 = a.to(DType::FP8_E5M2).to(DType::F32);
    Tensor mx = make_f32({32}, std::vector<float>(32, 1.0f)).to(DType::MXFP4).to(DType::F32);
    bool ok = eq_data(bf, {1.5f, -2.0f, 0.5f, 0.0f}, 0.01f);
    ok = ok && eq_data(fp8, {1.5f, -2.0f, 0.5f, 0.0f}, 0.01f);
    ok = ok && eq_data(mx, std::vector<float>(32, 1.0f), 0.01f);
    report("low_precision_casts", ok);
}

static void test_views() {
    std::vector<float> v(24);
    for (int i = 0; i < 24; ++i) v[i] = static_cast<float>(i);
    Tensor a = make_f32({2, 3, 4}, v);
    Tensor p = a.permute({2, 0, 1});
    bool ok = shape_is(p, {4, 2, 3});
    const std::vector<float> pv = f32_data(p);
    ok = ok && pv.size() == 24 && close(pv[0], 0.0f) && close(pv[1], 4.0f) && close(pv[2], 8.0f);
    Tensor c = a.reshape({6, 4});
    ok = ok && shape_is(c, {6, 4}) && eq_data(c, v);
    report("views_permute_reshape", ok);
}

static void test_slices() {
    std::vector<float> v(10);
    for (int i = 0; i < 10; ++i) v[i] = static_cast<float>(i);
    Tensor a = make_f32({10}, v);
    Tensor s = a.slice(0, 1, 8, 2);
    Tensor r = a.slice(0, 8, 1, -2);
    bool ok = eq_data(s, {1, 3, 5, 7}) && eq_data(r, {8, 6, 4, 2});
    report("slices_negative_step", ok);
}

static void test_elementwise_broadcast() {
    Tensor a = make_f32({2, 3}, {1, 2, 3, 4, 5, 6});
    Tensor b = make_f32({3}, {10, 20, 30});
    Tensor c = nn::add(a, b);
    Tensor d = nn::mul(c, b);
    bool ok = shape_is(c, {2, 3}) && eq_data(c, {11, 22, 33, 14, 25, 36});
    ok = ok && eq_data(d, {110, 440, 990, 140, 500, 1080});
    report("elementwise_broadcast", ok);
}

static void test_reductions() {
    Tensor a = make_f32({2, 3}, {1, 2, 3, 4, 5, 6});
    Tensor s = nn::sum(a, 1);
    Tensor m = nn::mean(a, -1);
    report("reductions", eq_data(s, {6, 15}) && close(f32_data(m)[0], 3.5f));
}

static void test_matmul_2d() {
    Tensor a = make_f32({2, 3}, {1, 2, 3, 4, 5, 6});
    Tensor b = make_f32({3, 2}, {7, 8, 9, 10, 11, 12});
    Tensor c = nn::matmul(a, b);
    report("matmul_2d", shape_is(c, {2, 2}) && eq_data(c, {58, 64, 139, 154}));
}

static void test_matmul_batch_and_nd() {
    Tensor a = make_f32({2, 2, 3}, {1, 2, 3, 4, 5, 6, 1, 0, 0, 0, 1, 0});
    Tensor b = make_f32({2, 3, 2}, {1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0});
    Tensor c = nn::matmul(a, b);
    bool ok = shape_is(c, {2, 2, 2}) && eq_data(c, {1, 2, 4, 5, 1, 0, 0, 1});
    Tensor a2 = make_f32({2, 3, 4}, std::vector<float>(24, 1.0f));
    Tensor b2 = make_f32({2, 4, 5}, std::vector<float>(40, 1.0f));
    Tensor c2 = nn::matmul_nd(a2, b2, {0}, {0}, {1, 2}, {1, 2});
    ok = ok && shape_is(c2, {2, 3, 5}) && eq_data(c2, std::vector<float>(30, 4.0f));
    report("matmul_batch_nd", ok);
}

static void test_autograd_add_mul() {
    Tensor a = make_f32({2}, {2.0f, 3.0f}); a.set_requires_grad(true);
    Tensor b = make_f32({2}, {5.0f, 7.0f}); b.set_requires_grad(true);
    Tensor c = nn::add(nn::mul(a, b), a);
    nn::sum(c).backward();
    bool ok = eq_data(a.grad(), {6.0f, 8.0f}) && eq_data(b.grad(), {2.0f, 3.0f});
    report("autograd_add_mul", ok);
}

static void test_autograd_matmul() {
    Tensor a = make_f32({2, 3}, {1, 2, 3, 4, 5, 6}); a.set_requires_grad(true);
    Tensor b = make_f32({3, 2}, {7, 8, 9, 10, 11, 12}); b.set_requires_grad(true);
    nn::sum(nn::matmul(a, b)).backward();
    bool ok = eq_data(a.grad(), {15, 19, 23, 15, 19, 23});
    ok = ok && eq_data(b.grad(), {5, 5, 7, 7, 9, 9});
    report("autograd_matmul", ok);
}

static void test_autograd_view_slice() {
    Tensor x = make_f32({2, 3}, {1, 2, 3, 4, 5, 6}); x.set_requires_grad(true);
    Tensor s = x.slice(1, 0, 2);
    nn::sum(s).backward();
    report("autograd_slice", eq_data(x.grad(), {1, 1, 0, 1, 1, 0}));
}

static void test_stochastic_stats() {
    const float lo = 1.0f;
    const float hi = 1.0078125f;  // bf16 adjacent around 1.0
    const float mid = (lo + hi) * 0.5f;
    std::vector<float> vals(4096, mid);
    Tensor a = make_f32({4096}, vals);
    Tensor q = a.to(DType::BF16, nn::Rounding::Stochastic, 12345);
    Tensor f = q.to(DType::F32);
    const std::vector<float> out = f32_data(f);
    int upper = 0;
    for (float v : out) { if (v > mid) ++upper; }
    report("stochastic_stats", upper > 1500 && upper < 2600);
}

static void test_threads() {
    nn::set_thread_mode(nn::ThreadMode::Development);
    nn::set_num_threads(4);
    int dev = nn::active_workers();
    bool ok = dev >= 1 && dev <= nn::kDevMaxThreads;
    nn::set_thread_mode(nn::ThreadMode::Evaluation);
    nn::set_num_threads(0);
    ok = ok && nn::active_workers() >= 1;
    report("thread_policy", ok);
}

static void test_all_dtype_alloc() {
    bool ok = true;
    for (DType d : {DType::F32, DType::F64, DType::I8, DType::I16, DType::I32, DType::I64,
                    DType::I128, DType::U8, DType::U16, DType::U32, DType::U64, DType::U128,
                    DType::BF16, DType::FP8_E5M2, DType::MXFP4}) {
        Tensor t = Tensor::empty(d == DType::MXFP4 ? Shape{32} : Shape{2, 3}, d);
        ok = ok && t.numel() == (d == DType::MXFP4 ? 32 : 6) && t.dtype() == d;
    }
    report("all_dtype_alloc", ok);
}

static void bench_matmul() {
    const int n = 256;
    std::vector<float> av(static_cast<size_t>(n) * n), bv(static_cast<size_t>(n) * n);
    for (size_t i = 0; i < av.size(); ++i) { av[i] = static_cast<float>((i * 17) % 101) * 0.01f; bv[i] = static_cast<float>((i * 29) % 97) * 0.01f; }
    Tensor a = make_f32({n, n}, av);
    Tensor b = make_f32({n, n}, bv);
    nn::set_thread_mode(nn::ThreadMode::Evaluation);
    nn::set_num_threads(0);
    Tensor warm = nn::matmul(a, b);
    (void)warm;
    const int iters = 3;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) { Tensor c = nn::matmul(a, b); (void)c; }
    auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count() / iters;
    double gflops = (2.0 * n * n * n) / (ms * 1e6);
    std::printf("PERF_MS %.6f\nPERF_GFLOPS %.6f\n", ms, gflops);
}


// ---------------------------------------------------------------------------
// Metric section. Reference values are computed by local scalar reference code
// BEFORE calling the candidate implementation. Candidate timing only wraps
// candidate calls; reference loops are never inside the timed region.
// ---------------------------------------------------------------------------

static std::vector<float> ref_matmul_f32(const std::vector<float>& a, const std::vector<float>& b,
                                         int M, int K, int N) {
    std::vector<float> c(static_cast<size_t>(M) * N, 0.0f);
    for (int i = 0; i < M; ++i)
        for (int k = 0; k < K; ++k)
            for (int j = 0; j < N; ++j)
                c[static_cast<size_t>(i) * N + j] += a[static_cast<size_t>(i) * K + k] * b[static_cast<size_t>(k) * N + j];
    return c;
}

static const float kMxTable[8] = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f};

static float mx_quant_one(float x, float scale, std::mt19937_64* rng) {
    const float q = x / scale;
    const float aq = std::fabs(q);
    int code = 0;
    if (aq >= kMxTable[7]) {
        code = 7;
    } else {
        int lo = 0, hi = 1;
        for (int c = 0; c < 7; ++c) if (kMxTable[c] <= aq && aq <= kMxTable[c + 1]) { lo = c; hi = c + 1; break; }
        const float t = (aq - kMxTable[lo]) / (kMxTable[hi] - kMxTable[lo]);
        if (rng != nullptr && t > 0.0f && t < 1.0f) {
            std::uniform_real_distribution<double> d(0.0, 1.0);
            code = d(*rng) < t ? hi : lo;
        } else {
            code = t < 0.5f ? lo : hi;
        }
    }
    const float v = kMxTable[code] * scale;
    return q < 0 ? -v : v;
}

static std::vector<float> mx_quantize(const std::vector<float>& x, std::mt19937_64* rng) {
    std::vector<float> out(x.size());
    for (size_t base = 0; base < x.size(); base += 32) {
        float mx = 0.0f;
        for (size_t j = base; j < std::min(x.size(), base + 32); ++j) mx = std::max(mx, std::fabs(x[j]));
        int e = 127;
        if (mx > 0) e = static_cast<int>(std::floor(std::log2(mx / 4.0f))) + 127;
        e = std::max(0, std::min(254, e));
        const float scale = std::pow(2.0f, e - 127);
        for (size_t j = base; j < std::min(x.size(), base + 32); ++j) out[j] = mx_quant_one(x[j], scale, rng);
    }
    return out;
}

static std::vector<float> ref_matmul_mx(const std::vector<float>& a, const std::vector<float>& b,
                                        int M, int K, int N, std::mt19937_64* rng) {
    std::vector<float> aq = mx_quantize(a, rng), bq = mx_quantize(b, rng);
    std::vector<float> c = ref_matmul_f32(aq, bq, M, K, N);
    return mx_quantize(c, rng);
}

static std::vector<__int128> ref_matmul_i128(const std::vector<__int128>& a,
                                             const std::vector<__int128>& b, int M, int K, int N) {
    std::vector<__int128> c(static_cast<size_t>(M) * N, 0);
    for (int i = 0; i < M; ++i)
        for (int k = 0; k < K; ++k)
            for (int j = 0; j < N; ++j)
                c[static_cast<size_t>(i) * N + j] += a[static_cast<size_t>(i) * K + k] * b[static_cast<size_t>(k) * N + j];
    return c;
}

static double mean_abs_error_f32(const std::vector<float>& got, const std::vector<float>& ref) {
    double s = 0.0;
    for (size_t i = 0; i < got.size(); ++i) s += std::fabs(static_cast<double>(got[i]) - ref[i]);
    return got.empty() ? 0.0 : s / got.size();
}

static double mean_abs_error_i128(const std::vector<__int128>& got, const std::vector<__int128>& ref) {
    double s = 0.0;
    for (size_t i = 0; i < got.size(); ++i) {
        __int128 d = got[i] - ref[i];
        if (d < 0) d = -d;
        s += static_cast<double>(d);
    }
    return got.empty() ? 0.0 : s / got.size();
}

static std::vector<__int128> read_i128(const Tensor& t) {
    Tensor c = t.contiguous();
    std::vector<__int128> out(static_cast<size_t>(c.numel()), 0);
    if (c.dtype() != DType::I128) return out;
    std::memcpy(out.data(), c.data(), out.size() * sizeof(__int128));
    return out;
}

static void emit_metric(const char* dtype, int stochastic, int M, int K, int N,
                        double fwd_acc, double fwd_err, double fwd_ms,
                        double bwd_acc, double bwd_err, double bwd_ms) {
    const double ops = 2.0 * M * K * N;
    const double fwd_gflops = fwd_ms > 0 ? ops / (fwd_ms * 1e6) : -1.0;
    const double bwd_gflops = bwd_ms > 0 ? ops / (bwd_ms * 1e6) : -1.0;
    std::printf("METRIC dtype=%s stochastic=%d m=%d k=%d n=%d fwd_accuracy=%.6f fwd_mean_err=%.9g fwd_ms=%.6f fwd_gflops=%.6f bwd_accuracy=%.6f bwd_mean_err=%.9g bwd_ms=%.6f bwd_gflops=%.6f\n",
                dtype, stochastic, M, K, N, fwd_acc, fwd_err, fwd_ms, fwd_gflops,
                bwd_acc, bwd_err, bwd_ms, bwd_gflops);
}

static double time_once_ms(const std::function<void()>& fn, int iters = 3) {
    // warmup
    fn();
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) fn();
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count() / iters;
}

static void fill_rand(std::vector<float>& v, uint64_t seed, float lo, float hi) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<float> d(lo, hi);
    for (float& x : v) x = d(rng);
}

struct StatScore {
    double accuracy = 0.0;
    double mean_err = 0.0;
    double std_ratio = 0.0;
    double ref_mean_avg = 0.0;
    double ref_std_avg = 0.0;
    double cand_mean_avg = 0.0;
    double cand_std_avg = 0.0;
};

static void add_stats(std::vector<double>& sum, std::vector<double>& sq, const std::vector<float>& v) {
    for (size_t i = 0; i < v.size(); ++i) {
        sum[i] += v[i];
        sq[i] += static_cast<double>(v[i]) * v[i];
    }
}

static StatScore score_stats(const std::vector<double>& ref_sum, const std::vector<double>& ref_sq,
                             const std::vector<double>& cand_sum, const std::vector<double>& cand_sq,
                             int samples) {
    StatScore out;
    const double n = std::max(1, samples);
    int ok = 0;
    for (size_t i = 0; i < ref_sum.size(); ++i) {
        const double rm = ref_sum[i] / n;
        const double rv = std::max(0.0, ref_sq[i] / n - rm * rm);
        const double rs = std::sqrt(rv);
        const double cm = cand_sum[i] / n;
        const double cv = std::max(0.0, cand_sq[i] / n - cm * cm);
        const double cs = std::sqrt(cv);
        bool good = false;
        if (rs <= 1e-12) good = std::fabs(cm - rm) <= 1e-6 && cs <= 1e-6;
        else good = std::fabs(cm - rm) <= 3.0 * rs && cs >= 0.1 * rs && cs <= 10.0 * rs;
        if (good) ++ok;
        out.mean_err += std::fabs(cm - rm);
        out.std_ratio += (rs > 1e-12) ? cs / rs : 1.0;
        out.ref_mean_avg += rm;
        out.ref_std_avg += rs;
        out.cand_mean_avg += cm;
        out.cand_std_avg += cs;
    }
    const double count = static_cast<double>(ref_sum.size());
    out.accuracy = 100.0 * ok / count;
    out.mean_err /= count;
    out.std_ratio /= count;
    out.ref_mean_avg /= count;
    out.ref_std_avg /= count;
    out.cand_mean_avg /= count;
    out.cand_std_avg /= count;
    return out;
}

static void metric_f32() {
    const int M = 256, K = 256, N = 256;
    const int DATA = 4;
    double acc_f = 0.0, err_f = 0.0, ms_f = 0.0, acc_b = 0.0, err_b = 0.0, ms_b = 0.0;
    for (int d = 0; d < DATA; ++d) {
        std::vector<float> a(static_cast<size_t>(M) * K), b(static_cast<size_t>(K) * N);
        fill_rand(a, 1000 + d, -1.0f, 1.0f);
        fill_rand(b, 2000 + d, -1.0f, 1.0f);
        const std::vector<float> ref_c = ref_matmul_f32(a, b, M, K, N);
        std::vector<float> ref_da(static_cast<size_t>(M) * K), ref_db(static_cast<size_t>(K) * N);
        for (int i = 0; i < M; ++i) for (int k = 0; k < K; ++k) { float s = 0; for (int j = 0; j < N; ++j) s += b[k * N + j]; ref_da[i * K + k] = s; }
        for (int k = 0; k < K; ++k) for (int j = 0; j < N; ++j) { float s = 0; for (int i = 0; i < M; ++i) s += a[i * K + k]; ref_db[k * N + j] = s; }

        Tensor at = make_f32({M, K}, a), bt = make_f32({K, N}, b);
        const std::vector<float> got = f32_data(nn::matmul(at, bt));
        double err = mean_abs_error_f32(got, ref_c);
        int ok = 0; for (size_t i = 0; i < got.size(); ++i) if (std::fabs(got[i] - ref_c[i]) <= 1e-3f + 1e-4f * std::fabs(ref_c[i])) ++ok;
        acc_f += 100.0 * ok / got.size(); err_f += err;
        ms_f += time_once_ms([&] { Tensor c = nn::matmul(at, bt); (void)c; });

        Tensor ag = make_f32({M, K}, a), bg = make_f32({K, N}, b);
        ag.set_requires_grad(true); bg.set_requires_grad(true);
        ms_b += time_once_ms([&] {
            ag = make_f32({M, K}, a); bg = make_f32({K, N}, b);
            ag.set_requires_grad(true); bg.set_requires_grad(true);
            Tensor c = nn::matmul(ag, bg);
            nn::sum(c).backward();
        });
        const std::vector<float> got_da = f32_data(ag.grad()), got_db = f32_data(bg.grad());
        double err_da = mean_abs_error_f32(got_da, ref_da), err_db = mean_abs_error_f32(got_db, ref_db);
        int ok_da = 0, ok_db = 0;
        for (size_t i = 0; i < got_da.size(); ++i) if (std::fabs(got_da[i] - ref_da[i]) <= 1e-3f) ++ok_da;
        for (size_t i = 0; i < got_db.size(); ++i) if (std::fabs(got_db[i] - ref_db[i]) <= 1e-3f) ++ok_db;
        acc_b += 100.0 * (ok_da + ok_db) / (got_da.size() + got_db.size());
        err_b += (err_da + err_db) / 2.0;
    }
    emit_metric("f32", 0, M, K, N, acc_f / DATA, err_f / DATA, ms_f / DATA, acc_b / DATA, err_b / DATA, ms_b / DATA);
}

static void metric_i128() {
    const int M = 256, K = 256, N = 256;
    const int DATA = 4;
    double acc_f = 0.0, err_f = 0.0, ms_f = 0.0, acc_b = 0.0, err_b = 0.0, ms_b = 0.0;
    for (int d = 0; d < DATA; ++d) {
        std::vector<__int128> a(static_cast<size_t>(M) * K), b(static_cast<size_t>(K) * N);
        for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<int>((i * 17 + d * 13) % 9) - 4;
        for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<int>((i * 29 + d * 7) % 9) - 4;
        const std::vector<__int128> ref_c = ref_matmul_i128(a, b, M, K, N);
        std::vector<__int128> ref_da(static_cast<size_t>(M) * K), ref_db(static_cast<size_t>(K) * N);
        for (int i = 0; i < M; ++i) for (int k = 0; k < K; ++k) { __int128 s = 0; for (int j = 0; j < N; ++j) s += b[k * N + j]; ref_da[i * K + k] = s; }
        for (int k = 0; k < K; ++k) for (int j = 0; j < N; ++j) { __int128 s = 0; for (int i = 0; i < M; ++i) s += a[i * K + k]; ref_db[k * N + j] = s; }

        Tensor at = Tensor::from_data(a.data(), {M, K}, DType::I128);
        Tensor bt = Tensor::from_data(b.data(), {K, N}, DType::I128);
        const std::vector<__int128> got = read_i128(nn::matmul(at, bt));
        double err = mean_abs_error_i128(got, ref_c);
        int ok = 0; for (size_t i = 0; i < got.size(); ++i) if (got[i] == ref_c[i]) ++ok;
        acc_f += 100.0 * ok / got.size(); err_f += err;
        ms_f += time_once_ms([&] { Tensor c = nn::matmul(at, bt); (void)c; });

        Tensor ag = Tensor::from_data(a.data(), {M, K}, DType::I128);
        Tensor bg = Tensor::from_data(b.data(), {K, N}, DType::I128);
        ag.set_requires_grad(true); bg.set_requires_grad(true);
        ms_b += time_once_ms([&] {
            ag = Tensor::from_data(a.data(), {M, K}, DType::I128);
            bg = Tensor::from_data(b.data(), {K, N}, DType::I128);
            ag.set_requires_grad(true); bg.set_requires_grad(true);
            Tensor c = nn::matmul(ag, bg);
            nn::sum(c).backward();
        });
        const std::vector<__int128> got_da = read_i128(ag.grad()), got_db = read_i128(bg.grad());
        double err_da = mean_abs_error_i128(got_da, ref_da), err_db = mean_abs_error_i128(got_db, ref_db);
        int ok_da = 0, ok_db = 0;
        for (size_t i = 0; i < got_da.size(); ++i) if (got_da[i] == ref_da[i]) ++ok_da;
        for (size_t i = 0; i < got_db.size(); ++i) if (got_db[i] == ref_db[i]) ++ok_db;
        acc_b += 100.0 * (ok_da + ok_db) / (got_da.size() + got_db.size());
        err_b += (err_da + err_db) / 2.0;
    }
    emit_metric("i128", 0, M, K, N, acc_f / DATA, err_f / DATA, ms_f / DATA, acc_b / DATA, err_b / DATA, ms_b / DATA);
}

static void metric_mx() {
    const int M = 256, K = 256, N = 256;
    const int DATA = 4;
    double acc_f = 0.0, err_f = 0.0, ms_f = 0.0, acc_b = 0.0, err_b = 0.0, ms_b = 0.0;
    for (int d = 0; d < DATA; ++d) {
        std::vector<float> a(static_cast<size_t>(M) * K), b(static_cast<size_t>(K) * N);
        fill_rand(a, 3000 + d, -2.0f, 2.0f);
        fill_rand(b, 4000 + d, -2.0f, 2.0f);
        std::vector<float> aq = mx_quantize(a, nullptr), bq = mx_quantize(b, nullptr);
        const std::vector<float> ref_c = mx_quantize(ref_matmul_f32(aq, bq, M, K, N), nullptr);
        std::vector<float> ref_da(static_cast<size_t>(M) * K), ref_db(static_cast<size_t>(K) * N);
        for (int i = 0; i < M; ++i) for (int k = 0; k < K; ++k) { float s = 0; for (int j = 0; j < N; ++j) s += bq[k * N + j]; ref_da[i * K + k] = s; }
        for (int k = 0; k < K; ++k) for (int j = 0; j < N; ++j) { float s = 0; for (int i = 0; i < M; ++i) s += aq[i * K + k]; ref_db[k * N + j] = s; }
        ref_da = mx_quantize(ref_da, nullptr); ref_db = mx_quantize(ref_db, nullptr);

        Tensor at = make_f32({M, K}, a).to(DType::MXFP4);
        Tensor bt = make_f32({K, N}, b).to(DType::MXFP4);
        const std::vector<float> got = f32_data(nn::matmul(at, bt).to(DType::F32));
        double err = mean_abs_error_f32(got, ref_c);
        int ok = 0; for (size_t i = 0; i < got.size(); ++i) if (std::fabs(got[i] - ref_c[i]) <= 0.75f + 0.1f * std::fabs(ref_c[i])) ++ok;
        acc_f += 100.0 * ok / got.size(); err_f += err;
        ms_f += time_once_ms([&] { Tensor c = nn::matmul(at, bt); (void)c; });

        Tensor ag = make_f32({M, K}, a).to(DType::MXFP4), bg = make_f32({K, N}, b).to(DType::MXFP4);
        ag.set_requires_grad(true); bg.set_requires_grad(true);
        ms_b += time_once_ms([&] {
            ag = make_f32({M, K}, a).to(DType::MXFP4); bg = make_f32({K, N}, b).to(DType::MXFP4);
            ag.set_requires_grad(true); bg.set_requires_grad(true);
            Tensor c = nn::matmul(ag, bg);
            nn::sum(c).backward();
        });
        const std::vector<float> got_da = f32_data(ag.grad().to(DType::F32)), got_db = f32_data(bg.grad().to(DType::F32));
        double err_da = mean_abs_error_f32(got_da, ref_da), err_db = mean_abs_error_f32(got_db, ref_db);
        int ok_da = 0, ok_db = 0;
        for (size_t i = 0; i < got_da.size(); ++i) if (std::fabs(got_da[i] - ref_da[i]) <= 0.75f + 0.1f * std::fabs(ref_da[i])) ++ok_da;
        for (size_t i = 0; i < got_db.size(); ++i) if (std::fabs(got_db[i] - ref_db[i]) <= 0.75f + 0.1f * std::fabs(ref_db[i])) ++ok_db;
        acc_b += 100.0 * (ok_da + ok_db) / (got_da.size() + got_db.size());
        err_b += (err_da + err_db) / 2.0;
    }
    emit_metric("mxfp4", 0, M, K, N, acc_f / DATA, err_f / DATA, ms_f / DATA, acc_b / DATA, err_b / DATA, ms_b / DATA);

    const int STOCH_DATA = 2;
    const int samples = 1024;
    double s_acc_f = 0.0, s_err_f = 0.0, s_ms_f = 0.0, s_acc_b = 0.0, s_err_b = 0.0, s_ms_b = 0.0;
    double s_ref_mean = 0.0, s_ref_std = 0.0, s_cand_mean = 0.0, s_cand_std = 0.0;
    for (int d = 0; d < STOCH_DATA; ++d) {
        std::vector<float> a(static_cast<size_t>(M) * K), b(static_cast<size_t>(K) * N);
        fill_rand(a, 5000 + d, -2.0f, 2.0f);
        fill_rand(b, 6000 + d, -2.0f, 2.0f);
        const size_t out_n = static_cast<size_t>(M) * N;
        std::vector<double> ref_sum(out_n, 0.0), ref_sq(out_n, 0.0), cand_sum(out_n, 0.0), cand_sq(out_n, 0.0);
        std::vector<double> ref_da_sum(static_cast<size_t>(M) * K, 0.0), ref_da_sq(ref_da_sum.size(), 0.0);
        std::vector<double> ref_db_sum(static_cast<size_t>(K) * N, 0.0), ref_db_sq(ref_db_sum.size(), 0.0);
        std::vector<double> cand_da_sum(ref_da_sum.size(), 0.0), cand_da_sq(ref_da_sum.size(), 0.0);
        std::vector<double> cand_db_sum(ref_db_sum.size(), 0.0), cand_db_sq(ref_db_sum.size(), 0.0);
        std::mt19937_64 ref_rng(0xabcdefULL + d);
        for (int t = 0; t < samples; ++t) {
            const std::vector<float> raq = mx_quantize(a, &ref_rng), rbq = mx_quantize(b, &ref_rng);
            const std::vector<float> rc = mx_quantize(ref_matmul_f32(raq, rbq, M, K, N), &ref_rng);
            add_stats(ref_sum, ref_sq, rc);
            std::vector<float> rda(static_cast<size_t>(M) * K), rdb(static_cast<size_t>(K) * N);
            for (int i = 0; i < M; ++i) for (int k = 0; k < K; ++k) { float s = 0; for (int j = 0; j < N; ++j) s += rbq[k * N + j]; rda[i * K + k] = s; }
            for (int k = 0; k < K; ++k) for (int j = 0; j < N; ++j) { float s = 0; for (int i = 0; i < M; ++i) s += raq[i * K + k]; rdb[k * N + j] = s; }
            rda = mx_quantize(rda, nullptr); rdb = mx_quantize(rdb, nullptr);
            add_stats(ref_da_sum, ref_da_sq, rda); add_stats(ref_db_sum, ref_db_sq, rdb);

            const uint64_t seed = 100000 + static_cast<uint64_t>(d) * samples + t;
            Tensor qa = make_f32({M, K}, a).to(DType::MXFP4, nn::Rounding::Stochastic, seed);
            Tensor qb = make_f32({K, N}, b).to(DType::MXFP4, nn::Rounding::Stochastic, seed + 7919);
            Tensor c = nn::matmul(qa, qb);
            add_stats(cand_sum, cand_sq, f32_data(c.to(DType::F32)));
            qa.set_requires_grad(true); qb.set_requires_grad(true);
            Tensor cg = nn::matmul(qa, qb);
            nn::sum(cg).backward();
            add_stats(cand_da_sum, cand_da_sq, f32_data(qa.grad().to(DType::F32)));
            add_stats(cand_db_sum, cand_db_sq, f32_data(qb.grad().to(DType::F32)));
        }
        StatScore f = score_stats(ref_sum, ref_sq, cand_sum, cand_sq, samples);
        StatScore bscore = score_stats(ref_da_sum, ref_da_sq, cand_da_sum, cand_da_sq, samples);
        StatScore bs2 = score_stats(ref_db_sum, ref_db_sq, cand_db_sum, cand_db_sq, samples);
        s_acc_f += f.accuracy; s_err_f += f.mean_err;
        s_acc_b += (bscore.accuracy + bs2.accuracy) / 2.0;
        s_err_b += (bscore.mean_err + bs2.mean_err) / 2.0;
        s_ref_mean += f.ref_mean_avg; s_ref_std += f.ref_std_avg;
        s_cand_mean += f.cand_mean_avg; s_cand_std += f.cand_std_avg;
        s_ms_f += time_once_ms([&] {
            Tensor qa = make_f32({M, K}, a).to(DType::MXFP4, nn::Rounding::Stochastic, 7);
            Tensor qb = make_f32({K, N}, b).to(DType::MXFP4, nn::Rounding::Stochastic, 11);
            Tensor c = nn::matmul(qa, qb); (void)c;
        }, 1);
        s_ms_b += time_once_ms([&] {
            Tensor qa = make_f32({M, K}, a).to(DType::MXFP4, nn::Rounding::Stochastic, 7);
            Tensor qb = make_f32({K, N}, b).to(DType::MXFP4, nn::Rounding::Stochastic, 11);
            qa.set_requires_grad(true); qb.set_requires_grad(true);
            Tensor c = nn::matmul(qa, qb);
            nn::sum(c).backward();
        }, 1);
        std::printf("STAT data=%d samples=%d fwd_acc=%.6f fwd_mean_err=%.9g fwd_std_ratio=%.9g bwd_acc=%.6f bwd_mean_err=%.9g bwd_std_ratio=%.9g ref_mean=%.9g ref_std=%.9g cand_mean=%.9g cand_std=%.9g\n",
                    d, samples, f.accuracy, f.mean_err, f.std_ratio, (bscore.accuracy + bs2.accuracy) / 2.0,
                    (bscore.mean_err + bs2.mean_err) / 2.0, (bscore.std_ratio + bs2.std_ratio) / 2.0,
                    f.ref_mean_avg, f.ref_std_avg, f.cand_mean_avg, f.cand_std_avg);
    }
    emit_metric("mxfp4", 1, M, K, N, s_acc_f / STOCH_DATA, s_err_f / STOCH_DATA, s_ms_f / STOCH_DATA,
                s_acc_b / STOCH_DATA, s_err_b / STOCH_DATA, s_ms_b / STOCH_DATA);
    std::printf("STAT dtype=mxfp4 stochastic=1 data=%d samples=%d ref_mean_avg=%.9g ref_std_avg=%.9g cand_mean_avg=%.9g cand_std_avg=%.9g\n",
                STOCH_DATA, samples, s_ref_mean / STOCH_DATA, s_ref_std / STOCH_DATA,
                s_cand_mean / STOCH_DATA, s_cand_std / STOCH_DATA);
}


int main() {
    test_dtype_metadata();
    test_casts_float();
    test_casts_int();
    test_integer_casts();
    test_low_precision_casts();
    test_views();
    test_slices();
    test_elementwise_broadcast();
    test_reductions();
    test_matmul_2d();
    test_matmul_batch_and_nd();
    test_autograd_add_mul();
    test_autograd_matmul();
    test_autograd_view_slice();
    test_stochastic_stats();
    test_threads();
    test_all_dtype_alloc();
    bench_matmul();
    try { metric_f32(); } catch (const std::exception& e) { std::printf("METRIC dtype=f32 stochastic=0 error=%s\n", e.what()); }
    try { metric_i128(); } catch (const std::exception& e) { std::printf("METRIC dtype=i128 stochastic=0 error=%s\n", e.what()); }
    try { metric_mx(); } catch (const std::exception& e) { std::printf("METRIC dtype=mxfp4 stochastic=0 error=%s\n", e.what()); }
    std::printf("SUMMARY %d %d\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
