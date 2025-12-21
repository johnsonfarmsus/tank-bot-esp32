#!/usr/bin/env python3
"""Convert PEM certificates to C header format for ESP32"""

def pem_to_c_array(input_file, output_file, var_name):
    with open(input_file, 'r') as f:
        pem_content = f.read()

    with open(output_file, 'w') as f:
        f.write(f'const char {var_name}[] PROGMEM = R"EOF(\n')
        f.write(pem_content)
        f.write(')EOF";\n')

    print(f"Converted {input_file} to {output_file}")

if __name__ == "__main__":
    pem_to_c_array('certs/cert.pem', 'src/cert.h', 'SSL_CERT')
    pem_to_c_array('certs/key.pem', 'src/key.h', 'SSL_KEY')
    print("Certificate conversion complete!")
