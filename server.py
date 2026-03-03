from flask import Flask, request
import numpy as np
import pandas as pd
import os
import glob
from datetime import datetime

app = Flask(__name__)

# --- CONFIGURATION (Must match ESP32 exactly) ---
MAX_LOWER = 4
MAX_UPPER = 60
DC_NULL = 32
# Note: BATCH_SIZE is only for validation of the FULL batch. 
# On server, we care about SUB_BATCH_SIZE (e.g., 40)
SUB_BATCH_SIZE = 40 
SAVE_DIR = "csi_data"

# Calculate exactly how many bytes per packet based on the exclusion
CSI_HEADERS = [f"SC_{i}" for i in range(MAX_LOWER, MAX_UPPER) if i != DC_NULL]
SUB_COUNT = len(CSI_HEADERS) 

@app.route('/upload_data', methods=['POST'])
def upload_data():
    room_state = request.headers.get('X-Room-State', 'unknown')
    esp32_id = request.headers.get('X-ESP32-ID', '0')
    sub_batch_idx = int(request.headers.get('X-Sub-Batch-Index', -1))
    total_sub_batches = int(request.headers.get('X-Total-Sub-Batches', 0))
    
    if sub_batch_idx < 0 or total_sub_batches <= 0:
        return "Invalid Headers", 400
        
    raw_data = request.get_data()
    
    # Validation: The ESP32 sends a buffer of SUB_BATCH_SIZE
    expected_size = SUB_BATCH_SIZE * SUB_COUNT
    
    if len(raw_data) != expected_size:
        print(f"DATA MISMATCH: Received {len(raw_data)}, expected {expected_size}")
        return "Wrong Size", 400

    # Reshape binary data to DataFrame
    csi_matrix = np.frombuffer(raw_data, dtype=np.uint8).reshape(SUB_BATCH_SIZE, SUB_COUNT)
    df = pd.DataFrame(csi_matrix, columns=CSI_HEADERS)
    
    # Path setup: Use a temporary sub-directory for the current session to handle out-of-order parts
    session_dir = os.path.join(SAVE_DIR, esp32_id, room_state, "temp_session")
    if not os.path.exists(session_dir):
        os.makedirs(session_dir)
    
    # Save each sub-batch as its own individual file named by its index (e.g., part_000.csv)
    # This allows requests to arrive in any order (out-of-sequence)
    part_filename = os.path.join(session_dir, f"part_{sub_batch_idx:03d}.csv")
    df.to_csv(part_filename, index=False)
    
    # Check how many parts we have collected so far
    existing_parts = glob.glob(os.path.join(session_dir, "part_*.csv"))
    print(f"[{esp32_id}-{room_state}] Part {sub_batch_idx + 1}/{total_sub_batches} received (Current: {len(existing_parts)})")

    # Only merge when ALL parts have arrived
    if len(existing_parts) == total_sub_batches:
        print(f"FULL BATCH RECEIVED: Merging {total_sub_batches} parts...")
        
        # Sort files by name to ensure sequence (part_000, part_001, etc)
        existing_parts.sort()
        
        # Merge all parts into one large dataframe
        full_df_list = [pd.read_csv(f) for f in existing_parts]
        combined_df = pd.concat(full_df_list, ignore_index=True)
        
        # Save final combined CSV with timestamp
        timestamp = datetime.now().strftime('%H%M%S')
        final_filename = os.path.join(SAVE_DIR, esp32_id, room_state, f"csi_{room_state}_{timestamp}.csv")
        combined_df.to_csv(final_filename, index=False)
        
        # Cleanup: Remove temporary parts and their directory
        for f in existing_parts:
            os.remove(f)
        try:
            os.rmdir(session_dir)
        except OSError:
            pass # Directory might not be empty if another batch started simultaneously
        
        print(f"SAVED: {final_filename} with {len(combined_df)} rows.")
    
    return "OK", 200

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=True)
