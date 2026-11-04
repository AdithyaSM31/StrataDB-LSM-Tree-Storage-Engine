#include "util/histogram.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace strata {

namespace {

const std::vector<double>& BucketLimits() {
  static const auto limits = []() {
    std::vector<double> v;
    const double kSteps[] = {1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 2.0,
                             2.2, 2.4, 2.6, 2.8, 3.0, 3.2, 3.4, 3.6, 3.8, 4.0, 4.5,
                             5.0, 5.5, 6.0, 6.5, 7.0, 7.5, 8.0, 8.5, 9.0, 9.5, 10.0};
    double base = 0.1;
    for (int d = 0; d < 10; ++d) {
      for (double step : kSteps) {
        double limit = base * step;
        if (v.empty() || limit > v.back() * 1.000001) {
          v.push_back(limit);
        }
      }
      base *= 10.0;
    }
    return v;
  }();
  return limits;
}

}  // namespace

Histogram::Histogram() {
  buckets_.resize(BucketLimits().size() + 1, 0.0);
  Clear();
}

double Histogram::BucketLower(size_t i) {
  if (i == 0) {
    return 0.0;
  }
  const auto& limits = BucketLimits();
  if (i - 1 < limits.size()) {
    return limits[i - 1];
  }
  return limits.back();
}

double Histogram::BucketUpper(size_t i) {
  const auto& limits = BucketLimits();
  if (i < limits.size()) {
    return limits[i];
  }
  return std::numeric_limits<double>::infinity();
}

void Histogram::Clear() {
  min_ = std::numeric_limits<double>::max();
  max_ = 0.0;
  num_ = 0.0;
  sum_ = 0.0;
  sum_squares_ = 0.0;
  std::fill(buckets_.begin(), buckets_.end(), 0.0);
}

void Histogram::Add(double value_micros) {
  if (value_micros < 0.0) {
    value_micros = 0.0;
  }
  if (value_micros < min_) {
    min_ = value_micros;
  }
  if (value_micros > max_) {
    max_ = value_micros;
  }
  num_ += 1.0;
  sum_ += value_micros;
  sum_squares_ += value_micros * value_micros;

  const auto& limits = BucketLimits();
  auto it = std::upper_bound(limits.begin(), limits.end(), value_micros);
  size_t idx = std::distance(limits.begin(), it);
  if (idx >= buckets_.size()) {
    idx = buckets_.size() - 1;
  }
  buckets_[idx] += 1.0;
}

void Histogram::Merge(const Histogram& other) {
  if (other.num_ <= 0.0) {
    return;
  }
  if (num_ <= 0.0) {
    *this = other;
    return;
  }
  if (other.min_ < min_) {
    min_ = other.min_;
  }
  if (other.max_ > max_) {
    max_ = other.max_;
  }
  num_ += other.num_;
  sum_ += other.sum_;
  sum_squares_ += other.sum_squares_;
  if (buckets_.size() < other.buckets_.size()) {
    buckets_.resize(other.buckets_.size(), 0.0);
  }
  for (size_t i = 0; i < buckets_.size() && i < other.buckets_.size(); ++i) {
    buckets_[i] += other.buckets_[i];
  }
}

double Histogram::Average() const { return (num_ > 0.0) ? (sum_ / num_) : 0.0; }

double Histogram::StandardDeviation() const {
  if (num_ <= 0.0) {
    return 0.0;
  }
  double mean = Average();
  double variance = (sum_squares_ / num_) - (mean * mean);
  if (variance <= 0.0) {
    return 0.0;
  }
  return std::sqrt(variance);
}

double Histogram::Percentile(double p) const {
  if (num_ <= 0.0 || std::isnan(p)) {
    return 0.0;
  }
  if (p <= 0.0) {
    return min_;
  }
  if (p >= 100.0) {
    return max_;
  }

  const auto& limits = BucketLimits();
  double threshold = num_ * (p / 100.0);
  double cumulative = 0.0;
  for (size_t i = 0; i < buckets_.size(); ++i) {
    cumulative += buckets_[i];
    if (cumulative >= threshold) {
      double lower = BucketLower(i);
      double upper = (i < limits.size()) ? limits[i] : max_;
      if (upper < lower) {
        upper = lower;
      }
      double prev = cumulative - buckets_[i];
      double fraction = (buckets_[i] > 0.0) ? ((threshold - prev) / buckets_[i]) : 0.0;
      double result = lower + (upper - lower) * fraction;
      if (result < min_) {
        result = min_;
      }
      if (result > max_) {
        result = max_;
      }
      return result;
    }
  }
  return max_;
}

std::string Histogram::ToString() const {
  std::string r;
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "Count: %.0f  Average: %.4f  StdDev: %.4f\n"
                "Min: %.4f  Median: %.4f  Max: %.4f\n"
                "Percentiles: P50: %.2f P75: %.2f P99: %.2f P99.9: %.2f P99.99: %.2f\n"
                "------------------------------------------------------\n",
                num_, Average(), StandardDeviation(), Min(), Median(), Max(), Percentile(50.0),
                Percentile(75.0), Percentile(99.0), Percentile(99.9), Percentile(99.99));
  r.append(buf);
  if (num_ == 0.0) {
    return r;
  }

  double cum = 0.0;
  for (size_t i = 0; i < buckets_.size(); ++i) {
    if (buckets_[i] == 0.0) {
      continue;
    }
    cum += buckets_[i];
    double lower = BucketLower(i);
    double upper = (i < BucketLimits().size()) ? BucketLimits()[i] : max_;
    std::snprintf(buf, sizeof(buf), "[ %10.2f, %10.2f ) %8.0f %7.3f%% %7.3f%%\n", lower, upper,
                  buckets_[i], (buckets_[i] / num_) * 100.0, (cum / num_) * 100.0);
    r.append(buf);
  }
  return r;
}

}  // namespace strata
