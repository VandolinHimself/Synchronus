// Aggregate.h - sample-weighted master measurements for this machine.
// Each timing interval keeps a sum and a sample weight. A new run is folded in
// as value * its sample count. The weight grows by that count, so a later run
// moves the master by n / (weight + n) and older runs matter less.
#pragma once

#include <string>
#include <vector>

struct TimingSample {
    unsigned requested = 0;  // 100-ns units
    unsigned loopN = 0;
    double loopMedian = 0, loopMean = 0, loopStdev = 0, loopLate = 0, loopScore = 0;
    unsigned idleN = 0;
    double idleMedian = 0, idleMean = 0, idleStdev = 0, idleLate = 0, idleScore = 0;
    double score = 0;
};

struct AggregatePoint {
    unsigned requested = 0;
    unsigned runs = 0;
    double loopWeight = 0;
    double idleWeight = 0;
    double scoreWeight = 0;
    double loopMedian = 0, loopMean = 0, loopStdev = 0, loopLate = 0, loopScore = 0;
    double idleMedian = 0, idleMean = 0, idleStdev = 0, idleLate = 0, idleScore = 0;
    double score = 0;
};

struct TuneEvent {
    std::wstring when;
    std::wstring kind;
    std::wstring detail;
};

struct LatestPoint {
    unsigned requested = 0;
    double score = 0;
};

struct AggregateView {
    std::vector<AggregatePoint> points;
    std::vector<TuneEvent> events;    // newest first
    std::vector<LatestPoint> latest;  // newest saved scan, for comparison
    unsigned runs = 0;
    double samples = 0;
};

// Folds one finished measurement into the master file. Samples that share a
// requested interval are combined first, then counted as one run.
void MergeTimingRun(const std::wstring& dataDir, const std::vector<TimingSample>& samples);

void NoteTuneEvent(const std::wstring& dataDir, const std::wstring& kind, const std::wstring& detail);

AggregateView LoadAggregate(const std::wstring& dataDir);

// 0 when the weighted-mean cases hold.
int SelfTestAggregate();
