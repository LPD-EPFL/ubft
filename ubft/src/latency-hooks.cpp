#include "latency-hooks.hpp"

hooks::Timepoint hooks::smr_start;
LatencyProfiler hooks::smr_latency(10000);

hooks::Timepoint hooks::swmr_read_start;
LatencyProfiler hooks::swmr_read_latency(10000);

hooks::Timepoint hooks::swmr_write_start;
LatencyProfiler hooks::swmr_write_latency(10000);

hooks::Timepoint hooks::sig_computation_start;
LatencyProfiler hooks::sig_computation_latency(10000);

hooks::Timepoint hooks::sig_check_start;
LatencyProfiler hooks::sig_check_latency(10000);

hooks::Timepoint hooks::tcb_sp_start;
LatencyProfiler hooks::tcb_sp_latency(10000);