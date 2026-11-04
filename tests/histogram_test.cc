#include "util/histogram.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

namespace strata {

TEST(HistogramTest, Empty) {
  Histogram h;
  EXPECT_DOUBLE_EQ(h.num(), 0.0);
  EXPECT_DOUBLE_EQ(h.Min(), 0.0);
  EXPECT_DOUBLE_EQ(h.Max(), 0.0);
  EXPECT_DOUBLE_EQ(h.Average(), 0.0);
  EXPECT_DOUBLE_EQ(h.Median(), 0.0);
  EXPECT_DOUBLE_EQ(h.StandardDeviation(), 0.0);
  EXPECT_DOUBLE_EQ(h.Percentile(0.0), 0.0);
  EXPECT_DOUBLE_EQ(h.Percentile(50.0), 0.0);
  EXPECT_DOUBLE_EQ(h.Percentile(99.0), 0.0);
  EXPECT_DOUBLE_EQ(h.Percentile(100.0), 0.0);
  EXPECT_FALSE(h.ToString().empty());
}

TEST(HistogramTest, SingleValue) {
  Histogram h;
  for (int i = 0; i < 100; ++i) {
    h.Add(42.5);
  }
  EXPECT_DOUBLE_EQ(h.num(), 100.0);
  EXPECT_DOUBLE_EQ(h.Min(), 42.5);
  EXPECT_DOUBLE_EQ(h.Max(), 42.5);
  EXPECT_NEAR(h.Average(), 42.5, 1e-9);
  EXPECT_NEAR(h.Median(), 42.5, 1e-9);
  EXPECT_NEAR(h.StandardDeviation(), 0.0, 1e-9);
  EXPECT_NEAR(h.Percentile(50.0), 42.5, 1e-9);
  EXPECT_NEAR(h.Percentile(99.0), 42.5, 1e-9);
}

TEST(HistogramTest, SyntheticUniformData) {
  Histogram h;
  const int n = 100000;
  for (int i = 1; i <= n; ++i) {
    // 0.01 to 1000.0 us
    double val = (1000.0 * i) / n;
    h.Add(val);
  }

  EXPECT_DOUBLE_EQ(h.num(), n);
  EXPECT_NEAR(h.Min(), 0.01, 1e-4);
  EXPECT_DOUBLE_EQ(h.Max(), 1000.0);
  EXPECT_NEAR(h.Average(), 500.0, 1.0);
  // StdDev for uniform (0, 1000) is 1000 / sqrt(12) ~= 288.675
  EXPECT_NEAR(h.StandardDeviation(), 288.675, 2.0);

  // Bucket resolution is ~10-12%, so percentiles should be well within 5% error.
  double p50 = h.Percentile(50.0);
  EXPECT_NEAR(p50, 500.0, 500.0 * 0.05);

  double p90 = h.Percentile(90.0);
  EXPECT_NEAR(p90, 900.0, 900.0 * 0.05);

  double p99 = h.Percentile(99.0);
  EXPECT_NEAR(p99, 990.0, 990.0 * 0.05);

  double p999 = h.Percentile(99.9);
  EXPECT_NEAR(p999, 999.0, 999.0 * 0.05);

  // Bounds
  EXPECT_EQ(h.Percentile(-5.0), h.Min());
  EXPECT_EQ(h.Percentile(105.0), h.Max());
}

TEST(HistogramTest, Merge) {
  Histogram h1;
  Histogram h2;
  Histogram h_all;

  for (int i = 1; i <= 50000; ++i) {
    double val = static_cast<double>(i);
    h1.Add(val);
    h_all.Add(val);
  }
  for (int i = 50001; i <= 100000; ++i) {
    double val = static_cast<double>(i);
    h2.Add(val);
    h_all.Add(val);
  }

  h1.Merge(h2);

  EXPECT_DOUBLE_EQ(h1.num(), h_all.num());
  EXPECT_DOUBLE_EQ(h1.Min(), h_all.Min());
  EXPECT_DOUBLE_EQ(h1.Max(), h_all.Max());
  EXPECT_NEAR(h1.Average(), h_all.Average(), 1e-6);
  EXPECT_NEAR(h1.StandardDeviation(), h_all.StandardDeviation(), 1e-6);
  EXPECT_NEAR(h1.Percentile(50.0), h_all.Percentile(50.0), 1e-6);
  EXPECT_NEAR(h1.Percentile(99.0), h_all.Percentile(99.0), 1e-6);
  EXPECT_NEAR(h1.Percentile(99.9), h_all.Percentile(99.9), 1e-6);

  // Merging with empty histogram is a no-op
  Histogram h_empty;
  h1.Merge(h_empty);
  EXPECT_DOUBLE_EQ(h1.num(), h_all.num());

  // Merging empty with non-empty adopts non-empty
  Histogram h_fresh;
  h_fresh.Merge(h2);
  EXPECT_DOUBLE_EQ(h_fresh.num(), h2.num());
  EXPECT_DOUBLE_EQ(h_fresh.Min(), h2.Min());
  EXPECT_DOUBLE_EQ(h_fresh.Max(), h2.Max());
}

TEST(HistogramTest, Clear) {
  Histogram h;
  h.Add(10.0);
  h.Add(20.0);
  EXPECT_DOUBLE_EQ(h.num(), 2.0);
  h.Clear();
  EXPECT_DOUBLE_EQ(h.num(), 0.0);
  EXPECT_DOUBLE_EQ(h.Min(), 0.0);
  EXPECT_DOUBLE_EQ(h.Max(), 0.0);
  EXPECT_DOUBLE_EQ(h.Average(), 0.0);
}

TEST(HistogramTest, AddAfterClear) {
  Histogram h;
  h.Add(100.0);
  h.Clear();
  EXPECT_DOUBLE_EQ(h.num(), 0.0);

  h.Add(25.0);
  h.Add(75.0);
  EXPECT_DOUBLE_EQ(h.num(), 2.0);
  EXPECT_DOUBLE_EQ(h.Min(), 25.0);
  EXPECT_DOUBLE_EQ(h.Max(), 75.0);
  EXPECT_NEAR(h.Average(), 50.0, 1e-6);
}

TEST(HistogramTest, NaNExtremeBounds) {
  Histogram h;
  h.Add(10.0);
  h.Add(20.0);

  double nan_val = std::numeric_limits<double>::quiet_NaN();
  EXPECT_DOUBLE_EQ(h.Percentile(nan_val), 0.0);
  EXPECT_DOUBLE_EQ(h.Percentile(-1.0), h.Min());
  EXPECT_DOUBLE_EQ(h.Percentile(101.0), h.Max());
}

TEST(HistogramTest, ExactBucketBoundaries) {
  Histogram h;
  const double boundaries[] = {0.1, 1.0, 2.0, 10.0, 100.0, 1000.0, 10000.0};
  for (double b : boundaries) {
    h.Add(b);
  }
  EXPECT_DOUBLE_EQ(h.num(), 7.0);
  EXPECT_DOUBLE_EQ(h.Min(), 0.1);
  EXPECT_DOUBLE_EQ(h.Max(), 10000.0);
  EXPECT_GT(h.Percentile(50.0), 0.1);
  EXPECT_LT(h.Percentile(50.0), 10000.0);

  std::string str = h.ToString();
  EXPECT_FALSE(str.empty());
  EXPECT_NE(str.find("Percentiles:"), std::string::npos);
}

TEST(HistogramTest, MergeTakesTheWiderRange) {
  Histogram narrow;
  narrow.Add(100.0);
  Histogram wide;
  wide.Add(10.0);
  wide.Add(1000.0);

  narrow.Merge(wide);

  EXPECT_DOUBLE_EQ(narrow.Min(), 10.0);
  EXPECT_DOUBLE_EQ(narrow.Max(), 1000.0);
}

TEST(HistogramTest, PercentileInterpolatesWithinABucket) {
  // Evenly spread across one bucket, [100, 110), so interpolation decides the answer.
  Histogram h;
  for (int i = 0; i < 1000; ++i) {
    h.Add(100.0 + i * 0.01);
  }
  EXPECT_NEAR(h.Percentile(25.0), 102.5, 0.01);
  EXPECT_NEAR(h.Percentile(50.0), 105.0, 0.01);
  EXPECT_NEAR(h.Percentile(75.0), 107.5, 0.01);
}

TEST(HistogramTest, PercentileStaysWithinObservedValues) {
  // Interpolation alone would give about 40.05 and 44.95 for values in [40, 45).
  Histogram h;
  for (int i = 0; i < 100; ++i) {
    h.Add(42.5);
  }
  EXPECT_DOUBLE_EQ(h.Percentile(1.0), 42.5);
  EXPECT_DOUBLE_EQ(h.Percentile(99.0), 42.5);
}

TEST(HistogramTest, ValueOnABoundaryBelongsToTheBucketAboveIt) {
  // 10 is the bottom of [10, 11). Counted in [9.5, 10) instead, the median would
  // interpolate below 10 and be clamped to exactly 10.
  Histogram h;
  for (int i = 0; i < 100; ++i) {
    h.Add(10.0);
  }
  h.Add(1000.0);
  EXPECT_GT(h.Percentile(50.0), 10.0);
  EXPECT_LT(h.Percentile(50.0), 11.0);
}

TEST(HistogramTest, ValuesAboveTheLastBucketStillInterpolateUpToTheMax) {
  // Past the last limit (1e9 us) the only upper bound available is the maximum.
  Histogram h;
  for (int i = 0; i < 50; ++i) {
    h.Add(1.5e9);
    h.Add(3.0e9);
  }
  EXPECT_NEAR(h.Percentile(75.0), 2.5e9, 1e6);
  EXPECT_DOUBLE_EQ(h.Max(), 3.0e9);
}

TEST(HistogramTest, NegativeValuesCountAsZero) {
  Histogram h;
  h.Add(-5.0);
  EXPECT_DOUBLE_EQ(h.num(), 1.0);
  EXPECT_DOUBLE_EQ(h.Min(), 0.0);
  EXPECT_DOUBLE_EQ(h.Average(), 0.0);
}

}  // namespace strata
