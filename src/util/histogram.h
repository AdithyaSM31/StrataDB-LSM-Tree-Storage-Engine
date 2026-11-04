#ifndef STRATA_UTIL_HISTOGRAM_H_
#define STRATA_UTIL_HISTOGRAM_H_

#include <cstddef>
#include <string>
#include <vector>

namespace strata {

class Histogram {
 public:
  Histogram();
  ~Histogram() = default;

  void Clear();
  void Add(double value_micros);
  void Merge(const Histogram& other);

  double min() const { return num_ > 0.0 ? min_ : 0.0; }
  double max() const { return num_ > 0.0 ? max_ : 0.0; }
  double num() const { return num_; }
  double sum() const { return sum_; }

  double Min() const { return min(); }
  double Max() const { return max(); }
  double Average() const;
  double StandardDeviation() const;
  double Median() const { return Percentile(50.0); }
  double Percentile(double p) const;

  std::string ToString() const;

 private:
  static double BucketLower(size_t i);

  double min_;
  double max_;
  double num_;
  double sum_;
  double sum_squares_;
  std::vector<double> buckets_;
};

}  // namespace strata

#endif  // STRATA_UTIL_HISTOGRAM_H_
