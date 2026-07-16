import struct
import random
from pathlib import Path

# Format: uint64 order_id, uint32 price, uint32 qty, uint8 side (0=Buy, 1=Sell), char type
RECORD_FORMAT = "=QIIB1s" # 18 bytes, = for no padding
OUTPUT_FILE = Path(__file__).resolve().parent / "market_data.bin"

def generate_million_row_stress_test():
    print("[Generator] Manufacturing 1 million rows with engineered edge cases...")
    
    records = []
    # Use a list for O(1) random choice selection
    active_ids = [] 
    next_id = 1

    # 1. Seed initial tight liquidity book spread
    for i in range(10000):
        records.append((next_id, random.randint(90, 99), random.randint(1, 100), 0, b'L'))
        active_ids.append(next_id)
        next_id += 1
        
        records.append((next_id, random.randint(101, 110), random.randint(1, 100), 1, b'L'))
        active_ids.append(next_id)
        next_id += 1

    # 2. Loop high volume matching and adversarial actions
    for _ in range(980000):
        action = random.choice([b'L', b'M', b'C'])
        side = random.choice([0, 1])
        
        if action == b'L': # Limit order placement
            price = random.choice([random.randint(1, 5), random.randint(95, 105), random.randint(1000, 5000)])
            records.append((next_id, price, random.randint(1, 150), side, b'L'))
            active_ids.append(next_id)
            next_id += 1
            
        elif action == b'M': # Market Order Taker Sweep
            records.append((0, 0, random.randint(5, 50), side, b'M'))
            
        elif action == b'C' and active_ids: # Cancellation request
            # Fast O(1) random choice lookup
            target_idx = random.randint(0, len(active_ids) - 1)
            target_id = active_ids[target_idx]
            records.append((target_id, 0, 0, side, b'C'))
            
            # O(1) Swap-and-Pop deletion to avoid expensive array shifting
            active_ids[target_idx] = active_ids[-1]
            active_ids.pop()

    # Write out memory-mapped compatible binary structure
    with open(OUTPUT_FILE, "wb") as f:
        for rec in records:
            f.write(struct.pack(RECORD_FORMAT, *rec))
    
    print(f"[Generator] Successfully exported {len(records)} packed records to {OUTPUT_FILE}")

if __name__ == "__main__":
    generate_million_row_stress_test()