// Neural-CPP public interface. Implement everything declared here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nn {

using Shape = std::vector<int64_t>;

enum class DType : uint8_t {
    F32, F64,
    I8, I16, I32, I64, I128,
    U8, U16, U32, U64, U128,
    BF16, FP8_E5M2, MXFP4
};

enum class Rounding : uint8_t { NearestEven, TowardZero, Stochastic };
enum class ThreadMode : uint8_t { Development, Evaluation };

constexpr int kDevMaxThreads = 4;

std::size_t dtype_size(DType dtype);
const char* dtype_name(DType dtype);

class Tensor {
public:
    Tensor();
    Tensor(const Tensor&) = default;
    Tensor(Tensor&&) noexcept = default;
    Tensor& operator=(const Tensor&) = default;
    Tensor& operator=(Tensor&&) noexcept = default;
    ~Tensor();

    static Tensor empty(const Shape& shape, DType dtype);
    static Tensor from_data(const void* data, const Shape& shape, DType dtype);

    const Shape& shape() const;
    DType dtype() const;
    int64_t ndim() const;
    int64_t numel() const;
    bool is_contiguous() const;

    const void* data() const;
    void* data();

    Tensor to(DType dst, Rounding mode = Rounding::NearestEven, uint64_t seed = 0) const;
    Tensor view(const Shape& shape) const;
    Tensor reshape(const Shape& shape) const;
    Tensor permute(const std::vector<int>& dims) const;
    Tensor slice(int dim, int64_t start, int64_t stop, int64_t step = 1) const;
    Tensor contiguous() const;
    Tensor clone() const;

    void set_requires_grad(bool value);
    bool requires_grad() const;
    Tensor grad() const;
    void backward();

    // Implementation detail. Exposed so the library implementer can define
    // free functions without the header becoming a barrier.
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

Tensor add(const Tensor& a, const Tensor& b);
Tensor sub(const Tensor& a, const Tensor& b);
Tensor mul(const Tensor& a, const Tensor& b);
Tensor div(const Tensor& a, const Tensor& b);
Tensor sum(const Tensor& a, int dim = -1);
Tensor mean(const Tensor& a, int dim = -1);

Tensor matmul(const Tensor& a, const Tensor& b);
Tensor matmul_nd(const Tensor& a, const Tensor& b,
                 const std::vector<int>& a_batch_dims,
                 const std::vector<int>& b_batch_dims,
                 const std::vector<int>& a_matrix_dims,
                 const std::vector<int>& b_matrix_dims);

void set_thread_mode(ThreadMode mode);
void set_num_threads(int n);
int num_threads();
int active_workers();

}  // namespace nn
