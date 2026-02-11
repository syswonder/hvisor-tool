import re
import json
import os

def generate_rk3588_mapping(input_header):
    c_header_out = "rk3588_scmi_reset_map.h"
    json_data_out = "rk3588_reset_mapping_data.json"
    
    # Pattern to match: #define SRST_XXXX  NUMBER (handling both tabs and spaces)
    pattern = re.compile(r'^#define\s+(SRST_[A-Za-z0-9_]+)\s+([0-9]+)')
    
    raw_resets = []
    line_count = 0

    print(f"--- Starting RK3588 Metadata Extraction ---")
    if not os.path.exists(input_header):
        print(f"Error: Source file '{input_header}' not found!")
        return

    with open(input_header, 'r') as f:
        for line in f:
            line_count += 1
            match = pattern.search(line.strip())
            if match:
                name = match.group(1)
                phys_id = int(match.group(2))
                raw_resets.append((phys_id, name))

    print(f"Total lines scanned: {line_count}")
    print(f"Raw SRST macros found: {len(raw_resets)}")

    # Sort strictly by Physical ID to ensure deterministic SCMI Domain IDs
    raw_resets.sort(key=lambda x: x[0])

    unique_resets = []
    phys_to_virt = {}
    virt_counter = 0

    print("\n--- Generating SCMI Domain Mapping Table ---")
    print(f"{'Virt ID':<10} | {'Phys ID':<10} | {'Original Macros (Aliases)'}")
    print("-" * 60)

    for phys_id, name in raw_resets:
        if phys_id not in phys_to_virt:
            phys_to_virt[phys_id] = virt_counter
            unique_resets.append({"virt": virt_counter, "phys": phys_id, "names": [name]})
            
            # Print mapping for every domain (useful for your thesis logs)
            print(f"{virt_counter:<10} | {phys_id:<10} | {name}")
            virt_counter += 1
        else:
            # Handle Alias (Same hardware bit defined with different names)
            idx = phys_to_virt[phys_id]
            unique_resets[idx]["names"].append(name)
            print(f"{'':<10} | {'':<10} | [Alias] -> {name}")

    # Generate C Header for SCMI Server
    with open(c_header_out, "w") as f:
        f.write(f"/* Automatically generated SCMI Reset mapping for RK3588 */\n")
        f.write(f"/* Source: {input_header} */\n")
        f.write(f"#ifndef _RK3588_SCMI_RESET_MAP_H\n#define _RK3588_SCMI_RESET_MAP_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define RK3588_SCMI_RESET_NUM_DOMAINS {len(unique_resets)}\n\n")
        f.write("static const uint32_t rk3588_scmi_to_phys_map[] = {\n")
        for item in unique_resets:
            alias_info = "/".join(item["names"])
            f.write(f"    {item['phys']}, /* Virt ID {item['virt']}: {alias_info} */\n")
        f.write("};\n\n")
        f.write("#endif /* _RK3588_SCMI_RESET_MAP_H */\n")

    # Generate JSON for hvisor-tool
    mapping_data = {
        "platform": "rk3588",
        "total_domains": len(unique_resets),
        "phys_to_virt": phys_to_virt,
        "domain_details": unique_resets
    }
    with open(json_data_out, "w") as f:
        json.dump(mapping_data, f, indent=4)

    print("-" * 60)
    print(f"Success: Mapping generation completed.")
    print(f"Effective SCMI Domains: {len(unique_resets)}")
    print(f"Output 1 (C Header): {c_header_out}")
    print(f"Output 2 (JSON Metadata): {json_data_out}")
    print(f"--- Process Finished ---\n")

if __name__ == "__main__":
    generate_rk3588_mapping("rk3588-cru.h")