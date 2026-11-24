"""Copy upstream into the build tree and select the diagnostic native I/O backend."""
import shutil
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
destination = Path(sys.argv[1]).resolve()
upstream = here.parents[1] / 'research/FASTER/cc/src'
shutil.copytree(upstream, destination, dirs_exist_ok=True)
platform_header = destination / 'environment/file.h'
old = '#else\n#include "file_linux.h"'
text = platform_header.read_text()
assert text.count(old) == 1
platform_header.write_text(text.replace(old, '#elif defined(__APPLE__)\n#include "native_file.h"\n#else\n#include "file_linux.h"'))
address_header = destination / 'core/address.h'
address_text = address_header.read_text()
old_offset_bits = 'static constexpr uint64_t kOffsetBits = 25;'
assert address_text.count(old_offset_bits) == 1
address_header.write_text(address_text.replace(old_offset_bits, 'static constexpr uint64_t kOffsetBits = 20;'))
shutil.copyfile(here / 'native_file.h', destination / 'environment/native_file.h')
