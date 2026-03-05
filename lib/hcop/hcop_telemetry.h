#ifndef SIMBRICKS_HCOP_TELEMETRY_H_
#define SIMBRICKS_HCOP_TELEMETRY_H_

#include <string>
#include <time.h>
#include <map>
#include <fstream>
#include <mutex>
#include <iostream>

#include "hcop_proto.h"

namespace hcop {

inline uint64_t GetTimeNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

inline std::string DecodeTierPath(uint32_t path, uint8_t crossings) {
    if (crossings == 0) return "Unknown";
    std::string res = "";
    for (int i = crossings - 1; i >= 0; --i) {
        uint8_t nibble = (path >> (i * 4)) & 0x0F;
        if (nibble == 0) res += "S";
        else if (nibble == 1) res += "D";
        else if (nibble == 2) res += "H";
        else res += "?";
        
        if (i > 0) res += "->";
    }
    return res;
}

class TelemetryLogger {
public:
    static TelemetryLogger& Get() {
        static TelemetryLogger instance;
        return instance;
    }

    void LogOperation(uint32_t op_id, uint16_t prim_type, uint64_t start_time, uint64_t end_time,
                      uint32_t tier_path, uint8_t crossings, uint16_t exception_type) {
        std::lock_guard<std::mutex> lock(mu_);
        auto mode = init_ops_ ? std::ios::app : (std::ios::out | std::ios::trunc);
        std::ofstream ofs("/tmp/hcop_operations.csv", mode);
        init_ops_ = true;
        
        // Write header if file is empty
        ofs.seekp(0, std::ios::end);
        if (ofs.tellp() == 0) {
            ofs << "operation_id,primitive_type,placement_config,start_time_ns,end_time_ns,latency_ns,tier_path,num_tier_crossings,was_exception,exception_type\n";
        }
        
        std::string placement = getenv("HCOP_PLACEMENT") ? getenv("HCOP_PLACEMENT") : "unknown";
        
        uint64_t latency = end_time > start_time ? end_time - start_time : 0;
        bool was_exc = exception_type != 0;
        
        ofs << op_id << "," << prim_type << "," << placement << "," 
            << start_time << "," << end_time << "," << latency << ","
            << DecodeTierPath(tier_path, crossings) << "," << (int)crossings << ","
            << (was_exc ? 1 : 0) << "," << exception_type << "\n";
    }

    void LogUtilization(uint64_t ts_ns, SourceTier tier, float core_util_pct,
                        uint64_t mem_used, uint64_t mem_cap,
                        uint64_t pkts_proc, uint64_t pkts_queued, uint64_t pkts_drop) {
        std::lock_guard<std::mutex> lock(mu_);
        const char *filepath = "";
        const char *tier_str = "";
        
        if (tier == kTierSwitch) {
            filepath = "/tmp/hcop_utilization_switch.csv";
            tier_str = "S";
        } else if (tier == kTierDpu) {
            filepath = "/tmp/hcop_utilization_dpu.csv";
            tier_str = "D";
        } else if (tier == kTierHost) {
            filepath = "/tmp/hcop_utilization_host.csv";
            tier_str = "H";
        } else {
            return;
        }

        auto mode = std::ios::app;
        if (tier == kTierSwitch) {
            mode = init_switch_ ? std::ios::app : (std::ios::out | std::ios::trunc);
            init_switch_ = true;
        } else if (tier == kTierDpu) {
            mode = init_dpu_ ? std::ios::app : (std::ios::out | std::ios::trunc);
            init_dpu_ = true;
        } else if (tier == kTierHost) {
            mode = init_host_ ? std::ios::app : (std::ios::out | std::ios::trunc);
            init_host_ = true;
        }

        std::ofstream ofs(filepath, mode);
        
        ofs.seekp(0, std::ios::end);
        if (ofs.tellp() == 0) {
            ofs << "timestamp_ns,tier,core_utilization_pct,memory_used_bytes,memory_capacity_bytes,packets_processed,packets_queued,packets_dropped\n";
        }
        
        ofs << ts_ns << "," << tier_str << "," << core_util_pct << ","
            << mem_used << "," << mem_cap << "," << pkts_proc << ","
            << pkts_queued << "," << pkts_drop << "\n";
    }

private:
    std::mutex mu_;
    bool init_ops_ = false;
    bool init_dpu_ = false;
    bool init_switch_ = false;
    bool init_host_ = false;
    TelemetryLogger() {}
};

} // namespace hcop

#endif // SIMBRICKS_HCOP_TELEMETRY_H_
