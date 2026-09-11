"""Turn a .tap into a C header the firmware can play straight from flash.

Also walks the tape's block structure, so a file that is not a tape is
caught here rather than on the device.
"""
import sys

TYPES = {0: "Program", 1: "Number array", 2: "Character array", 3: "Code"}


def describe(data):
    pos = 0
    blocks = 0
    while pos + 2 <= len(data):
        length = data[pos] | (data[pos + 1] << 8)
        pos += 2
        if length == 0 or pos + length > len(data):
            print("  truncated block at offset {}".format(pos))
            return False
        flag = data[pos]
        if flag == 0x00 and length == 19:
            block_type = data[pos + 1]
            name = bytes(data[pos + 2:pos + 12]).decode("latin-1").rstrip()
            size = data[pos + 12] | (data[pos + 13] << 8)
            param = data[pos + 14] | (data[pos + 15] << 8)
            print("  header  {:<14} {:<16} {} bytes, param {}".format(
                TYPES.get(block_type, "?" + str(block_type)), repr(name), size, param))
        else:
            print("  data    flag 0x{:02X}, {} bytes".format(flag, length - 2))
        pos += length
        blocks += 1
    print("  {} blocks, {} bytes total".format(blocks, len(data)))
    return blocks > 0


def emit(path, data, name, title):
    lines = [
        "/*",
        " * Built-in tape: {}".format(title),
        " *",
        " * Played straight out of flash by the tape loader, so the",
        " * emulator has something to run with no card in the slot.",
        " */",
        "#pragma once",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "static const char {}_title[] = \"{}\";".format(name, title),
        "",
        "static const uint8_t {}[] = {{".format(name),
    ]
    for start in range(0, len(data), 16):
        chunk = data[start:start + 16]
        lines.append("    " + " ".join("0x{:02X},".format(b) for b in chunk))
    lines.append("};")
    lines.append("")
    lines.append("static const size_t {}_size = sizeof({});".format(name, name))
    lines.append("")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))
    print("wrote {} ({} bytes of tape)".format(path, len(data)))


if __name__ == "__main__":
    source, target, symbol, title = sys.argv[1:5]
    with open(source, "rb") as handle:
        payload = handle.read()
    print("{}: {} bytes".format(source, len(payload)))
    if not describe(payload):
        sys.exit("not a usable tape")
    emit(target, payload, symbol, title)
