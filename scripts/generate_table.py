import os
import pandas as pd

def generate_markdown_table():
    master_csv = "results/master_summary.csv"
    if not os.path.exists(master_csv):
        print("master_summary.csv not found!")
        return
        
    master_df = pd.read_csv(master_csv)
    
    primitives = ["paxos", "lock"]
    placements = ["switch_only", "dpu_only", "switch_host", "switch_dpu", "dpu_host"]
    
    for primitive in primitives:
        print(f"{primitive.upper()}:")
        print(f"{'Placement':<12} | {'Low tput':<8} | {'High tput':<9} | {'Low P50':<9} | {'High P50':<9}")
        print("-" * 59)
        
        for placement in placements:
            low_load_row = master_df[(master_df['primitive_type'] == primitive) & (master_df['placement_config'] == placement)]
            if not low_load_row.empty:
                ll_tput = f"{low_load_row.iloc[0]['throughput_ops_per_sec']:.1f}"
                try:
                    ll_p50 = f"{(int(low_load_row.iloc[0]['latency_p50_ns']) / 1000.0):.1f}"
                except ValueError:
                    ll_p50 = "nan"
            else:
                ll_tput = "N/A"
                ll_p50 = "N/A"
                
            high_load_file = f"results/{primitive}_highload/{primitive}/{placement}/summary.csv"
            if os.path.exists(high_load_file):
                hl_df = pd.read_csv(high_load_file)
                if not hl_df.empty:
                    hl_tput_val = hl_df.iloc[0]['throughput_ops_per_sec']
                    if pd.isna(hl_tput_val) or str(hl_tput_val).upper() == 'N/A' or str(hl_tput_val) == 'nan':
                        hl_tput = "N/A"
                    else:
                        hl_tput_val = float(hl_tput_val)
                        if hl_tput_val == 0.0:
                             hl_tput = "0.0"
                        else:
                             hl_tput = f"{hl_tput_val:.1f}"
                    
                    hl_p50_val = hl_df.iloc[0]['latency_p50_ns']
                    if pd.isna(hl_p50_val) or str(hl_p50_val).upper() == 'N/A' or str(hl_p50_val) == 'nan':
                        hl_p50 = "N/A"
                    else:
                        hl_p50 = f"{(float(hl_p50_val) / 1000.0):.1f}"
                else:
                    hl_tput = "N/A"
                    hl_p50 = "N/A"
            else:
                hl_tput = "N/A"
                hl_p50 = "N/A"
                
            print(f"{placement:<12} | {ll_tput:<8} | {hl_tput:<9} | {ll_p50:<9} | {hl_p50:<9}")
        print("\n")

if __name__ == "__main__":
    generate_markdown_table()
