import struct
import random

# Format: char type, uint64 order_id, uint8 side (0=Buy, 1=Sell), uint32 price, uint32 qty
RECORD_FORMAT = "=1sQBII" # 18 bytes, = for no padding

def generate_million_row_stress_test():
    print("[Generator] Manufacturing 1 million rows with engineered edge cases...")
    
    records = []
    # Use a list for O(1) random choice selection
    active_ids = [] 
    next_id = 1

    # 1. Seed initial tight liquidity book spread
    for i in range(10000):
        records.append((b'L', next_id, 0, random.randint(90, 99), random.randint(1, 100)))
        active_ids.append(next_id)
        next_id += 1
        
        records.append((b'L', next_id, 1, random.randint(101, 110), random.randint(1, 100)))
        active_ids.append(next_id)
        next_id += 1

    # 2. Loop high volume matching and adversarial actions
    for _ in range(980000):
        action = random.choice([b'L', b'M', b'C'])
        side = random.choice([0, 1])
        
        if action == b'L': # Limit order placement
            price = random.choice([random.randint(1, 5), random.randint(95, 105), random.randint(1000, 5000)])
            records.append((b'L', next_id, side, price, random.randint(1, 150)))
            active_ids.append(next_id)
            next_id += 1
            
        elif action == b'M': # Market Order Taker Sweep
            records.append((b'M', 0, side, 0, random.randint(5, 50)))
            
        elif action == b'C' and active_ids: # Cancellation request
            # Fast O(1) random choice lookup
            target_idx = random.randint(0, len(active_ids) - 1)
            target_id = active_ids[target_idx]
            records.append((b'C', target_id, side, 0, 0))
            
            # O(1) Swap-and-Pop deletion to avoid expensive array shifting
            active_ids[target_idx] = active_ids[-1]
            active_ids.pop()

    # Write out memory-mapped compatible binary structure
    with open("data/market_data.bin", "wb") as f:
        for rec in records:
            f.write(struct.pack(RECORD_FORMAT, *rec))
            
    print(f"[Generator] Successfully exported {len(records)} packed records to data/market_data.bin")

if __name__ == "__main__":
    generate_million_row_stress_test()