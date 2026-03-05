/*
 * HCOP Switch — Main Entry Point.
 */

#include "hcop_switch.h"

#include <getopt.h>
#include <unistd.h>
#include <csignal>
#include <iostream>
#include <vector>

// Global netParams (referenced by NetPort)
struct SimbricksBaseIfParams netParams;

static hcop_switch::HcopSwitch *g_switch = nullptr;
static void sig_handler(int) {
    if (g_switch) g_switch->Stop();
}

int main(int argc, char *argv[]) {
    std::string config_path = "switch_config.json";
    std::vector<std::pair<std::string, bool>> port_defs; // path, is_listen
    int sync_mode = 1;
    
    int c;
    while ((c = getopt(argc, argv, "c:s:h:uS:E:p:m:")) != -1) {
        switch (c) {
            case 'c': config_path = optarg; break;
            case 's': port_defs.push_back({optarg, false}); break;
            case 'h': port_defs.push_back({optarg, true}); break;
            case 'u': sync_mode = 0; break;
            case 'S':
            case 'E':
            case 'p':
            case 'm': break; // ignore
        }
    }

    // Load config
    hcop_switch::SwitchConfig cfg;
    try {
        cfg = hcop_switch::SwitchConfig::Load(config_path);
    } catch (const std::exception &e) {
        fprintf(stderr, "Error loading config: %s\n", e.what());
        // Check if config file exists? If not, maybe use defaults?
        // But for behavioral model, defaults are okay. 
        // We catch exception and proceed? No, if explicit file fails, exit.
        // If minimal args, proceed.
        // SwitchConfig::Load creates default if file missing? No, throws.
        // Let's assume user must provide valid config if -c is used?
        // If Load fails, we exit.
        return 1;
    }

    hcop_switch::HcopSwitch dev(cfg);
    g_switch = &dev;
    
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    
    // Create ports
    std::vector<hcop_switch::NetPort*> ports;
    struct SimbricksProtoNetIntro intro; // shared intro
    
    for (const auto &p : port_defs) {
        hcop_switch::NetPort *np;
        if (p.second) {
            np = new hcop_switch::NetListenPort(p.first.c_str(), sync_mode);
        } else {
            np = new hcop_switch::NetPort(p.first.c_str(), sync_mode);
        }
        ports.push_back(np);
        dev.AddPort(np);
    }

    if (ports.empty()) {
        fprintf(stderr, "Usage: hcop_switch -c config.json -s socket ...\n");
        return 1;
    }
    
    // Connect
    size_t n = ports.size();
    std::vector<struct SimBricksBaseIfEstablishData> ests(n);
    for (size_t i=0; i<n; ++i) {
        ests[i].base_if = &ports[i]->netif_.base;
        ests[i].tx_intro = &intro;
        ests[i].tx_intro_len = sizeof(intro);
        ests[i].rx_intro = &intro;
        ests[i].rx_intro_len = sizeof(intro);
        
        if (!ports[i]->Prepare()) return 1;
    }
    
    if (SimBricksBaseIfEstablish(ests.data(), n)) {
        fprintf(stderr, "SimBricksBaseIfEstablish failed\n");
        return 1;
    }
    
    for (auto *p : ports) p->Prepared();

    dev.Run();

    return 0;
}
