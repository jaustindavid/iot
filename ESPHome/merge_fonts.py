import os
import re

def parse_bdf(filepath):
    with open(filepath, 'r') as f:
        lines = f.readlines()
    
    chars = {}
    header = []
    footer = []
    in_char = False
    current_char = []
    current_encoding = None
    
    for line in lines:
        line = line.strip()
        if not line:
            continue
        
        # Strip comments
        if "#" in line and not line.startswith("STARTCHAR"):
            line = line.split("#")[0].strip()
            
        if line.startswith("STARTCHAR"):
            in_char = True
            current_char = [line]
        elif line.startswith("ENDCHAR"):
            current_char.append(line)
            if current_encoding is not None:
                chars[current_encoding] = current_char
            in_char = False
            current_encoding = None
        elif in_char:
            current_char.append(line)
            if line.startswith("ENCODING"):
                try:
                    current_encoding = int(line.split()[1])
                except:
                    current_encoding = -1
        elif line == "ENDFONT":
            footer = [line]
        else:
            if not in_char:
                header.append(line)
                
    return header, chars, footer

def merge_fonts():
    _, mega_chars, _ = parse_bdf("megafont.bdf")
    chun_header, chun_chars, chun_footer = parse_bdf("MatrixChunky8.bdf")
    
    # Check what offsets the numbers usually have in the chunky font
    chun_zero = chun_chars.get(48, [])
    # Find BBX in chunky to see if we should adjust Y offset
    for line in chun_zero:
        pass # Actually we'll just trust megafont's author intent directly!

    # merge 0-9 and :
    for enc in range(48, 59):
        if enc in mega_chars:
            # We want to replace the character, but we probably should
            # keep the character's header from MEGA if it provides it!
            chun_chars[enc] = mega_chars[enc]
    
    # We must update the CHARS count in the header just to be safe
    new_header = []
    for line in chun_header:
        if line.startswith("CHARS"):
            new_header.append(f"CHARS {len(chun_chars)}")
        else:
            new_header.append(line)
            
    # Modify font name
    for i, line in enumerate(new_header):
        if line.startswith("FONT "):
            new_header[i] = "FONT -Custom-Mixed-Font"
            
    with open("CustomMerged.bdf", "w") as f:
        for line in new_header:
            f.write(line + "\n")
            
        for enc in sorted(chun_chars.keys()):
            for line in chun_chars[enc]:
                f.write(line + "\n")
                
        for line in chun_footer:
            if line:
                f.write(line + "\n")

if __name__ == "__main__":
    merge_fonts()
