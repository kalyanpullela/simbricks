import csv
with open('/tmp/hcop_operations.csv', 'w') as f:
    writer = csv.writer(f)
    writer.writerow(['operation_id','primitive_type','placement_config','start_time_ns','end_time_ns','latency_ns','tier_path','num_tier_crossings','was_exception','exception_type'])
    for i in range(10):
        writer.writerow([i, 'paxos', '3.4b', i*100000000, i*100000000 + 10000, 10000, 'S', 1, 0, 0])
